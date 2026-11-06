# xbox_kernel — Xbox Kernel Replacement Layer

Replaces all 371 Xbox kernel exports (347 function ordinals + 24 data exports) with Win32 equivalents. This is the foundation layer that every recompiled game needs — it provides memory layout, file I/O, threading, synchronization, and hardware abstraction.

All 371 ordinals are routed: every export now has either a per-ordinal `bridge_*` wrapper (real implementation or a documented stub) or a kernel data export entry. Verified by `tools/kernel_audit/test_bridge_ordinals.py`, which cross-checks routing, argument sizes, and data exports against the reference export table.

## Files

| File | LOC | Purpose |
|------|-----|---------|
| `kernel.h` | 829 | Master public header — all types, constants, and function prototypes |
| `xbox_memory_layout.h` | 204 | Memory layout constants and API |
| `xbox_memory_layout.c` | 534 | Project guest mapping integration |
| `kernel_bridge.c` | 8,390 | Core dispatcher — thunk table, ICALL resolution, all 371 per-ordinal bridges, kernel data exports |
| `kernel_file.c` | 834 | File I/O — NtCreateFile, NtReadFile, path translation |
| `kernel_thread.c` | 344 | Threading — PsCreateSystemThreadEx, thread priorities |
| `kernel_sync.c` | 547 | Synchronization — events, semaphores, mutexes, critical sections |
| `kernel_memory.c` | 323 | Memory allocation — MmAllocateContiguousMemory, page alloc |
| `kernel_rtl.c` | 334 | Runtime library — string ops, memory copy, ANSI/Unicode conversion |
| `kernel_hal.c` | 453 | Hardware abstraction — PCI, timers, DPC, IRQLs |
| `kernel_crypto.c` | 409 | Cryptography — SHA1, RC4, HMAC |
| `kernel_thunks.c` | 414 | Thunk layer for recompiled code → kernel dispatch |
| `kernel_ob.c` | 126 | Object manager — handle table, reference counting |
| `kernel_io.c` | 242 | Device I/O control |
| `kernel_path.c` | 199 | Xbox path parsing (`\Device\CdRom0\` → Windows path) |
| `kernel_pool.c` | 49 | Pool memory allocator |
| `kernel_xbox.c` | 121 | Misc Xbox APIs (XeLoadSection, etc.) |

## Memory layout integration

The runtime delegates construction to project `guest_map_layout`, shared backing
ownership to `shared_views`, and native arena/MMIO placement to `owned_arena` and
`guest_arena_service`. These providers live in the project's `src/runtime/`,
with OS adapters in `src/native/`. Windows uses real host sections; native Mac
uses a reserved arena and shared backing views. Generic POSIX emulation of the
Windows file-mapping API is no longer part of this runtime.

The supported title uses 64 MiB RAM by default. Optional map-size configuration
does not redefine reported RAM. Up to 28 RAM mirrors alias the main RAM backing;
physical storage has a separate backing with tiled/high aliases when available.
The tiled extent reports only the mapped portion. Image sections are validated
from the supported XBE before loading; callers do not hard-code section layouts
or change headers to admit another title. Broader title compatibility is not
established by these interfaces.

Initialization publishes the layout before workers can use it. Unpublished
failure cleanup withdraws public pointers and retains any ownership whose
release failed. Published maps remain through process exit until whole-runtime
worker quiescence can be established. Bounded ack-worker waits do not establish
that broader condition. Guest allocation and thread-storage providers retain
their separate ownership and ABI contracts.

Use `xbox_GetMemoryBase` / `xbox_GetMemoryOffset` for host translation,
`xbox_GetMappedSize` for the primary map, and `xbox_TiledApertureSize` for the
actual tiled extent. Guest heap/free, reservations, Mm/Nt calls and thread
construction route to project services. An arbitrary host pointer or an address
in a different alias is not an allocation ownership token.

The maintained project guide is `docs/agent-context/guest-host-mappings.md`.
Measured build/game results belong in `docs/project/STATUS.md`; Windows gameplay
does not establish native Mac title support. The original toolkit snapshot is
still part of preparation until the broader independence work is complete.

## Kernel Thunk Table

The Xbox kernel exposes functions via ordinal numbers. Games import them through
a thunk table. **Both the address and the length of that table are per-title** —
they come from the XBE header's `KernelImageThunkAddress`, not from a constant.

`xbox_MemoryLayoutInit()` decodes that field (trying the retail and debug XOR
keys and keeping whichever lands inside the image), counts the entries, and
hands both to the kernel layer:

```c
void     xbox_kernel_set_thunk_address(uint32_t xbox_va, uint32_t count);
uint32_t xbox_kernel_get_thunk_address(void);
uint32_t xbox_kernel_get_thunk_count(void);
```

`XBOX_KERNEL_THUNK_TABLE_SIZE` (366) is the capacity bound of the fixed slot
arrays — the size of the Xbox kernel's ordinal space — **not** an entry count.
Never iterate it against a title's table: past the real end you are reading
adjacent `.rdata`, and any word there with bit 31 set looks exactly like an
unresolved ordinal and would be overwritten.

Entry values in the on-disc XBE are `0x80000000 | ordinal`. The engine reads
those ordinals out of the loaded image; it does not carry a per-game list.

### Initialization

Order matters — the thunk address is only known after the XBE is mapped:

```c
xbox_MemoryLayoutInit(xbe, size);  // Maps the XBE, discovers the thunk table
xbox_kernel_init();                // Resolves ordinals read from the mapped image
xbox_kernel_bridge_init();         // Patches thunk entries, sets up kernel data area
```

### Kernel Data Exports

Some kernel ordinals are data exports (not functions). We allocate a kernel data area:

```c
#define XBOX_KERNEL_DATA_BASE  0x00740000

// Offsets within the data area
#define KDATA_HARDWARE_INFO     0x000  // XBOX_HARDWARE_INFO struct
#define KDATA_KRNL_VERSION      0x010  // XBOX_KRNL_VERSION struct
#define KDATA_TICK_COUNT        0x020  // KeTickCount (updated by timer)
#define KDATA_HD_KEY            0x030  // XboxHDKey (16 bytes)
#define KDATA_EEPROM_KEY        0x050  // XboxEEPROMKey (16 bytes)
// ... more at documented offsets
```

## File I/O

Xbox file I/O uses NT-style APIs (NtCreateFile, NtReadFile). We translate Xbox paths to Windows:

```
Xbox Path                    → Windows Path
───────────────────────────────────────────────
\Device\CdRom0\Data\foo.dat → <game_dir>\Data\foo.dat
D:\Data\foo.dat              → <game_dir>\Data\foo.dat
T:\save.dat                  → <save_dir>\save.dat
Z:\cache.dat                 → <save_dir>\cache\cache.dat
```

### Path Setup

```c
// Call early — before game tries to open files
xbox_path_init("Burnout 3 Takedown", "saves");
```

## Threading

Xbox threads use `PsCreateSystemThreadEx` with NT-style parameters. We create Win32 threads with a wrapper that sets up the register context:

```c
// The kernel bridge creates a thread that:
// 1. Looks up the start routine via recomp_lookup()
// 2. Sets up ESP for the new thread
// 3. Calls the recompiled function
NTSTATUS xbox_PsCreateSystemThreadEx(
    PHANDLE ThreadHandle,
    ULONG   ThreadExtraSize,
    ULONG   KernelStackSize,
    ULONG   TlsDataSize,
    PULONG  ThreadId,
    PVOID   StartContext1,
    PVOID   StartContext2,
    BOOLEAN CreateSuspended,
    BOOLEAN DebuggerThread,
    PVOID   StartRoutine        // Xbox VA → resolved via recomp_lookup()
);
```

## Synchronization

Full implementations of:

| Xbox API | Win32 Implementation |
|----------|---------------------|
| NtCreateEvent | CreateEvent |
| NtSetEvent / KeSetEvent | SetEvent |
| NtWaitForSingleObject | WaitForSingleObject |
| NtWaitForMultipleObjectsEx | WaitForMultipleObjects |
| NtCreateSemaphore | CreateSemaphore |
| NtReleaseSemaphore | ReleaseSemaphore |
| RtlInitializeCriticalSection | InitializeCriticalSection |
| RtlEnterCriticalSection | EnterCriticalSection |
| RtlLeaveCriticalSection | LeaveCriticalSection |

## NTSTATUS Codes

```c
#define STATUS_SUCCESS                0x00000000
#define STATUS_TIMEOUT                0x00000102
#define STATUS_PENDING                0x00000103
#define STATUS_NO_MEMORY              0xC0000017
#define STATUS_ACCESS_DENIED          0xC0000022
#define STATUS_OBJECT_NAME_NOT_FOUND  0xC0000034
#define STATUS_OBJECT_NAME_COLLISION  0xC0000035
// ... full list in kernel.h

#define NT_SUCCESS(Status) ((NTSTATUS)(Status) >= 0)
```

## Calling Conventions

| Convention | Used By | Example |
|-----------|---------|---------|
| `__stdcall` | Most kernel functions | `xbox_NtCreateFile(...)` |
| `__fastcall` | Kf* and ObfDereference | `xbox_KfRaiseIrql(NewIrql)` |
| `__cdecl` | Variadic functions | `xbox_DbgPrint(format, ...)` |
