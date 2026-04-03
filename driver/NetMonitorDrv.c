#include "NetMonitorDrv.h"

static PDEVICE_OBJECT gDeviceObject = NULL;
static UNICODE_STRING gDeviceName;
static UNICODE_STRING gSymbolicLinkName;

static
void
NetMonitorCompleteIrp(
    _Inout_ PIRP Irp,
    _In_ NTSTATUS Status,
    _In_ ULONG_PTR Information
    )
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static
NTSTATUS
NetMonitorHandleSetLimit(
    _In_reads_bytes_(InputLength) const void* InputBuffer,
    _In_ ULONG InputLength
    )
{
    if (InputLength < sizeof(NetMonitorSetRateLimitRequest)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    const NetMonitorSetRateLimitRequest* request = (const NetMonitorSetRateLimitRequest*)InputBuffer;
    return NetMonitorRateLimitSet((HANDLE)(UINT_PTR)request->pid,
                                  (NmRateLimitDirection)request->direction,
                                  request->bytesPerSecond);
}

static
NTSTATUS
NetMonitorHandleRemoveLimit(
    _In_reads_bytes_(InputLength) const void* InputBuffer,
    _In_ ULONG InputLength
    )
{
    if (InputLength < sizeof(NetMonitorRemoveRateLimitRequest)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    const NetMonitorRemoveRateLimitRequest* request = (const NetMonitorRemoveRateLimitRequest*)InputBuffer;
    NetMonitorRateLimitRemove((HANDLE)(UINT_PTR)request->pid,
                              (NmRateLimitDirection)request->direction);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NetMonitorHandleQueryLimit(
    _In_reads_bytes_(InputLength) const void* InputBuffer,
    _In_ ULONG InputLength,
    _Out_writes_bytes_(OutputLength) void* OutputBuffer,
    _In_ ULONG OutputLength,
    _Out_ ULONG_PTR* Information
    )
{
    if (InputLength < sizeof(NetMonitorQueryLimitRequest) ||
        OutputLength < sizeof(NetMonitorQueryLimitResponse)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    const NetMonitorQueryLimitRequest* request = (const NetMonitorQueryLimitRequest*)InputBuffer;
    NetMonitorQueryLimitResponse* response = (NetMonitorQueryLimitResponse*)OutputBuffer;
    UINT64 bytesPerSecond = 0;
    UINT64 availableTokens = 0;
    BOOLEAN active = FALSE;

    NetMonitorRateLimitQuery((HANDLE)(UINT_PTR)request->pid,
                             (NmRateLimitDirection)request->direction,
                             &bytesPerSecond,
                             &availableTokens,
                             &active);

    RtlZeroMemory(response, sizeof(*response));
    response->pid = request->pid;
    response->direction = request->direction;
    response->active = active ? 1 : 0;
    response->bytesPerSecond = bytesPerSecond;
    response->availableTokens = availableTokens;

    *Information = sizeof(*response);
    return STATUS_SUCCESS;
}

NTSTATUS
NetMonitorCreateClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    NetMonitorCompleteIrp(Irp, STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

NTSTATUS
NetMonitorDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG controlCode = stack->Parameters.DeviceIoControl.IoControlCode;
    void* inputBuffer = Irp->AssociatedIrp.SystemBuffer;
    void* outputBuffer = Irp->AssociatedIrp.SystemBuffer;
    ULONG inputLength = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG_PTR information = 0;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;

    switch (controlCode) {
    case IOCTL_NETMONITOR_SET_RATE_LIMIT:
        status = NetMonitorHandleSetLimit(inputBuffer, inputLength);
        break;
    case IOCTL_NETMONITOR_REMOVE_RATE_LIMIT:
        status = NetMonitorHandleRemoveLimit(inputBuffer, inputLength);
        break;
    case IOCTL_NETMONITOR_CLEAR_LIMITS:
        NetMonitorRateLimitClear();
        status = STATUS_SUCCESS;
        break;
    case IOCTL_NETMONITOR_QUERY_LIMIT:
        status = NetMonitorHandleQueryLimit(inputBuffer,
                                            inputLength,
                                            outputBuffer,
                                            outputLength,
                                            &information);
        break;
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    NetMonitorCompleteIrp(Irp, status, information);
    return status;
}

void
NetMonitorDriverUnload(
    _In_ PDRIVER_OBJECT DriverObject
    )
{
    UNREFERENCED_PARAMETER(DriverObject);

    NetMonitorUnregisterCallouts();
    NetMonitorRateLimitCleanup();

    if (gDeviceObject != NULL) {
        IoDeleteSymbolicLink(&gSymbolicLinkName);
        IoDeleteDevice(gDeviceObject);
        gDeviceObject = NULL;
    }
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    UNREFERENCED_PARAMETER(RegistryPath);

    NTSTATUS status;
    UINT32 index;

    RtlInitUnicodeString(&gDeviceName, NETMONITOR_NT_DEVICE_NAME);
    RtlInitUnicodeString(&gSymbolicLinkName, NETMONITOR_DOS_DEVICE_LINK);

    status = IoCreateDevice(DriverObject,
                            0,
                            &gDeviceName,
                            FILE_DEVICE_NETMONITOR,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &gDeviceObject);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    gDeviceObject->Flags |= DO_BUFFERED_IO;

    status = IoCreateSymbolicLink(&gSymbolicLinkName, &gDeviceName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(gDeviceObject);
        gDeviceObject = NULL;
        return status;
    }

    for (index = 0; index <= IRP_MJ_MAXIMUM_FUNCTION; ++index) {
        DriverObject->MajorFunction[index] = NetMonitorCreateClose;
    }
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = NetMonitorDeviceControl;
    DriverObject->DriverUnload = NetMonitorDriverUnload;

    status = NetMonitorRateLimitInitialize();
    if (!NT_SUCCESS(status)) {
        NetMonitorDriverUnload(DriverObject);
        return status;
    }

    status = NetMonitorRegisterCallouts(gDeviceObject);
    if (!NT_SUCCESS(status)) {
        NetMonitorDriverUnload(DriverObject);
        return status;
    }

    gDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
