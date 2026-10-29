#include "T2AudioMiniport.h"

#ifndef VT_ILLEGAL
#define VT_ILLEGAL 0xffff
#endif

// Topology miniport for audio endpoint creation
// Implements IMiniportTopology interface with minimal speaker node

typedef struct _T2AUDIO_TOPOLOGY {
    IMiniportTopology Interface;
    LONG ReferenceCount;
    PT2AUDIO_DEVICE_CONTEXT DeviceContext;
    PPORTTOPOLOGY Port;
    // TRUE for the headphone topology filter (Parameters\HeadphoneOutput):
    // pin 1 is a 3.5 mm jack with a jack description and change events.
    BOOLEAN Headphone;
} T2AUDIO_TOPOLOGY, *PT2AUDIO_TOPOLOGY;

// Data range for bridge pin (internal connection from WaveRT)
// This is a wildcard range that accepts any audio format
static KSDATARANGE g_TopologyBridgePinDataRange = {
    sizeof(KSDATARANGE),
    0,
    0,
    0,
    // KSDATAFORMAT_TYPE_AUDIO
    { STATIC_KSDATAFORMAT_TYPE_AUDIO },
    // KSDATAFORMAT_SUBTYPE_ANALOG
    { STATIC_KSDATAFORMAT_SUBTYPE_ANALOG },
    // KSDATAFORMAT_SPECIFIER_NONE
    { STATIC_KSDATAFORMAT_SPECIFIER_NONE }
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
    // KSDATAFORMAT_TYPE_AUDIO
    { STATIC_KSDATAFORMAT_TYPE_AUDIO },
    // KSDATAFORMAT_SUBTYPE_ANALOG
    { STATIC_KSDATAFORMAT_SUBTYPE_ANALOG },
    // KSDATAFORMAT_SPECIFIER_NONE
    { STATIC_KSDATAFORMAT_SPECIFIER_NONE }
};

static PKSDATARANGE g_TopologySpeakerPinDataRanges[] = {
    &g_TopologySpeakerPinDataRange
};

// Forward declarations
static PCFILTER_DESCRIPTOR g_T2AudioTopologyFilterDescriptor;
static PCFILTER_DESCRIPTOR g_T2AudioHeadphoneTopologyFilterDescriptor;

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
    PT2AUDIO_TOPOLOGY instance = CONTAINING_RECORD(
        Miniport, T2AUDIO_TOPOLOGY, Interface);

    if (Description == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Description = instance->Headphone ? &g_T2AudioHeadphoneTopologyFilterDescriptor
                                       : &g_T2AudioTopologyFilterDescriptor;
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
    // Headphone filter: keep the port's IPortEvents to signal jack changes
    // (T2AudioNotifyJackChange); released in the PnP stop path.
    if (instance->Headphone && Port != NULL && instance->DeviceContext != NULL &&
        instance->DeviceContext->HeadphonePortEvents == NULL) {
        PVOID events = NULL;

        if (NT_SUCCESS(Port->lpVtbl->QueryInterface((PVOID)Port, &IID_IPortEvents, &events)) &&
            events != NULL) {
            instance->DeviceContext->HeadphonePortEvents = events;
        }
    }
    KdPrint(("T2Audio: Topology Init success (headphone=%u)\n", instance->Headphone ? 1u : 0u));
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
    // Pin 0: bridge input from the WaveRT filter (physical connection).
    {
        1, 1, 0, NULL,
        {
            0, NULL,
            0, NULL,
            SIZEOF_ARRAY(g_TopologyBridgePinDataRanges),
            g_TopologyBridgePinDataRanges,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
    // Pin 1: speaker output (physical connector).
    {
        1, 1, 0, NULL,
        {
            0, NULL,
            0, NULL,
            SIZEOF_ARRAY(g_TopologySpeakerPinDataRanges),
            g_TopologySpeakerPinDataRanges,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_SPEAKER,
            NULL,
            0
        }
    }
};

// ---------------------------------------------------------------------------
// Headphone jack (ROADMAP 4.5): KSPROPERTY_JACK_DESCRIPTION(2) on the jack pin
// and KSEVENT_PINCAPS_JACKINFOCHANGE when the jack state changes. Windows
// then shows the endpoint as plugged in / unplugged. The state is only known
// with JackEvents=1 (jack polling); without it the jack reads "connected"
// and the endpoint is always available.
// ---------------------------------------------------------------------------
static BOOLEAN
T2AudioTopologyJackConnected(_In_opt_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    // Only a jack read that was once seen "plugged" is trusted; before that
    // (or without JackEvents) the endpoint is always shown.
    if (Context == NULL || !Context->JackEvents || !Context->JackSeenPlugged) {
        return TRUE;
    }
    return (Context->HeadphoneJackPlugged != 0) ? TRUE : FALSE;
}

static NTSTATUS
T2AudioTopologyBasicSupport(_Inout_ PPCPROPERTY_REQUEST Request)
{
    if (Request->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION) && Request->Value != NULL) {
        PKSPROPERTY_DESCRIPTION desc = (PKSPROPERTY_DESCRIPTION)Request->Value;

        desc->AccessFlags = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET;
        desc->DescriptionSize = sizeof(KSPROPERTY_DESCRIPTION);
        desc->PropTypeSet.Set = KSPROPTYPESETID_General;
        desc->PropTypeSet.Id = VT_ILLEGAL;
        desc->PropTypeSet.Flags = 0;
        desc->MembersListCount = 0;
        desc->Reserved = 0;
        Request->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
        return STATUS_SUCCESS;
    }
    if (Request->ValueSize >= sizeof(ULONG) && Request->Value != NULL) {
        *(PULONG)Request->Value = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET;
        Request->ValueSize = sizeof(ULONG);
        return STATUS_SUCCESS;
    }
    Request->ValueSize = 0;
    return STATUS_BUFFER_TOO_SMALL;
}

static NTSTATUS
T2AudioTopologyJackProperty(_Inout_ PPCPROPERTY_REQUEST Request)
{
    PT2AUDIO_TOPOLOGY instance;
    ULONG pinId = (ULONG)-1;
    ULONG needed;
    PKSMULTIPLE_ITEM item;

    if (Request == NULL || Request->PropertyItem == NULL || Request->MajorTarget == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    instance = CONTAINING_RECORD((PMINIPORTTOPOLOGY)Request->MajorTarget, T2AUDIO_TOPOLOGY, Interface);
    if (Request->InstanceSize >= sizeof(ULONG) && Request->Instance != NULL) {
        pinId = *(PULONG)Request->Instance;
    }
    if (!instance->Headphone || pinId != T2AUDIO_TOPO_PIN_HEADPHONE) {
        return STATUS_NOT_SUPPORTED;
    }
    if (Request->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        return T2AudioTopologyBasicSupport(Request);
    }
    if (!(Request->Verb & KSPROPERTY_TYPE_GET)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    needed = sizeof(KSMULTIPLE_ITEM) +
             ((Request->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION)
                  ? sizeof(KSJACK_DESCRIPTION) : sizeof(KSJACK_DESCRIPTION2));
    if (Request->ValueSize == 0) {
        Request->ValueSize = needed;
        return STATUS_BUFFER_OVERFLOW;
    }
    if (Request->ValueSize < needed || Request->Value == NULL) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    item = (PKSMULTIPLE_ITEM)Request->Value;
    item->Size = needed;
    item->Count = 1;
    if (Request->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION) {
        PKSJACK_DESCRIPTION jack = (PKSJACK_DESCRIPTION)(item + 1);

        RtlZeroMemory(jack, sizeof(*jack));
        jack->ChannelMapping = KSAUDIO_SPEAKER_STEREO;
        jack->Color = 0;
        jack->ConnectionType = eConnType3Point5mm;
        jack->GeoLocation = eGeoLocLeft;          // the MacBook Pro 16" jack is on the left
        jack->GenLocation = eGenLocPrimaryBox;
        jack->PortConnection = ePortConnJack;
        jack->IsConnected = T2AudioTopologyJackConnected(instance->DeviceContext);
    } else {
        PKSJACK_DESCRIPTION2 jack2 = (PKSJACK_DESCRIPTION2)(item + 1);

        RtlZeroMemory(jack2, sizeof(*jack2));
        jack2->DeviceStateInfo = 0;
        // Presence detection only when the driver actually tracks the jack.
        jack2->JackCapabilities = (instance->DeviceContext != NULL && instance->DeviceContext->JackEvents &&
                                   instance->DeviceContext->JackSeenPlugged)
                                      ? JACKDESC2_PRESENCE_DETECT_CAPABILITY : 0;
    }
    Request->ValueSize = needed;
    return STATUS_SUCCESS;
}

static NTSTATUS
T2AudioTopologyJackEvent(_In_ PPCEVENT_REQUEST Request)
{
    PT2AUDIO_TOPOLOGY instance;
    PPORTEVENTS events;

    if (Request == NULL || Request->MajorTarget == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    instance = CONTAINING_RECORD((PMINIPORTTOPOLOGY)Request->MajorTarget, T2AUDIO_TOPOLOGY, Interface);
    events = (instance->DeviceContext != NULL)
                 ? (PPORTEVENTS)instance->DeviceContext->HeadphonePortEvents : NULL;
    switch (Request->Verb) {
    case PCEVENT_VERB_ADD:
        if (!instance->Headphone || events == NULL || Request->EventEntry == NULL) {
            return STATUS_UNSUCCESSFUL;
        }
        events->lpVtbl->AddEventToEventList(events, Request->EventEntry);
        return STATUS_SUCCESS;
    case PCEVENT_VERB_REMOVE:
        // PortCls removes the entry from its list itself.
        return STATUS_SUCCESS;
    case PCEVENT_VERB_SUPPORT:
        return instance->Headphone ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
    default:
        return STATUS_INVALID_PARAMETER;
    }
}

static PCPROPERTY_ITEM g_T2AudioJackProperties[] = {
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION,
        PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT,
        T2AudioTopologyJackProperty
    },
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION2,
        PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT,
        T2AudioTopologyJackProperty
    }
};

static PCEVENT_ITEM g_T2AudioJackEvents[] = {
    {
        &KSEVENTSETID_PinCapsChange,
        KSEVENT_PINCAPS_JACKINFOCHANGE,
        KSEVENT_TYPE_ENABLE | KSEVENT_TYPE_BASICSUPPORT,
        T2AudioTopologyJackEvent
    }
};

static PCAUTOMATION_TABLE g_T2AudioJackAutomation = {
    sizeof(PCPROPERTY_ITEM),
    SIZEOF_ARRAY(g_T2AudioJackProperties),
    g_T2AudioJackProperties,
    sizeof(PCMETHOD_ITEM),
    0,
    NULL,
    sizeof(PCEVENT_ITEM),
    SIZEOF_ARRAY(g_T2AudioJackEvents),
    g_T2AudioJackEvents,
    0
};

// Called at PASSIVE_LEVEL when the polled jack state changed (BceTransport.c).
VOID
T2AudioNotifyJackChange(_In_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    PPORTEVENTS events;

    if (Context == NULL || !Context->HeadphoneFiltersRegistered) {
        return;
    }
    events = (PPORTEVENTS)Context->HeadphonePortEvents;
    if (events == NULL) {
        return;
    }
    events->lpVtbl->GenerateEventList(events, (GUID *)&KSEVENTSETID_PinCapsChange,
                                      KSEVENT_PINCAPS_JACKINFOCHANGE,
                                      TRUE, T2AUDIO_TOPO_PIN_HEADPHONE,
                                      FALSE, (ULONG)-1);
    KdPrint(("T2Audio: jack change signalled to Windows\n"));
}

// PnP stop/removal: drop the IPortEvents reference taken at Init.
VOID
T2AudioReleaseHeadphoneEvents(_Inout_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    PPORTEVENTS events;

    if (Context == NULL) {
        return;
    }
    events = (PPORTEVENTS)InterlockedExchangePointer(&Context->HeadphonePortEvents, NULL);
    if (events != NULL) {
        events->lpVtbl->Release(events);
    }
}

static PCPIN_DESCRIPTOR g_T2AudioHeadphoneTopologyPins[] = {
    // Pin 0: bridge input from the headphone WaveRT filter.
    {
        1, 1, 0, NULL,
        {
            0, NULL,
            0, NULL,
            SIZEOF_ARRAY(g_TopologyBridgePinDataRanges),
            g_TopologyBridgePinDataRanges,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
    // Pin 1: the 3.5 mm headphone jack.
    {
        1, 1, 0, &g_T2AudioJackAutomation,
        {
            0, NULL,
            0, NULL,
            SIZEOF_ARRAY(g_TopologySpeakerPinDataRanges),
            g_TopologySpeakerPinDataRanges,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_HEADPHONES,
            NULL,
            0
        }
    }
};

// Connections: bridge pin 0 -> speaker pin 1 (no intermediate nodes).
static PCCONNECTION_DESCRIPTOR g_T2AudioTopologyConnections[] = {
    { PCFILTER_NODE, T2AUDIO_TOPO_PIN_BRIDGE, PCFILTER_NODE, T2AUDIO_TOPO_PIN_SPEAKER }
};

static PCFILTER_DESCRIPTOR g_T2AudioTopologyFilterDescriptor = {
    0,
    NULL,
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(g_T2AudioTopologyPins),
    g_T2AudioTopologyPins,
    sizeof(PCNODE_DESCRIPTOR),
    0,
    NULL,
    SIZEOF_ARRAY(g_T2AudioTopologyConnections),
    g_T2AudioTopologyConnections,
    0,
    NULL
};

static PCFILTER_DESCRIPTOR g_T2AudioHeadphoneTopologyFilterDescriptor = {
    0,
    NULL,
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(g_T2AudioHeadphoneTopologyPins),
    g_T2AudioHeadphoneTopologyPins,
    sizeof(PCNODE_DESCRIPTOR),
    0,
    NULL,
    SIZEOF_ARRAY(g_T2AudioTopologyConnections),
    g_T2AudioTopologyConnections,
    0,
    NULL
};

NTSTATUS
T2AudioCreateTopology(
    _In_ PT2AUDIO_DEVICE_CONTEXT DeviceContext,
    _In_ BOOLEAN Headphone,
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
    instance->Headphone = Headphone;
    *Topology = &instance->Interface;
    
    KdPrint(("T2Audio: Topology miniport created\n"));
    return STATUS_SUCCESS;
}
