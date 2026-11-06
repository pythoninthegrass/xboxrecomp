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

/* Project native Mm ownership. Guest allocations use a different provider. */
#include "host_blocks.h"
#include "host_pages.h"
#if !defined(_WIN32)
#include <pthread.h>
#endif

enum { NATIVE_MM_CONTIGUOUS=1, NATIVE_MM_SYSTEM, NATIVE_MM_IO, NATIVE_MM_STACK };
static tsfp_host_blocks native_mm_storage;
static tsfp_host_block native_mm_records[4096];
static int native_mm_ready;
static void *native_mm_map(void *context,size_t bytes,uint32_t protection)
{
    (void)context;
    return tsfp_host_pages_map(bytes,protection);
}
static int native_mm_unmap(void *context,void *address,size_t bytes)
{
    (void)context;
    return tsfp_host_pages_unmap(address,bytes);
}
static void native_mm_initialize(void)
{
    const tsfp_host_block_backend backend={NULL,native_mm_map,native_mm_unmap};
    native_mm_ready=tsfp_host_blocks_init(&native_mm_storage,&backend,
        tsfp_host_page_size(),native_mm_records,
        sizeof(native_mm_records)/sizeof(native_mm_records[0]));
}
#if defined(_WIN32)
static INIT_ONCE native_mm_once=INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK native_mm_initialize_once(PINIT_ONCE once,PVOID parameter,PVOID *context)
{
    (void)once;(void)parameter;(void)context;
    native_mm_initialize();return TRUE;
}
#else
static pthread_once_t native_mm_once=PTHREAD_ONCE_INIT;
#endif
static tsfp_host_blocks *native_mm_owner(void)
{
#if defined(_WIN32)
    if(!InitOnceExecuteOnce(&native_mm_once,native_mm_initialize_once,NULL,NULL))return NULL;
#else
    if(pthread_once(&native_mm_once,native_mm_initialize))return NULL;
#endif
    return native_mm_ready?&native_mm_storage:NULL;
}
static void *native_mm_allocate(size_t bytes,size_t alignment,uint32_t protection,uint32_t kind)
{
    if(!bytes)return NULL;
    return tsfp_host_block_allocate(native_mm_owner(),bytes,alignment,
        tsfp_host_vm_protection(protection),kind);
}
static void native_mm_release(void *address,uint32_t kind)
{
    if(address)(void)tsfp_host_block_release(native_mm_owner(),address,kind);
}
/* The process owns metadata for its lifetime. Kernel shutdown does not join
 * guest/device workers, so it must not tear down allocations still in use.
 * Matching Mm releases reclaim blocks; OS process exit reclaims the rest. */
/* End project native Mm ownership. */

PVOID __stdcall xbox_MmAllocateContiguousMemory(ULONG bytes)
{
    return native_mm_allocate(bytes,0,4,NATIVE_MM_CONTIGUOUS);
}

PVOID __stdcall xbox_MmAllocateContiguousMemoryEx(ULONG bytes,
    ULONG_PTR lowest,ULONG_PTR highest,ULONG alignment,ULONG protection)
{
    /* Native CPU storage has no Xbox physical range. Guest callers enforce
     * physical placement through their own allocator. Invalid alignment keeps
     * the existing page-aligned fallback; valid alignment is always honored. */
    (void)lowest;(void)highest;
    if(alignment && (alignment&(alignment-1)))alignment=0;
    return native_mm_allocate(bytes,alignment,protection,NATIVE_MM_CONTIGUOUS);
}

VOID __stdcall xbox_MmFreeContiguousMemory(PVOID address)
{
    native_mm_release(address,NATIVE_MM_CONTIGUOUS);
}

/* ============================================================================
 * System Memory
 * ============================================================================ */

PVOID __stdcall xbox_MmAllocateSystemMemory(ULONG bytes,ULONG protection)
{
    return native_mm_allocate(bytes,0,protection,NATIVE_MM_SYSTEM);
}

VOID __stdcall xbox_MmFreeSystemMemory(PVOID address,ULONG bytes)
{
    (void)bytes;native_mm_release(address,NATIVE_MM_SYSTEM);
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

PVOID __stdcall xbox_MmMapIoSpace(ULONG_PTR physical,ULONG bytes,ULONG protection)
{
    /* Preserve the native scratch-buffer route. Device MMIO belongs to the
     * guest mapping/device frontend, not a host physical mapping. */
    (void)physical;(void)protection;
    return native_mm_allocate(bytes,0,4,NATIVE_MM_IO);
}

VOID __stdcall xbox_MmUnmapIoSpace(PVOID address,ULONG bytes)
{
    (void)bytes;native_mm_release(address,NATIVE_MM_IO);
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

ULONG __stdcall xbox_MmQueryAddressProtect(PVOID address)
{
    tsfp_host_page_region region;
    return tsfp_host_pages_query(address,&region)?(ULONG)region.protection:1;
}

VOID __stdcall xbox_MmSetAddressProtect(PVOID address,ULONG bytes,ULONG protection)
{
    (void)tsfp_host_pages_protect(address,bytes,tsfp_host_vm_protection(protection));
}

ULONG __stdcall xbox_MmQueryAllocationSize(PVOID address)
{
    /* Retain the native OS-region query, distinct from guest allocation size.
     * Region boundaries may change after protection splits a mapping. */
    tsfp_host_page_region region;
    return tsfp_host_pages_query(address,&region)?(ULONG)region.bytes:0;
}

PVOID __stdcall xbox_MmClaimGpuInstanceMemory(ULONG bytes,PULONG padding)
{
    if(padding)*padding=0;
    return native_mm_allocate(bytes,0,4,NATIVE_MM_CONTIGUOUS);
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

PVOID __stdcall xbox_MmCreateKernelStack(ULONG bytes,BOOLEAN debugger)
{
    unsigned char *base;
    (void)debugger;
    base=(unsigned char *)native_mm_allocate(bytes,0,4,NATIVE_MM_STACK);
    return base?base+bytes:NULL;
}

VOID __stdcall xbox_MmDeleteKernelStack(PVOID top,PVOID limit)
{
    (void)top;native_mm_release(limit,NATIVE_MM_STACK);
}

/* ============================================================================
 * Virtual Memory (Nt API)
 * ============================================================================ */

/* Project native VM boundary: pointer and size_t widths stay native. */
static void *native_vm_allocate(void *host, void *hint, size_t size,
                                uint32_t type, uint32_t protection)
{
    (void)host;
    return VirtualAlloc(hint, size, (DWORD)type, xbox_protect_to_win32((ULONG)protection));
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
