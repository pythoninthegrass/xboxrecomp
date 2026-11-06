/*
 * kernel_memory.c - Xbox Memory Management
 *
 * Implements Mm* and NtAllocateVirtualMemory/NtFreeVirtualMemory/NtQueryVirtualMemory
 * using Win32 VirtualAlloc/VirtualFree/VirtualQuery.
 *
 * Xbox contiguous memory (MmAllocateContiguousMemory) is used for GPU-accessible
 * buffers. On Windows, actual GPU resources are handled by our D3D11 layer;
 * these allocations just need to return valid CPU-accessible memory.
 */

#include "kernel.h"
#include "host_virtual_memory.h"
#if defined(_WIN32)
/* _aligned_malloc/_aligned_free; POSIX gets them from win32_compat.h */
#include <malloc.h>
#endif

/* ============================================================================
 * Helper: Xbox protect flags → Win32 protect flags
 * ============================================================================ */

static DWORD xbox_protect_to_win32(ULONG protection)
{
    return (DWORD)tsfp_host_vm_protection((uint32_t)protection);
}

/* ============================================================================
 * Contiguous Memory (GPU-accessible on Xbox)
 * ============================================================================ */

PVOID __stdcall xbox_MmAllocateContiguousMemory(ULONG NumberOfBytes)
{
    PVOID p = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    XBOX_TRACE(XBOX_LOG_MEM, "MmAllocateContiguousMemory(%u) = %p", NumberOfBytes, p);
    return p;
}

PVOID __stdcall xbox_MmAllocateContiguousMemoryEx(
    ULONG NumberOfBytes,
    ULONG_PTR LowestAcceptableAddress,
    ULONG_PTR HighestAcceptableAddress,
    ULONG Alignment,
    ULONG Protect)
{
    /*
     * Xbox requests physically contiguous, aligned memory for GPU use.
     * We can't guarantee physical contiguity on Windows, but the game's
     * CPU-side code just needs a valid pointer. GPU resources are handled
     * separately by our D3D11 layer.
     *
     * Use _aligned_malloc for alignment, then VirtualAlloc for a fallback.
     */
    PVOID p = NULL;

    if (Alignment > 0 && (Alignment & (Alignment - 1)) == 0) {
        /* Power-of-2 alignment: use _aligned_malloc */
        p = _aligned_malloc(NumberOfBytes, Alignment);
        if (p)
            memset(p, 0, NumberOfBytes);
    }

    if (!p) {
        /* Fallback: page-aligned VirtualAlloc */
        p = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE,
                         xbox_protect_to_win32(Protect));
    }

    XBOX_TRACE(XBOX_LOG_MEM, "MmAllocateContiguousMemoryEx(%u, align=%u) = %p",
        NumberOfBytes, Alignment, p);
    return p;
}

VOID __stdcall xbox_MmFreeContiguousMemory(PVOID BaseAddress)
{
    XBOX_TRACE(XBOX_LOG_MEM, "MmFreeContiguousMemory(%p)", BaseAddress);
    if (!BaseAddress)
        return;

    /*
     * Determine if this was allocated with _aligned_malloc or VirtualAlloc.
     * We use VirtualQuery to check: if it's a VirtualAlloc'd region,
     * AllocationBase will equal the pointer (page-aligned).
     */
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(BaseAddress, &mbi, sizeof(mbi)) &&
        mbi.AllocationBase == BaseAddress &&
        mbi.State == MEM_COMMIT) {
        VirtualFree(BaseAddress, 0, MEM_RELEASE);
    } else {
        _aligned_free(BaseAddress);
    }
}

/* ============================================================================
 * System Memory
 * ============================================================================ */

PVOID __stdcall xbox_MmAllocateSystemMemory(ULONG NumberOfBytes, ULONG Protect)
{
    PVOID p = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE,
                           xbox_protect_to_win32(Protect));
    XBOX_TRACE(XBOX_LOG_MEM, "MmAllocateSystemMemory(%u) = %p", NumberOfBytes, p);
    return p;
}

VOID __stdcall xbox_MmFreeSystemMemory(PVOID BaseAddress, ULONG NumberOfBytes)
{
    XBOX_TRACE(XBOX_LOG_MEM, "MmFreeSystemMemory(%p, %u)", BaseAddress, NumberOfBytes);
    if (BaseAddress)
        VirtualFree(BaseAddress, 0, MEM_RELEASE);
}

/* ============================================================================
 * Memory Query & Protection
 * ============================================================================ */

NTSTATUS __stdcall xbox_MmQueryStatistics(PXBOX_MM_STATISTICS MemoryStatistics)
{
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);

    if (!MemoryStatistics)
        return STATUS_INVALID_PARAMETER;

    if (!GlobalMemoryStatusEx(&ms))
        return STATUS_UNSUCCESSFUL;

    memset(MemoryStatistics, 0, sizeof(XBOX_MM_STATISTICS));
    MemoryStatistics->Length = sizeof(XBOX_MM_STATISTICS);

    /* Xbox has 64MB RAM. Report plausible values. */
    ULONG page_size = 4096;
    MemoryStatistics->TotalPhysicalPages = 64 * 1024 * 1024 / page_size; /* 16384 pages */
    MemoryStatistics->AvailablePages = (ULONG)(ms.ullAvailPhys / page_size);
    if (MemoryStatistics->AvailablePages > MemoryStatistics->TotalPhysicalPages)
        MemoryStatistics->AvailablePages = MemoryStatistics->TotalPhysicalPages / 2;

    return STATUS_SUCCESS;
}

PVOID __stdcall xbox_MmMapIoSpace(ULONG_PTR PhysicalAddress, ULONG NumberOfBytes, ULONG Protect)
{
    /* GPU register access - handled by our D3D11 layer. Return a dummy buffer. */
    PVOID p = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    XBOX_TRACE(XBOX_LOG_MEM, "MmMapIoSpace(0x%08X, %u) = %p (stub)", (ULONG)PhysicalAddress, NumberOfBytes, p);
    return p;
}

VOID __stdcall xbox_MmUnmapIoSpace(PVOID BaseAddress, ULONG NumberOfBytes)
{
    XBOX_TRACE(XBOX_LOG_MEM, "MmUnmapIoSpace(%p, %u)", BaseAddress, NumberOfBytes);
    if (BaseAddress)
        VirtualFree(BaseAddress, 0, MEM_RELEASE);
}

ULONG_PTR __stdcall xbox_MmGetPhysicalAddress(PVOID BaseAddress)
{
    /* No physical address translation on Windows - return the VA as a placeholder */
    return (ULONG_PTR)BaseAddress;
}

VOID __stdcall xbox_MmPersistContiguousMemory(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN Persist)
{
    /* Xbox: mark memory to survive soft-reboot. No-op on Windows. */
    XBOX_TRACE(XBOX_LOG_MEM, "MmPersistContiguousMemory(%p, %u, %d) - stub", BaseAddress, NumberOfBytes, Persist);
}

ULONG __stdcall xbox_MmQueryAddressProtect(PVOID VirtualAddress)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(VirtualAddress, &mbi, sizeof(mbi))) {
        /* Return the Xbox-equivalent protection */
        return mbi.Protect;
    }
    return PAGE_NOACCESS;
}

VOID __stdcall xbox_MmSetAddressProtect(PVOID BaseAddress, ULONG NumberOfBytes, ULONG NewProtect)
{
    DWORD old_protect;
    VirtualProtect(BaseAddress, NumberOfBytes, xbox_protect_to_win32(NewProtect), &old_protect);
    XBOX_TRACE(XBOX_LOG_MEM, "MmSetAddressProtect(%p, %u, 0x%X)", BaseAddress, NumberOfBytes, NewProtect);
}

ULONG __stdcall xbox_MmQueryAllocationSize(PVOID BaseAddress)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(BaseAddress, &mbi, sizeof(mbi))) {
        return (ULONG)mbi.RegionSize;
    }
    return 0;
}

PVOID __stdcall xbox_MmClaimGpuInstanceMemory(ULONG NumberOfBytes, PULONG NumberOfPaddingBytes)
{
    /* GPU instance memory - handled by D3D11 layer */
    if (NumberOfPaddingBytes)
        *NumberOfPaddingBytes = 0;
    PVOID p = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    XBOX_TRACE(XBOX_LOG_MEM, "MmClaimGpuInstanceMemory(%u) = %p (stub)", NumberOfBytes, p);
    return p;
}

VOID __stdcall xbox_MmLockUnlockBufferPages(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN UnlockPages)
{
    /* Page locking is not meaningful in user mode. No-op. */
    XBOX_TRACE(XBOX_LOG_MEM, "MmLockUnlockBufferPages(%p, %u, %d) - stub", BaseAddress, NumberOfBytes, UnlockPages);
}

VOID __stdcall xbox_MmLockUnlockPhysicalPage(ULONG_PTR PhysicalAddress, BOOLEAN UnlockPage)
{
    XBOX_TRACE(XBOX_LOG_MEM, "MmLockUnlockPhysicalPage(0x%08X, %d) - stub", (ULONG)PhysicalAddress, UnlockPage);
}

/* ============================================================================
 * Kernel Stack
 * ============================================================================ */

PVOID __stdcall xbox_MmCreateKernelStack(ULONG NumberOfBytes, BOOLEAN DebuggerThread)
{
    /* Allocate a stack-like region. Return the TOP of the stack (high address). */
    PVOID base = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!base)
        return NULL;

    /* Xbox convention: return pointer to top of stack */
    PVOID stack_top = (PUCHAR)base + NumberOfBytes;
    XBOX_TRACE(XBOX_LOG_MEM, "MmCreateKernelStack(%u) = %p (base=%p)", NumberOfBytes, stack_top, base);
    return stack_top;
}

VOID __stdcall xbox_MmDeleteKernelStack(PVOID StackBase, PVOID StackLimit)
{
    /* StackLimit is the low address (base of VirtualAlloc), StackBase is the high address */
    XBOX_TRACE(XBOX_LOG_MEM, "MmDeleteKernelStack(base=%p, limit=%p)", StackBase, StackLimit);
    if (StackLimit)
        VirtualFree(StackLimit, 0, MEM_RELEASE);
}

/* ============================================================================
 * Virtual Memory (Nt API)
 * ============================================================================ */

/* Project native VM boundary: pointer and size_t widths stay native. */
static void *native_vm_allocate(void *host, void *hint, size_t size,
                                uint32_t type, uint32_t protection)
{
    (void)host;
    return VirtualAlloc(hint, size, (DWORD)type, (DWORD)protection);
}
static int native_vm_release(void *host, void *address, size_t size, uint32_t type)
{
    (void)host;
    return VirtualFree(address, size, (DWORD)type) != 0;
}
static size_t native_vm_query(void *host, const void *address, void *record, size_t size)
{
    (void)host;
    return VirtualQuery(address, (MEMORY_BASIC_INFORMATION *)record, size);
}
static const tsfp_host_vm native_vm = { NULL, native_vm_allocate, native_vm_release, native_vm_query };
/* End project native VM boundary. */

NTSTATUS __stdcall xbox_NtAllocateVirtualMemory(PVOID *base, ULONG_PTR zero_bits,
    PSIZE_T size, ULONG type, ULONG protection)
{
    return (NTSTATUS)tsfp_host_vm_allocate(&native_vm, base, zero_bits, size, type, protection);
}

NTSTATUS __stdcall xbox_NtFreeVirtualMemory(PVOID *base, PSIZE_T size, ULONG type)
{
    return (NTSTATUS)tsfp_host_vm_free(&native_vm, base, size, type);
}

NTSTATUS __stdcall xbox_NtQueryVirtualMemory(PVOID address, PVOID information,
    ULONG length, PULONG returned)
{
    MEMORY_BASIC_INFORMATION scratch;
    uint32_t copied = 0;
    uint32_t status = tsfp_host_vm_query(&native_vm, address, information, length,
        &copied, &scratch, sizeof(scratch));
    if (!status && returned) *returned = (ULONG)copied;
    return (NTSTATUS)status;
}
