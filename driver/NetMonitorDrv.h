#pragma once

#include <ntifs.h>
#include <ndis.h>
#include <fwpmk.h>
#include <fwpsk.h>

#include "..\src\shared\DriverProtocol.h"

#define NETMONITOR_POOL_TAG 'mNtN'

DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD NetMonitorDriverUnload;

NTSTATUS
NetMonitorCreateClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    );

NTSTATUS
NetMonitorDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    );

NTSTATUS
NetMonitorRegisterCallouts(
    _In_ PDEVICE_OBJECT DeviceObject
    );

void
NetMonitorUnregisterCallouts(
    void
    );

NTSTATUS
NetMonitorRateLimitInitialize(
    void
    );

void
NetMonitorRateLimitCleanup(
    void
    );

NTSTATUS
NetMonitorRateLimitSet(
    _In_ HANDLE ProcessId,
    _In_ NmRateLimitDirection Direction,
    _In_ UINT64 BytesPerSecond
    );

void
NetMonitorRateLimitRemove(
    _In_ HANDLE ProcessId,
    _In_ NmRateLimitDirection Direction
    );

void
NetMonitorRateLimitClear(
    void
    );

BOOLEAN
NetMonitorRateLimitConsume(
    _In_ HANDLE ProcessId,
    _In_ NmRateLimitDirection Direction,
    _In_ UINT32 PacketBytes
    );

BOOLEAN
NetMonitorRateLimitQuery(
    _In_ HANDLE ProcessId,
    _In_ NmRateLimitDirection Direction,
    _Out_ UINT64* BytesPerSecond,
    _Out_ UINT64* AvailableTokens,
    _Out_ BOOLEAN* Active
    );
