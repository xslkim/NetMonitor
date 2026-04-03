#pragma once

#include <stdint.h>

#if !defined(_KERNEL_MODE)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#endif

#define NETMONITOR_DRIVER_NAME             L"NetMonitorDrv"
#define NETMONITOR_DOS_DEVICE_NAME         L"\\\\.\\NetMonitorDrv"
#define NETMONITOR_NT_DEVICE_NAME          L"\\Device\\NetMonitorDrv"
#define NETMONITOR_DOS_DEVICE_LINK         L"\\DosDevices\\NetMonitorDrv"

typedef enum NmRateLimitDirection {
    NmRateLimitDirectionUpload = 0,
    NmRateLimitDirectionDownload = 1,
    NmRateLimitDirectionBoth = 2,
} NmRateLimitDirection;

#if !defined(FILE_DEVICE_NETMONITOR)
#define FILE_DEVICE_NETMONITOR 0x00008337
#endif

#define IOCTL_NETMONITOR_SET_RATE_LIMIT \
    CTL_CODE(FILE_DEVICE_NETMONITOR, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_NETMONITOR_REMOVE_RATE_LIMIT \
    CTL_CODE(FILE_DEVICE_NETMONITOR, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_NETMONITOR_CLEAR_LIMITS \
    CTL_CODE(FILE_DEVICE_NETMONITOR, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_NETMONITOR_QUERY_LIMIT \
    CTL_CODE(FILE_DEVICE_NETMONITOR, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)

#pragma pack(push, 1)

typedef struct NetMonitorSetRateLimitRequest {
    uint32_t pid;
    uint8_t direction;
    uint8_t reserved[3];
    uint64_t bytesPerSecond;
} NetMonitorSetRateLimitRequest;

typedef struct NetMonitorRemoveRateLimitRequest {
    uint32_t pid;
    uint8_t direction;
    uint8_t reserved[3];
} NetMonitorRemoveRateLimitRequest;

typedef struct NetMonitorQueryLimitRequest {
    uint32_t pid;
    uint8_t direction;
    uint8_t reserved[3];
} NetMonitorQueryLimitRequest;

typedef struct NetMonitorQueryLimitResponse {
    uint32_t pid;
    uint8_t direction;
    uint8_t active;
    uint8_t reserved[2];
    uint64_t bytesPerSecond;
    uint64_t availableTokens;
} NetMonitorQueryLimitResponse;

#pragma pack(pop)
