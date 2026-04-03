#define INITGUID
#include "NetMonitorDrv.h"

DEFINE_GUID(NETMONITOR_SUBLAYER_GUID,
    0xb8af6a8c, 0x8f54, 0x4eef, 0xb5, 0x8a, 0x40, 0x43, 0x0e, 0xf3, 0x70, 0x21);
DEFINE_GUID(NETMONITOR_CALLOUT_OUTBOUND_V4_GUID,
    0x5d8cb412, 0x39d7, 0x49e2, 0x8b, 0x55, 0x1e, 0xfc, 0x07, 0xd4, 0x75, 0xb1);
DEFINE_GUID(NETMONITOR_CALLOUT_INBOUND_V4_GUID,
    0x8672e6e2, 0x2f65, 0x420c, 0x9b, 0xdd, 0x49, 0x6e, 0x6c, 0x55, 0x12, 0x0f);
DEFINE_GUID(NETMONITOR_CALLOUT_OUTBOUND_V6_GUID,
    0x8fcb6d95, 0x74b7, 0x4c71, 0x9f, 0x14, 0x12, 0x95, 0xe8, 0xf5, 0x54, 0x24);
DEFINE_GUID(NETMONITOR_CALLOUT_INBOUND_V6_GUID,
    0x60d3f8fd, 0x10af, 0x4242, 0xa0, 0x78, 0xc6, 0x27, 0x52, 0xde, 0x23, 0x8d);
DEFINE_GUID(NETMONITOR_CALLOUT_ALE_FLOW_ESTABLISHED_V4_GUID,
    0x9c4b9eb4, 0x0af7, 0x4c22, 0xa2, 0xa4, 0x77, 0x45, 0xf6, 0xcb, 0xb0, 0xe5);
DEFINE_GUID(NETMONITOR_CALLOUT_ALE_FLOW_ESTABLISHED_V6_GUID,
    0x0f93ea3c, 0x7b3a, 0x4f4f, 0x95, 0x0f, 0x46, 0x49, 0x5a, 0xab, 0x1c, 0x31);

typedef struct _NETMONITOR_CALLOUT_DESCRIPTOR {
    const GUID* CalloutGuid;
    const GUID* LayerGuid;
    UINT32 RuntimeId;
} NETMONITOR_CALLOUT_DESCRIPTOR;

typedef struct _NETMONITOR_FLOW_CONTEXT {
    HANDLE ProcessId;
} NETMONITOR_FLOW_CONTEXT;

static NETMONITOR_CALLOUT_DESCRIPTOR gCallouts[] = {
    { &NETMONITOR_CALLOUT_OUTBOUND_V4_GUID, &FWPM_LAYER_OUTBOUND_TRANSPORT_V4, 0 },
    { &NETMONITOR_CALLOUT_INBOUND_V4_GUID,  &FWPM_LAYER_INBOUND_TRANSPORT_V4,  0 },
    { &NETMONITOR_CALLOUT_OUTBOUND_V6_GUID, &FWPM_LAYER_OUTBOUND_TRANSPORT_V6, 0 },
    { &NETMONITOR_CALLOUT_INBOUND_V6_GUID,  &FWPM_LAYER_INBOUND_TRANSPORT_V6,  0 },
};

static UINT32 gAleFlowEstablishedV4Id = 0;
static UINT32 gAleFlowEstablishedV6Id = 0;

static HANDLE gEngineHandle = NULL;

static
NTSTATUS
NetMonitorAssociateFlowContext(
    _In_ UINT64 FlowHandle,
    _In_ UINT16 LayerId,
    _In_ UINT32 CalloutId,
    _In_ HANDLE ProcessId
    )
{
    NETMONITOR_FLOW_CONTEXT* context;
    NTSTATUS status;

    context = (NETMONITOR_FLOW_CONTEXT*)ExAllocatePoolWithTag(NonPagedPoolNx,
                                                              sizeof(NETMONITOR_FLOW_CONTEXT),
                                                              NETMONITOR_POOL_TAG);
    if (context == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    context->ProcessId = ProcessId;
    status = FwpsFlowAssociateContext0(FlowHandle,
                                       LayerId,
                                       CalloutId,
                                       (UINT64)(UINT_PTR)context);
    if (status == STATUS_OBJECT_NAME_EXISTS) {
        FwpsFlowRemoveContext0(FlowHandle, LayerId, CalloutId);
        status = FwpsFlowAssociateContext0(FlowHandle,
                                           LayerId,
                                           CalloutId,
                                           (UINT64)(UINT_PTR)context);
    }
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(context, NETMONITOR_POOL_TAG);
    }

    return status;
}

static
UINT32
NetMonitorGetDirectionForLayer(
    _In_ UINT16 LayerId
    )
{
    switch (LayerId) {
    case FWPS_LAYER_OUTBOUND_TRANSPORT_V4:
    case FWPS_LAYER_OUTBOUND_TRANSPORT_V6:
        return NmRateLimitDirectionUpload;
    default:
        return NmRateLimitDirectionDownload;
    }
}

static
UINT32
NetMonitorCountBytes(
    _In_opt_ NET_BUFFER_LIST* NetBufferList
    )
{
    UINT32 totalBytes = 0;
    NET_BUFFER_LIST* currentList = NetBufferList;
    while (currentList != NULL) {
        NET_BUFFER* currentBuffer = NET_BUFFER_LIST_FIRST_NB(currentList);
        while (currentBuffer != NULL) {
            totalBytes += NET_BUFFER_DATA_LENGTH(currentBuffer);
            currentBuffer = NET_BUFFER_NEXT_NB(currentBuffer);
        }
        currentList = NET_BUFFER_LIST_NEXT_NBL(currentList);
    }
    return totalBytes;
}

static
void
NTAPI
NetMonitorClassify(
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _Inout_opt_ void* LayerData,
    _In_opt_ const FWPS_FILTER0* Filter,
    _In_ UINT64 FlowContext,
    _Inout_ FWPS_CLASSIFY_OUT0* ClassifyOut
    )
{
    UNREFERENCED_PARAMETER(Filter);
    UNREFERENCED_PARAMETER(FlowContext);

    if ((ClassifyOut->rights & FWPS_RIGHT_ACTION_WRITE) == 0) {
        return;
    }

    if (LayerData == NULL) {
        ClassifyOut->actionType = FWP_ACTION_PERMIT;
        return;
    }

    HANDLE processId = NULL;
    if (FlowContext != 0) {
        NETMONITOR_FLOW_CONTEXT* flowContext = (NETMONITOR_FLOW_CONTEXT*)(UINT_PTR)FlowContext;
        processId = flowContext->ProcessId;
    } else if (FWPS_IS_METADATA_FIELD_PRESENT(InMetaValues, FWPS_METADATA_FIELD_PROCESS_ID)) {
        processId = (HANDLE)(UINT_PTR)InMetaValues->processId;
    }

    if (processId == NULL) {
        ClassifyOut->actionType = FWP_ACTION_PERMIT;
        return;
    }

    UINT32 packetBytes = NetMonitorCountBytes((NET_BUFFER_LIST*)LayerData);
    if (packetBytes == 0) {
        ClassifyOut->actionType = FWP_ACTION_PERMIT;
        return;
    }

    NmRateLimitDirection direction = (NmRateLimitDirection)NetMonitorGetDirectionForLayer(InFixedValues->layerId);

    if (NetMonitorRateLimitConsume(processId, direction, packetBytes)) {
        ClassifyOut->actionType = FWP_ACTION_PERMIT;
        return;
    }

    ClassifyOut->actionType = FWP_ACTION_BLOCK;
    ClassifyOut->rights &= ~FWPS_RIGHT_ACTION_WRITE;
}

static
void
NTAPI
NetMonitorFlowEstablishedClassify(
    _In_ const FWPS_INCOMING_VALUES0* InFixedValues,
    _In_ const FWPS_INCOMING_METADATA_VALUES0* InMetaValues,
    _Inout_opt_ void* LayerData,
    _In_opt_ const FWPS_FILTER0* Filter,
    _In_ UINT64 FlowContext,
    _Inout_ FWPS_CLASSIFY_OUT0* ClassifyOut
    )
{
    UNREFERENCED_PARAMETER(InFixedValues);
    UNREFERENCED_PARAMETER(LayerData);
    UNREFERENCED_PARAMETER(Filter);
    UNREFERENCED_PARAMETER(FlowContext);

    ClassifyOut->actionType = FWP_ACTION_PERMIT;

    if (!FWPS_IS_METADATA_FIELD_PRESENT(InMetaValues, FWPS_METADATA_FIELD_PROCESS_ID) ||
        !FWPS_IS_METADATA_FIELD_PRESENT(InMetaValues, FWPS_METADATA_FIELD_FLOW_HANDLE)) {
        return;
    }

    HANDLE processId = (HANDLE)(UINT_PTR)InMetaValues->processId;
    UINT64 flowHandle = InMetaValues->flowHandle;

    switch (InFixedValues->layerId) {
    case FWPS_LAYER_ALE_FLOW_ESTABLISHED_V4:
        if (gCallouts[0].RuntimeId != 0) {
            NetMonitorAssociateFlowContext(flowHandle,
                                           FWPS_LAYER_OUTBOUND_TRANSPORT_V4,
                                           gCallouts[0].RuntimeId,
                                           processId);
        }
        if (gCallouts[1].RuntimeId != 0) {
            NetMonitorAssociateFlowContext(flowHandle,
                                           FWPS_LAYER_INBOUND_TRANSPORT_V4,
                                           gCallouts[1].RuntimeId,
                                           processId);
        }
        break;
    case FWPS_LAYER_ALE_FLOW_ESTABLISHED_V6:
        if (gCallouts[2].RuntimeId != 0) {
            NetMonitorAssociateFlowContext(flowHandle,
                                           FWPS_LAYER_OUTBOUND_TRANSPORT_V6,
                                           gCallouts[2].RuntimeId,
                                           processId);
        }
        if (gCallouts[3].RuntimeId != 0) {
            NetMonitorAssociateFlowContext(flowHandle,
                                           FWPS_LAYER_INBOUND_TRANSPORT_V6,
                                           gCallouts[3].RuntimeId,
                                           processId);
        }
        break;
    default:
        break;
    }
}

static
NTSTATUS
NTAPI
NetMonitorNotify(
    _In_ FWPS_CALLOUT_NOTIFY_TYPE NotifyType,
    _In_ const GUID* FilterKey,
    _Inout_ FWPS_FILTER0* Filter
    )
{
    UNREFERENCED_PARAMETER(NotifyType);
    UNREFERENCED_PARAMETER(FilterKey);
    UNREFERENCED_PARAMETER(Filter);
    return STATUS_SUCCESS;
}

static
void
NTAPI
NetMonitorFlowDelete(
    _In_ UINT16 LayerId,
    _In_ UINT32 CalloutId,
    _In_ UINT64 FlowContext
    )
{
    UNREFERENCED_PARAMETER(LayerId);
    UNREFERENCED_PARAMETER(CalloutId);

    if (FlowContext != 0) {
        NETMONITOR_FLOW_CONTEXT* context = (NETMONITOR_FLOW_CONTEXT*)(UINT_PTR)FlowContext;
        ExFreePoolWithTag(context, NETMONITOR_POOL_TAG);
    }
}

static
NTSTATUS
NetMonitorAddFilter(
    _In_ const GUID* LayerGuid,
    _In_ const GUID* CalloutGuid,
    _In_ FWP_ACTION_TYPE ActionType,
    _In_z_ const wchar_t* Name
    )
{
    FWPM_FILTER0 filter;
    RtlZeroMemory(&filter, sizeof(filter));

    filter.displayData.name = (wchar_t*)Name;
    filter.layerKey = *LayerGuid;
    filter.subLayerKey = NETMONITOR_SUBLAYER_GUID;
    filter.action.type = ActionType;
    filter.action.calloutKey = *CalloutGuid;
    filter.weight.type = FWP_EMPTY;

    return FwpmFilterAdd0(gEngineHandle, &filter, NULL, NULL);
}

static
NTSTATUS
NetMonitorRegisterFlowEstablishedCallout(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ const GUID* CalloutGuid,
    _In_ const GUID* LayerGuid,
    _Out_ UINT32* RuntimeId
    )
{
    FWPS_CALLOUT0 callout;
    FWPM_CALLOUT0 managementCallout;
    NTSTATUS status;

    RtlZeroMemory(&callout, sizeof(callout));
    RtlZeroMemory(&managementCallout, sizeof(managementCallout));

    callout.calloutKey = *CalloutGuid;
    callout.classifyFn = NetMonitorFlowEstablishedClassify;
    callout.notifyFn = NetMonitorNotify;
    callout.flowDeleteFn = NULL;

    status = FwpsCalloutRegister0(DeviceObject, &callout, RuntimeId);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    managementCallout.calloutKey = *CalloutGuid;
    managementCallout.displayData.name = L"NetMonitor Flow Callout";
    managementCallout.applicableLayer = *LayerGuid;

    status = FwpmCalloutAdd0(gEngineHandle, &managementCallout, NULL, NULL);
    if (!NT_SUCCESS(status) && status != STATUS_FWP_ALREADY_EXISTS) {
        FwpsCalloutUnregisterById0(*RuntimeId);
        *RuntimeId = 0;
        return status;
    }

    status = NetMonitorAddFilter(LayerGuid,
                                 CalloutGuid,
                                 FWP_ACTION_CALLOUT_INSPECTION,
                                 L"NetMonitor Flow Filter");
    if (!NT_SUCCESS(status)) {
        FwpsCalloutUnregisterById0(*RuntimeId);
        *RuntimeId = 0;
    }

    return status;
}

NTSTATUS
NetMonitorRegisterCallouts(
    _In_ PDEVICE_OBJECT DeviceObject
    )
{
    NTSTATUS status;
    FWPS_CALLOUT0 callout;
    FWPM_SESSION0 session;
    FWPM_SUBLAYER0 subLayer;
    UINT32 index;

    RtlZeroMemory(&session, sizeof(session));
    session.flags = FWPM_SESSION_FLAG_DYNAMIC;

    status = FwpmEngineOpen0(NULL, RPC_C_AUTHN_DEFAULT, NULL, &session, &gEngineHandle);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = FwpmTransactionBegin0(gEngineHandle, 0);
    if (!NT_SUCCESS(status)) {
        FwpmEngineClose0(gEngineHandle);
        gEngineHandle = NULL;
        return status;
    }

    RtlZeroMemory(&subLayer, sizeof(subLayer));
    subLayer.subLayerKey = NETMONITOR_SUBLAYER_GUID;
    subLayer.displayData.name = L"NetMonitor Driver Sublayer";
    subLayer.weight = 0x100;
    status = FwpmSubLayerAdd0(gEngineHandle, &subLayer, NULL);
    if (!NT_SUCCESS(status) && status != STATUS_FWP_ALREADY_EXISTS) {
        FwpmTransactionAbort0(gEngineHandle);
        FwpmEngineClose0(gEngineHandle);
        gEngineHandle = NULL;
        return status;
    }

    for (index = 0; index < RTL_NUMBER_OF(gCallouts); ++index) {
        FWPM_CALLOUT0 managementCallout;
        RtlZeroMemory(&callout, sizeof(callout));
        RtlZeroMemory(&managementCallout, sizeof(managementCallout));

        callout.calloutKey = *gCallouts[index].CalloutGuid;
        callout.classifyFn = NetMonitorClassify;
        callout.notifyFn = NetMonitorNotify;
        callout.flowDeleteFn = NetMonitorFlowDelete;

        status = FwpsCalloutRegister0(DeviceObject, &callout, &gCallouts[index].RuntimeId);
        if (!NT_SUCCESS(status)) {
            break;
        }

        managementCallout.calloutKey = *gCallouts[index].CalloutGuid;
        managementCallout.displayData.name = L"NetMonitor Rate Limit Callout";
        managementCallout.applicableLayer = *gCallouts[index].LayerGuid;

        status = FwpmCalloutAdd0(gEngineHandle, &managementCallout, NULL, NULL);
        if (!NT_SUCCESS(status) && status != STATUS_FWP_ALREADY_EXISTS) {
            break;
        }

        status = NetMonitorAddFilter(gCallouts[index].LayerGuid,
                                     gCallouts[index].CalloutGuid,
                                     FWP_ACTION_CALLOUT_TERMINATING,
                                     L"NetMonitor Rate Limit Filter");
        if (!NT_SUCCESS(status)) {
            break;
        }
    }

    if (NT_SUCCESS(status)) {
        status = NetMonitorRegisterFlowEstablishedCallout(DeviceObject,
                                                          &NETMONITOR_CALLOUT_ALE_FLOW_ESTABLISHED_V4_GUID,
                                                          &FWPM_LAYER_ALE_FLOW_ESTABLISHED_V4,
                                                          &gAleFlowEstablishedV4Id);
    }

    if (NT_SUCCESS(status)) {
        status = NetMonitorRegisterFlowEstablishedCallout(DeviceObject,
                                                          &NETMONITOR_CALLOUT_ALE_FLOW_ESTABLISHED_V6_GUID,
                                                          &FWPM_LAYER_ALE_FLOW_ESTABLISHED_V6,
                                                          &gAleFlowEstablishedV6Id);
    }

    if (!NT_SUCCESS(status)) {
        FwpmTransactionAbort0(gEngineHandle);
        NetMonitorUnregisterCallouts();
        return status;
    }

    status = FwpmTransactionCommit0(gEngineHandle);
    if (!NT_SUCCESS(status)) {
        NetMonitorUnregisterCallouts();
    }

    return status;
}

void
NetMonitorUnregisterCallouts(
    void
    )
{
    UINT32 index;

    for (index = 0; index < RTL_NUMBER_OF(gCallouts); ++index) {
        if (gCallouts[index].RuntimeId != 0) {
            FwpsCalloutUnregisterById0(gCallouts[index].RuntimeId);
            gCallouts[index].RuntimeId = 0;
        }
    }

    if (gAleFlowEstablishedV4Id != 0) {
        FwpsCalloutUnregisterById0(gAleFlowEstablishedV4Id);
        gAleFlowEstablishedV4Id = 0;
    }

    if (gAleFlowEstablishedV6Id != 0) {
        FwpsCalloutUnregisterById0(gAleFlowEstablishedV6Id);
        gAleFlowEstablishedV6Id = 0;
    }

    if (gEngineHandle != NULL) {
        FwpmSubLayerDeleteByKey0(gEngineHandle, &NETMONITOR_SUBLAYER_GUID);
        FwpmEngineClose0(gEngineHandle);
        gEngineHandle = NULL;
    }
}
