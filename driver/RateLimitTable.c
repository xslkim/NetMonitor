#include "NetMonitorDrv.h"

typedef struct _NETMONITOR_RATE_LIMIT_ENTRY {
    LIST_ENTRY Link;
    HANDLE ProcessId;
    NmRateLimitDirection Direction;
    UINT64 BytesPerSecond;
    UINT64 AvailableTokens;
    LARGE_INTEGER LastRefillCounter;
} NETMONITOR_RATE_LIMIT_ENTRY;

typedef struct _NETMONITOR_RATE_LIMIT_TABLE {
    KSPIN_LOCK Lock;
    LIST_ENTRY Entries;
    LARGE_INTEGER CounterFrequency;
} NETMONITOR_RATE_LIMIT_TABLE;

static NETMONITOR_RATE_LIMIT_TABLE gRateTable;

static
NETMONITOR_RATE_LIMIT_ENTRY*
NetMonitorFindRateLimitEntryLocked(
    _In_ HANDLE ProcessId,
    _In_ NmRateLimitDirection Direction
    )
{
    PLIST_ENTRY current = gRateTable.Entries.Flink;
    while (current != &gRateTable.Entries) {
        NETMONITOR_RATE_LIMIT_ENTRY* entry = CONTAINING_RECORD(current, NETMONITOR_RATE_LIMIT_ENTRY, Link);
        if (entry->ProcessId == ProcessId && entry->Direction == Direction) {
            return entry;
        }
        current = current->Flink;
    }

    return NULL;
}

static
void
NetMonitorRefillTokensLocked(
    _Inout_ NETMONITOR_RATE_LIMIT_ENTRY* Entry,
    _In_ LARGE_INTEGER Now
    )
{
    LONGLONG elapsed = Now.QuadPart - Entry->LastRefillCounter.QuadPart;
    if (elapsed <= 0 || Entry->BytesPerSecond == 0 || gRateTable.CounterFrequency.QuadPart == 0) {
        Entry->LastRefillCounter = Now;
        return;
    }

    UINT64 addedTokens = (UINT64)((elapsed * Entry->BytesPerSecond) / gRateTable.CounterFrequency.QuadPart);
    if (addedTokens > 0) {
        UINT64 next = Entry->AvailableTokens + addedTokens;
        Entry->AvailableTokens = next > Entry->BytesPerSecond ? Entry->BytesPerSecond : next;
        Entry->LastRefillCounter = Now;
    }
}

static
NTSTATUS
NetMonitorRateLimitSetSingle(
    _In_ HANDLE ProcessId,
    _In_ NmRateLimitDirection Direction,
    _In_ UINT64 BytesPerSecond
    )
{
    KIRQL irql;
    LARGE_INTEGER now = KeQueryPerformanceCounter(NULL);

    KeAcquireSpinLock(&gRateTable.Lock, &irql);

    NETMONITOR_RATE_LIMIT_ENTRY* entry = NetMonitorFindRateLimitEntryLocked(ProcessId, Direction);
    if (entry == NULL) {
        entry = (NETMONITOR_RATE_LIMIT_ENTRY*)ExAllocatePoolWithTag(NonPagedPoolNx,
                                                                    sizeof(NETMONITOR_RATE_LIMIT_ENTRY),
                                                                    NETMONITOR_POOL_TAG);
        if (entry == NULL) {
            KeReleaseSpinLock(&gRateTable.Lock, irql);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlZeroMemory(entry, sizeof(*entry));
        entry->ProcessId = ProcessId;
        entry->Direction = Direction;
        InsertTailList(&gRateTable.Entries, &entry->Link);
    }

    entry->BytesPerSecond = BytesPerSecond;
    entry->AvailableTokens = BytesPerSecond;
    entry->LastRefillCounter = now;

    KeReleaseSpinLock(&gRateTable.Lock, irql);
    return STATUS_SUCCESS;
}

NTSTATUS
NetMonitorRateLimitInitialize(
    void
    )
{
    KeInitializeSpinLock(&gRateTable.Lock);
    InitializeListHead(&gRateTable.Entries);
    KeQueryPerformanceCounter(&gRateTable.CounterFrequency);
    return STATUS_SUCCESS;
}

void
NetMonitorRateLimitCleanup(
    void
    )
{
    NetMonitorRateLimitClear();
}

NTSTATUS
NetMonitorRateLimitSet(
    _In_ HANDLE ProcessId,
    _In_ NmRateLimitDirection Direction,
    _In_ UINT64 BytesPerSecond
    )
{
    if (Direction == NmRateLimitDirectionBoth) {
        NTSTATUS status = NetMonitorRateLimitSetSingle(ProcessId, NmRateLimitDirectionUpload, BytesPerSecond);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        return NetMonitorRateLimitSetSingle(ProcessId, NmRateLimitDirectionDownload, BytesPerSecond);
    }

    return NetMonitorRateLimitSetSingle(ProcessId, Direction, BytesPerSecond);
}

void
NetMonitorRateLimitRemove(
    _In_ HANDLE ProcessId,
    _In_ NmRateLimitDirection Direction
    )
{
    KIRQL irql;
    KeAcquireSpinLock(&gRateTable.Lock, &irql);

    PLIST_ENTRY current = gRateTable.Entries.Flink;
    while (current != &gRateTable.Entries) {
        NETMONITOR_RATE_LIMIT_ENTRY* entry = CONTAINING_RECORD(current, NETMONITOR_RATE_LIMIT_ENTRY, Link);
        PLIST_ENTRY next = current->Flink;
        BOOLEAN matchProcess = entry->ProcessId == ProcessId;
        BOOLEAN matchDirection = (Direction == NmRateLimitDirectionBoth) || (entry->Direction == Direction);
        if (matchProcess && matchDirection) {
            RemoveEntryList(&entry->Link);
            ExFreePoolWithTag(entry, NETMONITOR_POOL_TAG);
        }
        current = next;
    }

    KeReleaseSpinLock(&gRateTable.Lock, irql);
}

void
NetMonitorRateLimitClear(
    void
    )
{
    KIRQL irql;
    KeAcquireSpinLock(&gRateTable.Lock, &irql);

    while (!IsListEmpty(&gRateTable.Entries)) {
        PLIST_ENTRY entryLink = RemoveHeadList(&gRateTable.Entries);
        NETMONITOR_RATE_LIMIT_ENTRY* entry = CONTAINING_RECORD(entryLink, NETMONITOR_RATE_LIMIT_ENTRY, Link);
        ExFreePoolWithTag(entry, NETMONITOR_POOL_TAG);
    }

    KeReleaseSpinLock(&gRateTable.Lock, irql);
}

BOOLEAN
NetMonitorRateLimitConsume(
    _In_ HANDLE ProcessId,
    _In_ NmRateLimitDirection Direction,
    _In_ UINT32 PacketBytes
    )
{
    BOOLEAN permitted = TRUE;
    KIRQL irql;
    LARGE_INTEGER now = KeQueryPerformanceCounter(NULL);

    KeAcquireSpinLock(&gRateTable.Lock, &irql);

    NETMONITOR_RATE_LIMIT_ENTRY* entry = NetMonitorFindRateLimitEntryLocked(ProcessId, Direction);
    if (entry != NULL) {
        NetMonitorRefillTokensLocked(entry, now);
        if (entry->AvailableTokens >= PacketBytes) {
            entry->AvailableTokens -= PacketBytes;
        } else {
            permitted = FALSE;
        }
    }

    KeReleaseSpinLock(&gRateTable.Lock, irql);
    return permitted;
}

BOOLEAN
NetMonitorRateLimitQuery(
    _In_ HANDLE ProcessId,
    _In_ NmRateLimitDirection Direction,
    _Out_ UINT64* BytesPerSecond,
    _Out_ UINT64* AvailableTokens,
    _Out_ BOOLEAN* Active
    )
{
    BOOLEAN found = FALSE;
    KIRQL irql;
    LARGE_INTEGER now = KeQueryPerformanceCounter(NULL);

    *BytesPerSecond = 0;
    *AvailableTokens = 0;
    *Active = FALSE;

    KeAcquireSpinLock(&gRateTable.Lock, &irql);

    NETMONITOR_RATE_LIMIT_ENTRY* entry = NetMonitorFindRateLimitEntryLocked(ProcessId, Direction);
    if (entry != NULL) {
        NetMonitorRefillTokensLocked(entry, now);
        *BytesPerSecond = entry->BytesPerSecond;
        *AvailableTokens = entry->AvailableTokens;
        *Active = TRUE;
        found = TRUE;
    }

    KeReleaseSpinLock(&gRateTable.Lock, irql);
    return found;
}
