#include "T2AudioMiniport.h"

// Topology miniport for audio endpoint creation
// Implements IMiniportTopology interface with minimal speaker node

typedef struct _T2AUDIO_TOPOLOGY {
    IMiniportTopology Interface;
    LONG ReferenceCount;
    PT2AUDIO_DEVICE_CONTEXT DeviceContext;
    PPORTTOPOLOGY Port;
} T2AUDIO_TOPOLOGY, *PT2AUDIO_TOPOLOGY;

// Data range for bridge pin (internal connection from WaveRT)
// This is a wildcard range that accepts any audio format
static KSDATARANGE g_TopologyBridgePinDataRange = {
    sizeof(KSDATARANGE),
    0,
    0,
    0,
    {STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO)},
    {STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG)},
    {STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)}
};

static PKSDATARANGE g_TopologyBridgePinDataRanges[] = {
    &g_TopologyBridgePinDataRange
};

// Data range for speaker pin (analog output to physical speaker)
static KSDATARANGE g_TopologySpeakerPinDataRange = {
    sizeof(KSDATARANGE),
    0,
    0,
    0,
    {STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO)},
    {STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG)},
    {STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)}
};

static PKSDATARANGE g_TopologySpeakerPinDataRanges[] = {
    &g_TopologySpeakerPinDataRange
};

// Forward declarations
static PCFILTER_DESCRIPTOR g_T2AudioTopologyFilterDescriptor;

static NTSTATUS STDMETHODCALLTYPE
T2AudioTopologyQueryInterface(
    _In_ PMINIPORTTOPOLOGY Unknown,
    _In_ REFIID InterfaceId,
    _COM_Outptr_ PVOID *Interface)
{
    PT2AUDIO_TOPOLOGY topology = CONTAINING_RECORD(
        Unknown, T2AUDIO_TOPOLOGY, Interface);

    if (Interface == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *Interface = NULL;
    if (IsEqualGUIDAligned(InterfaceId, &IID_IUnknown) ||
        IsEqualGUIDAligned(InterfaceId, &IID_IMiniport) ||
        IsEqualGUIDAligned(InterfaceId, &IID_IMiniportTopology)) {
        *Interface = &topology->Interface;
        InterlockedIncrement(&topology->ReferenceCount);
        return STATUS_SUCCESS;
    }

    return STATUS_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE
T2AudioTopologyAddRef(_In_ PMINIPORTTOPOLOGY Unknown)
{
    PT2AUDIO_TOPOLOGY topology = CONTAINING_RECORD(
        Unknown, T2AUDIO_TOPOLOGY, Interface);
    return (ULONG)InterlockedIncrement(&topology->ReferenceCount);
}

static ULONG STDMETHODCALLTYPE
T2AudioTopologyRelease(_In_ PMINIPORTTOPOLOGY Unknown)
{
    PT2AUDIO_TOPOLOGY topology = CONTAINING_RECORD(
        Unknown, T2AUDIO_TOPOLOGY, Interface);
    LONG references = InterlockedDecrement(&topology->ReferenceCount);

    if (references == 0) {
        ExFreePoolWithTag(topology, '2TAT');
    }
    return (ULONG)references;
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioTopologyGetDescription(
    _In_ PMINIPORT Miniport,
    _Out_ PPCFILTER_DESCRIPTOR *Description)
{
    UNREFERENCED_PARAMETER(Miniport);
    if (Description == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Description = &g_T2AudioTopologyFilterDescriptor;
    return STATUS_SUCCESS;
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioTopologyDataRangeIntersection(
    _In_ PMINIPORT Miniport,
    _In_ ULONG PinId,
    _In_ PKSDATARANGE DataRange,
    _In_ PKSDATARANGE MatchingDataRange,
    _In_ ULONG OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength)
        PVOID ResultantFormat,
    _Out_ PULONG ResultantFormatLength)
{
    UNREFERENCED_PARAMETER(Miniport);
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(DataRange);
    UNREFERENCED_PARAMETER(MatchingDataRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);
    UNREFERENCED_PARAMETER(ResultantFormatLength);
    
    // Topology has no data ranges
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioTopologyInit(
    _In_ PMINIPORTTOPOLOGY Miniport,
    _In_ PUNKNOWN UnknownAdapter,
    _In_ PRESOURCELIST ResourceList,
    _In_ PPORTTOPOLOGY Port)
{
    PT2AUDIO_TOPOLOGY instance = CONTAINING_RECORD(
        Miniport, T2AUDIO_TOPOLOGY, Interface);

    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    
    instance->Port = Port;
    KdPrint(("T2Audio: Topology Init success\n"));
    return STATUS_SUCCESS;
}

static const IMiniportTopologyVtbl g_T2AudioTopologyVtbl = {
    (void *)T2AudioTopologyQueryInterface,
    (void *)T2AudioTopologyAddRef,
    (void *)T2AudioTopologyRelease,
    (void *)T2AudioTopologyGetDescription,
    (void *)T2AudioTopologyDataRangeIntersection,
    (void *)T2AudioTopologyInit
};

// Pin definitions: Pin 0 = WaveRT bridge, Pin 1 = Speaker output
static PCPIN_DESCRIPTOR g_T2AudioTopologyPins[] = {
    // Pin 0: Input from WaveRT (bridge pin - no category, internal connection)
    {
        1, 1, 0, NULL,  // Max 1 instance globally, per filter; min 0
        {
            0, NULL,
            0, NULL,
            SIZEOF_ARRAY(g_TopologyBridgePinDataRanges),
            g_TopologyBridgePinDataRanges,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            NULL,  // Bridge pin has no category
            NULL,  // Bridge pin has no name
            0
        }
    },
    // Pin 1: Output to speaker (physical connector)
    {
        1, 1, 0, NULL,  // Max 1 instance globally, per filter; min 0
        {
            0, NULL,
            0, NULL,
            SIZEOF_ARRAY(g_TopologySpeakerPinDataRanges),
            g_TopologySpeakerPinDataRanges,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            &KSNODETYPE_SPEAKER,
            0
        }
    }
};

// Node 0: Speaker node
static PCNODE_DESCRIPTOR g_T2AudioTopologyNodes[] = {
    {
        0,
        NULL,
        &KSNODETYPE_SPEAKER,
        NULL
    }
};

// Connections: Pin 0 -> Node 0 (speaker) -> Pin 1
static PCCONNECTION_DESCRIPTOR g_T2AudioTopologyConnections[] = {
    { PCFILTER_NODE, 0, 0, 0 },  // Filter Pin 0 -> Node 0 input
    { 0, 0, PCFILTER_NODE, 1 }   // Node 0 output -> Filter Pin 1
};

static PCFILTER_DESCRIPTOR g_T2AudioTopologyFilterDescriptor = {
    0,
    NULL,
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(g_T2AudioTopologyPins),
    g_T2AudioTopologyPins,
    sizeof(PCNODE_DESCRIPTOR),
    SIZEOF_ARRAY(g_T2AudioTopologyNodes),
    g_T2AudioTopologyNodes,
    SIZEOF_ARRAY(g_T2AudioTopologyConnections),
    g_T2AudioTopologyConnections,
    0,
    NULL
};

NTSTATUS
T2AudioCreateTopology(
    _In_ PT2AUDIO_DEVICE_CONTEXT DeviceContext,
    _Out_ PMINIPORTTOPOLOGY *Topology)
{
    PT2AUDIO_TOPOLOGY instance;

    if (Topology == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Topology = NULL;

    instance = ExAllocatePoolWithTag(NonPagedPoolNx,
                                     sizeof(*instance), '2TAT');
    if (instance == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(instance, sizeof(*instance));
    instance->Interface.lpVtbl = &g_T2AudioTopologyVtbl;
    instance->ReferenceCount = 1;
    instance->DeviceContext = DeviceContext;
    *Topology = &instance->Interface;
    
    KdPrint(("T2Audio: Topology miniport created\n"));
    return STATUS_SUCCESS;
}
