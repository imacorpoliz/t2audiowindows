#define INITGUID
#include "T2AudioMiniport.h"

static const GUID g_T2AudioTypeAudio = {
    0x73647561, 0x0000, 0x0010,
    { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};
static const GUID g_T2AudioSubtypePcm = {
    0x00000001, 0x0000, 0x0010,
    { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};
static const GUID g_T2AudioSpecifierWaveformex = {
    0x05589f81, 0xc356, 0x11ce,
    { 0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a }
};
static const GUID g_T2AudioCategoryAudio = {
    0x6994ad04, 0x93ef, 0x11d0,
    { 0xa3, 0xcc, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96 }
};
static const GUID g_T2AudioNodeSpeaker = {
    0xdff21ce1, 0xf70f, 0x11d0,
    { 0xb9, 0x17, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96 }
};

typedef struct _T2AUDIO_MINIPORT {
    IMiniportWaveRT Interface;
    LONG ReferenceCount;
    PT2AUDIO_DEVICE_CONTEXT DeviceContext;
    PPORTWAVERT Port;
} T2AUDIO_MINIPORT;

static NTSTATUS STDMETHODCALLTYPE
T2AudioMiniportQueryInterface(
    _In_ PMINIPORTWAVERT Unknown,
    _In_ REFIID InterfaceId,
    _COM_Outptr_ PVOID *Interface)
{
    PT2AUDIO_MINIPORT miniport = CONTAINING_RECORD(
        Unknown, T2AUDIO_MINIPORT, Interface);

    if (Interface == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *Interface = NULL;
    if (IsEqualGUIDAligned(InterfaceId, &IID_IUnknown) ||
        IsEqualGUIDAligned(InterfaceId, &IID_IMiniport) ||
        IsEqualGUIDAligned(InterfaceId, &IID_IMiniportWaveRT)) {
        *Interface = &miniport->Interface;
        InterlockedIncrement(&miniport->ReferenceCount);
        return STATUS_SUCCESS;
    }

    return STATUS_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE
T2AudioMiniportAddRef(_In_ PMINIPORTWAVERT Unknown)
{
    PT2AUDIO_MINIPORT miniport = CONTAINING_RECORD(
        Unknown, T2AUDIO_MINIPORT, Interface);
    return (ULONG)InterlockedIncrement(&miniport->ReferenceCount);
}

static ULONG STDMETHODCALLTYPE
T2AudioMiniportRelease(_In_ PMINIPORTWAVERT Unknown)
{
    PT2AUDIO_MINIPORT miniport = CONTAINING_RECORD(
        Unknown, T2AUDIO_MINIPORT, Interface);
    LONG references = InterlockedDecrement(&miniport->ReferenceCount);

    if (references == 0) {
        ExFreePoolWithTag(miniport, '2TAM');
    }
    return (ULONG)references;
}

static KSDATARANGE_AUDIO g_T2AudioSpeakerRange = {
    {
        sizeof(KSDATARANGE_AUDIO),
        0,
        0,
        0,
        STATIC_KSDATAFORMAT_TYPE_AUDIO,
        STATIC_KSDATAFORMAT_SUBTYPE_PCM,
        STATIC_KSDATAFORMAT_SPECIFIER_WAVEFORMATEX
    },
    6,
    32,
    32,
    48000,
    48000
};

static PKSDATARANGE g_T2AudioSpeakerRanges[] = {
    (PKSDATARANGE)&g_T2AudioSpeakerRange
};

static PCPIN_DESCRIPTOR g_T2AudioPins[] = {
    {
        1,
        1,
        1,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(g_T2AudioSpeakerRanges),
            g_T2AudioSpeakerRanges,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            &g_T2AudioCategoryAudio,
            &g_T2AudioNodeSpeaker,
            0
        }
    }
};

static PCFILTER_DESCRIPTOR g_T2AudioFilterDescriptor = {
    0,
    NULL,
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(g_T2AudioPins),
    g_T2AudioPins,
    0,
    0,
    NULL,
    0,
    NULL,
    0,
    NULL
};

static NTSTATUS STDMETHODCALLTYPE
T2AudioMiniportGetDescription(
    _In_ PMINIPORT Miniport,
    _Out_ PPCFILTER_DESCRIPTOR *Description)
{
    UNREFERENCED_PARAMETER(Miniport);
    if (Description == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Description = &g_T2AudioFilterDescriptor;
    return STATUS_SUCCESS;
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioMiniportDataRangeIntersection(
    _In_ PMINIPORT Miniport,
    _In_ ULONG PinId,
    _In_ PKSDATARANGE DataRange,
    _In_ PKSDATARANGE MatchingDataRange,
    _In_ ULONG OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength)
        PVOID ResultantFormat,
    _Out_ PULONG ResultantFormatLength)
{
    KSDATAFORMAT_WAVEFORMATEX format;

    UNREFERENCED_PARAMETER(Miniport);
    UNREFERENCED_PARAMETER(DataRange);
    UNREFERENCED_PARAMETER(MatchingDataRange);

    if (PinId != 0 || ResultantFormatLength == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *ResultantFormatLength = sizeof(format);
    if (OutputBufferLength < sizeof(format) || ResultantFormat == NULL) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(&format, sizeof(format));
    format.DataFormat.FormatSize = sizeof(format);
    format.DataFormat.MajorFormat = g_T2AudioTypeAudio;
    format.DataFormat.SubFormat = g_T2AudioSubtypePcm;
    format.DataFormat.Specifier = g_T2AudioSpecifierWaveformex;
    format.WaveFormatEx.wFormatTag = WAVE_FORMAT_PCM;
    format.WaveFormatEx.nChannels = 6;
    format.WaveFormatEx.nSamplesPerSec = 48000;
    format.WaveFormatEx.wBitsPerSample = 32;
    format.WaveFormatEx.nBlockAlign = 24;
    format.WaveFormatEx.nAvgBytesPerSec = 48000 * 24;

    RtlCopyMemory(ResultantFormat, &format, sizeof(format));
    return STATUS_SUCCESS;
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioMiniportInit(
    _In_ PMINIPORTWAVERT Miniport,
    _In_ PUNKNOWN UnknownAdapter,
    _In_ PRESOURCELIST ResourceList,
    _In_ PPORTWAVERT Port)
{
    PT2AUDIO_MINIPORT instance = CONTAINING_RECORD(
        Miniport, T2AUDIO_MINIPORT, Interface);

    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    instance->Port = Port;
    return STATUS_SUCCESS;
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioMiniportNewStream(
    _In_ PMINIPORTWAVERT Miniport,
    _Out_ PMINIPORTWAVERTSTREAM *Stream,
    _In_ PPORTWAVERTSTREAM PortStream,
    _In_ ULONG Pin,
    _In_ BOOLEAN Capture,
    _In_ PKSDATAFORMAT DataFormat)
{
    PT2AUDIO_MINIPORT instance = CONTAINING_RECORD(
        Miniport, T2AUDIO_MINIPORT, Interface);

    if (Stream == NULL || Capture || Pin != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    return T2AudioCreateStream(instance->DeviceContext, PortStream,
                               DataFormat, Stream);
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioMiniportGetDeviceDescription(
    _In_ PMINIPORTWAVERT Miniport,
    _Out_ PDEVICE_DESCRIPTION Description)
{
    UNREFERENCED_PARAMETER(Miniport);
    if (Description == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(Description, sizeof(*Description));
    Description->Version = DEVICE_DESCRIPTION_VERSION;
    Description->Master = TRUE;
    Description->ScatterGather = FALSE;
    Description->Dma32BitAddresses = TRUE;
    Description->Dma64BitAddresses = TRUE;
    Description->InterfaceType = PCIBus;
    return STATUS_SUCCESS;
}

static const IMiniportWaveRTVtbl g_T2AudioMiniportVtbl = {
    (void *)T2AudioMiniportQueryInterface,
    (void *)T2AudioMiniportAddRef,
    (void *)T2AudioMiniportRelease,
    (void *)T2AudioMiniportGetDescription,
    (void *)T2AudioMiniportDataRangeIntersection,
    (void *)T2AudioMiniportInit,
    (void *)T2AudioMiniportNewStream,
    (void *)T2AudioMiniportGetDeviceDescription
};

NTSTATUS
T2AudioValidateSixChannelFormat(_In_ PKSDATAFORMAT DataFormat)
{
    PKSDATAFORMAT_WAVEFORMATEX waveFormat;

    if (DataFormat == NULL ||
        DataFormat->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX) ||
        !IsEqualGUIDAligned(&DataFormat->MajorFormat,
                            &g_T2AudioTypeAudio) ||
        !IsEqualGUIDAligned(&DataFormat->SubFormat,
                            &g_T2AudioSubtypePcm) ||
        !IsEqualGUIDAligned(&DataFormat->Specifier,
                            &g_T2AudioSpecifierWaveformex)) {
        return STATUS_INVALID_PARAMETER;
    }

    waveFormat = (PKSDATAFORMAT_WAVEFORMATEX)DataFormat;
    if (waveFormat->WaveFormatEx.nChannels != 6 ||
        waveFormat->WaveFormatEx.nSamplesPerSec != 48000 ||
        waveFormat->WaveFormatEx.wBitsPerSample != 32 ||
        waveFormat->WaveFormatEx.nBlockAlign != 24) {
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
T2AudioCreateMiniport(
    _In_ PT2AUDIO_DEVICE_CONTEXT DeviceContext,
    _Out_ PMINIPORTWAVERT *Miniport)
{
    PT2AUDIO_MINIPORT instance;

    if (Miniport == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Miniport = NULL;

    instance = ExAllocatePoolWithTag(NonPagedPoolNx,
                                     sizeof(*instance), '2TAM');
    if (instance == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(instance, sizeof(*instance));
    instance->Interface.lpVtbl = &g_T2AudioMiniportVtbl;
    instance->ReferenceCount = 1;
    instance->DeviceContext = DeviceContext;
    *Miniport = &instance->Interface;
    return STATUS_SUCCESS;
}
