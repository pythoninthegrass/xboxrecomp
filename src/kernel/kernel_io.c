/*
 * kernel_io.c - I/O Manager Stubs
 *
 * Transitional request and completion services. Device ownership and
 * create/delete adapters are selected from project providers.
 *
 * The Xbox I/O manager is used internally by XDK libraries (DSOUND, D3D,
 * storage drivers). Since we replace those libraries entirely with Win32
 * equivalents, most I/O manager functions are stubs that return success.
 */

#include "kernel.h"
#include <string.h>

/* ============================================================================
 * Type Object Pointers
 *
 * Exported type objects for device and completion port types.
 * Game code rarely uses these directly - they're mainly for the
 * XDK libraries we're replacing.
 * ============================================================================ */

static ULONG g_completion_object_type_data = 0x434F4D50; /* 'COMP' */
static ULONG g_file_object_type_data       = 0x46494C45; /* 'FILE' */

PVOID xbox_IoCompletionObjectType = &g_completion_object_type_data;
PVOID xbox_IoFileObjectType       = &g_file_object_type_data;

/* Device creation/deletion is owned by the project guest bridge. */

/* ============================================================================
 * IRP Management Stubs
 *
 * IRPs (I/O Request Packets) are the core mechanism for driver I/O on Xbox.
 * Since we don't use real drivers, these are all stubs.
 * ============================================================================ */


VOID __stdcall xbox_IoStartNextPacket(PVOID DeviceObject, BOOLEAN Cancelable)
{
    (void)DeviceObject;
    (void)Cancelable;
}

VOID __stdcall xbox_IoStartNextPacketByKey(PVOID DeviceObject, BOOLEAN Cancelable, ULONG Key)
{
    (void)DeviceObject;
    (void)Cancelable;
    (void)Key;
}

VOID __stdcall xbox_IoStartPacket(PVOID DeviceObject, PVOID Irp, PULONG Key, PVOID CancelFunction)
{
    (void)DeviceObject;
    (void)Irp;
    (void)Key;
    (void)CancelFunction;
}

VOID __stdcall xbox_IoMarkIrpMustComplete(PVOID Irp)
{
    (void)Irp;
}

/* ============================================================================
 * I/O Completion Ports
 * ============================================================================ */

NTSTATUS __stdcall xbox_IoSetIoCompletion(
    PVOID IoCompletion,
    PVOID KeyContext,
    PVOID ApcContext,
    NTSTATUS IoStatus,
    ULONG_PTR IoStatusInformation)
{
    (void)IoCompletion;
    (void)KeyContext;
    (void)ApcContext;
    (void)IoStatus;
    (void)IoStatusInformation;

    xbox_log(XBOX_LOG_TRACE, XBOX_LOG_IO,
        "IoSetIoCompletion: completion=%p status=0x%08X (stubbed)",
        IoCompletion, IoStatus);

    return STATUS_SUCCESS;
}

/* ============================================================================
 * Synchronous Device I/O
 *
 * These functions perform synchronous I/O through the driver stack.
 * Stubbed since we replace all Xbox drivers.
 * ============================================================================ */

NTSTATUS __stdcall xbox_IoSynchronousDeviceIoControlRequest(
    ULONG IoControlCode,
    PVOID DeviceObject,
    PVOID InputBuffer,
    ULONG InputBufferLength,
    PVOID OutputBuffer,
    ULONG OutputBufferLength,
    PULONG ReturnedOutputBufferLength,
    BOOLEAN InternalDeviceIoControl)
{
    (void)IoControlCode;
    (void)DeviceObject;
    (void)InputBuffer;
    (void)InputBufferLength;
    (void)OutputBuffer;
    (void)OutputBufferLength;
    (void)InternalDeviceIoControl;

    if (ReturnedOutputBufferLength)
        *ReturnedOutputBufferLength = 0;

    xbox_log(XBOX_LOG_TRACE, XBOX_LOG_IO,
        "IoSynchronousDeviceIoControlRequest: ioctl=0x%08X device=%p (stubbed)",
        IoControlCode, DeviceObject);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_IoBuildDeviceIoControlRequest(
    ULONG IoControlCode,
    PVOID DeviceObject,
    PVOID InputBuffer,
    ULONG InputBufferLength,
    PVOID OutputBuffer,
    ULONG OutputBufferLength,
    BOOLEAN InternalDeviceIoControl,
    HANDLE Event,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock)
{
    (void)IoControlCode;
    (void)DeviceObject;
    (void)InputBuffer;
    (void)InputBufferLength;
    (void)OutputBuffer;
    (void)OutputBufferLength;
    (void)InternalDeviceIoControl;
    (void)Event;

    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = 0;
    }

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_IoSynchronousFsdRequest(
    ULONG MajorFunction,
    PVOID DeviceObject,
    PVOID Buffer,
    ULONG Length,
    PLARGE_INTEGER StartingOffset)
{
    (void)MajorFunction;
    (void)DeviceObject;
    (void)Buffer;
    (void)Length;
    (void)StartingOffset;

    xbox_log(XBOX_LOG_TRACE, XBOX_LOG_IO,
        "IoSynchronousFsdRequest: major=%u device=%p (stubbed)",
        MajorFunction, DeviceObject);

    return STATUS_SUCCESS;
}

PVOID __stdcall xbox_IoBuildSynchronousFsdRequest(
    ULONG MajorFunction,
    PVOID DeviceObject,
    PVOID Buffer,
    ULONG Length,
    PLARGE_INTEGER StartingOffset,
    HANDLE Event,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock)
{
    (void)DeviceObject;
    (void)Buffer;
    (void)Length;
    (void)StartingOffset;
    (void)Event;

    /*
     * Returns a built IRP for the caller to hand to IofCallDriver. We have no
     * driver stack, so there is nothing meaningful to build. Returning NULL is
     * the documented allocation-failure path, which callers already handle;
     * inventing a fake IRP would only push the failure to whoever dereferences
     * it. IoStatusBlock is filled in so a caller that reads it without checking
     * the return sees a coherent result.
     */
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = 0;
    }

    xbox_log(XBOX_LOG_WARN, XBOX_LOG_IO,
        "IoBuildSynchronousFsdRequest: major=%u len=%u - no driver stack, returning NULL",
        MajorFunction, Length);

    return NULL;
}

/* ============================================================================
 * Driver Dispatch
 *
 * IofCallDriver/IofCompleteRequest are __fastcall on Xbox (the 'f' suffix).
 * Getting the convention wrong here corrupts the stack at every call site.
 * ============================================================================ */

NTSTATUS __fastcall xbox_IofCallDriver(PVOID DeviceObject, PVOID Irp)
{
    (void)DeviceObject;
    (void)Irp;

    xbox_log(XBOX_LOG_TRACE, XBOX_LOG_IO,
        "IofCallDriver: device=%p irp=%p (stubbed)", DeviceObject, Irp);

    return STATUS_SUCCESS;
}

VOID __fastcall xbox_IofCompleteRequest(PVOID Irp, CCHAR PriorityBoost)
{
    (void)Irp;
    (void)PriorityBoost;
}

NTSTATUS __stdcall xbox_IoInvalidDeviceRequest(PVOID DeviceObject, PVOID Irp)
{
    (void)DeviceObject;
    (void)Irp;

    /*
     * The default dispatch entry for major functions a driver does not handle.
     * Its whole contract is to reject, so this is a real implementation rather
     * than a stub.
     */
    return STATUS_INVALID_DEVICE_REQUEST;
}

/* Symbolic links are owned by the project guest namespace. */
