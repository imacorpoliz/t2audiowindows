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
static const GUID g_T2AudioSubtypeAnalog = {
    0x6dba3190, 0x67bd, 0x11cf,
    { 0xa0, 0xf7, 0x00, 0x20, 0xaf, 0xd1, 0x56, 0xe4 }
};
// KSDATAFORMAT_TYPE/SUBTYPE/SPECIFIER_WILDCARD are all GUID_NULL.
static const GUID g_T2AudioWildcard = {
    0x00000000, 0x0000, 0x0000,
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
};
static const GUID g_T2AudioSpecifierNone = {
    0x0f6417d6, 0xc318, 0x11d0,
    { 0xa4, 0x3f, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96 }
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

// Bridge pin data range: analog, no specifier. This must match the topology
// bridge pin range so that PcRegisterPhysicalConnection can wire the two
// filters together.
static KSDATARANGE g_T2AudioBridgeRange = {
    sizeof(KSDATARANGE),
    0,
    0,
    0,
    STATIC_KSDATAFORMAT_TYPE_AUDIO,
    STATIC_KSDATAFORMAT_SUBTYPE_ANALOG,
    STATIC_KSDATAFORMAT_SPECIFIER_NONE
};

static PKSDATARANGE g_T2AudioBridgeRanges[] = {
    &g_T2AudioBridgeRange
};

static PCPIN_DESCRIPTOR g_T2AudioPins[] = {
    // Pin 0: render streaming pin (sink). The audio engine opens this pin.
    {
        1,
        1,
        0,
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
            NULL,
            0
        }
    },
    // Pin 1: bridge pin to the topology filter (physical connection).
    {
        1,
        1,
        0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(g_T2AudioBridgeRanges),
            g_T2AudioBridgeRanges,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &g_T2AudioCategoryAudio,
            NULL,
            0
        }
    }
};

static PCCONNECTION_DESCRIPTOR g_T2AudioConnections[] = {
    { PCFILTER_NODE, T2AUDIO_WAVE_PIN_RENDER_SINK, PCFILTER_NODE, T2AUDIO_WAVE_PIN_BRIDGE }
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
    SIZEOF_ARRAY(g_T2AudioConnections),
    g_T2AudioConnections,
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

VOID
T2AudioLogFormat(
    _In_z_ PCSTR Tag,
    _In_opt_ PKSDATAFORMAT DataFormat)
{
    if (DataFormat == NULL) {
        KdPrint(("T2Audio: %s format=<NULL>\n", Tag));
        return;
    }

    if (DataFormat->FormatSize >= sizeof(KSDATAFORMAT_WAVEFORMATEX)) {
        PKSDATAFORMAT_WAVEFORMATEX wf = (PKSDATAFORMAT_WAVEFORMATEX)DataFormat;
        KdPrint(("T2Audio: %s size=%u major=%08X sub=%08X spec=%08X wtag=%u ch=%u rate=%u bits=%u block=%u\n",
                 Tag, DataFormat->FormatSize,
                 DataFormat->MajorFormat.Data1,
                 DataFormat->SubFormat.Data1,
                 DataFormat->Specifier.Data1,
                 wf->WaveFormatEx.wFormatTag,
                 wf->WaveFormatEx.nChannels,
                 wf->WaveFormatEx.nSamplesPerSec,
                 wf->WaveFormatEx.wBitsPerSample,
                 wf->WaveFormatEx.nBlockAlign));
    } else {
        KdPrint(("T2Audio: %s size=%u major=%08X sub=%08X spec=%08X\n",
                 Tag, DataFormat->FormatSize,
                 DataFormat->MajorFormat.Data1,
                 DataFormat->SubFormat.Data1,
                 DataFormat->Specifier.Data1));
    }
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
    PKSDATAFORMAT_WAVEFORMATEXTENSIBLE result;

    UNREFERENCED_PARAMETER(Miniport);
    UNREFERENCED_PARAMETER(MatchingDataRange);

    KdPrint(("T2Audio: DataRangeIntersection pin=%u outLen=%u\n",
             PinId, OutputBufferLength));

    if (ResultantFormatLength == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *ResultantFormatLength = 0;

    if (PinId == T2AUDIO_WAVE_PIN_BRIDGE) {
        // Analog bridge: let PortCls run its default intersection.
        return STATUS_NOT_IMPLEMENTED;
    }
    if (PinId != T2AUDIO_WAVE_PIN_RENDER_SINK || DataRange == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    // The client range must be audio/PCM with a WAVEFORMATEX specifier (or
    // wildcards) and, if it is an audio range, cover 6ch/32-bit/48 kHz.
    if (!IsEqualGUIDAligned(&DataRange->MajorFormat, &g_T2AudioTypeAudio) &&
        !IsEqualGUIDAligned(&DataRange->MajorFormat, &g_T2AudioWildcard)) {
        return STATUS_NO_MATCH;
    }
    if (!IsEqualGUIDAligned(&DataRange->SubFormat, &g_T2AudioSubtypePcm) &&
        !IsEqualGUIDAligned(&DataRange->SubFormat, &g_T2AudioWildcard)) {
        return STATUS_NO_MATCH;
    }
    if (!IsEqualGUIDAligned(&DataRange->Specifier, &g_T2AudioSpecifierWaveformex) &&
        !IsEqualGUIDAligned(&DataRange->Specifier, &g_T2AudioWildcard)) {
        return STATUS_NO_MATCH;
    }
    // A client may pass either a range (KSDATARANGE_AUDIO, exactly its size)
    // or a concrete format (KSDATAFORMAT_WAVEFORMATEX[TENSIBLE]). Telling them
    // apart by size matters: reading a format's WAVEFORMATEX as range limits
    // would match against garbage. A concrete format must be one we accept.
    if (IsEqualGUIDAligned(&DataRange->Specifier, &g_T2AudioSpecifierWaveformex) &&
        DataRange->FormatSize != sizeof(KSDATARANGE_AUDIO) &&
        DataRange->FormatSize >= sizeof(KSDATAFORMAT_WAVEFORMATEX)) {
        if (!NT_SUCCESS(T2AudioValidateSixChannelFormat((PKSDATAFORMAT)DataRange))) {
            return STATUS_NO_MATCH;
        }
    } else if (IsEqualGUIDAligned(&DataRange->Specifier, &g_T2AudioSpecifierWaveformex) &&
               DataRange->FormatSize == sizeof(KSDATARANGE_AUDIO)) {
        PKSDATARANGE_AUDIO audioRange = (PKSDATARANGE_AUDIO)DataRange;

        if (audioRange->MaximumChannels < 6 ||
            audioRange->MinimumBitsPerSample > 32 ||
            audioRange->MaximumBitsPerSample < 32 ||
            audioRange->MinimumSampleFrequency > 48000 ||
            audioRange->MaximumSampleFrequency < 48000) {
            return STATUS_NO_MATCH;
        }
    }

    // Size query, or a buffer too small for the result.
    if (OutputBufferLength == 0) {
        *ResultantFormatLength = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
        return STATUS_BUFFER_OVERFLOW;
    }
    if (OutputBufferLength < sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE) ||
        ResultantFormat == NULL) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    // Multichannel formats must be described with WAVEFORMATEXTENSIBLE; a
    // plain 6-channel WAVE_FORMAT_PCM has no channel mask and the audio engine
    // does not accept it for a 5.1 endpoint.
    result = (PKSDATAFORMAT_WAVEFORMATEXTENSIBLE)ResultantFormat;
    RtlZeroMemory(result, sizeof(*result));
    result->DataFormat.FormatSize = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
    result->DataFormat.Flags = 0;
    result->DataFormat.SampleSize = 24;
    result->DataFormat.Reserved = 0;
    result->DataFormat.MajorFormat = g_T2AudioTypeAudio;
    result->DataFormat.SubFormat = g_T2AudioSubtypePcm;
    result->DataFormat.Specifier = g_T2AudioSpecifierWaveformex;
    result->WaveFormatExt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    result->WaveFormatExt.Format.nChannels = 6;
    result->WaveFormatExt.Format.nSamplesPerSec = 48000;
    result->WaveFormatExt.Format.wBitsPerSample = 32;
    result->WaveFormatExt.Format.nBlockAlign = 24;
    result->WaveFormatExt.Format.nAvgBytesPerSec = 48000 * 24;
    result->WaveFormatExt.Format.cbSize =
        sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    result->WaveFormatExt.Samples.wValidBitsPerSample = 32;
    result->WaveFormatExt.dwChannelMask = KSAUDIO_SPEAKER_5POINT1_SURROUND;
    result->WaveFormatExt.SubFormat = g_T2AudioSubtypePcm;

    *ResultantFormatLength = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
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
    NTSTATUS status;

    KdPrint(("T2Audio: NewStream pin=%u capture=%u\n", Pin, Capture ? 1u : 0u));
    T2AudioLogFormat("NewStream req", DataFormat);

    if (Stream == NULL || Capture || Pin != T2AUDIO_WAVE_PIN_RENDER_SINK) {
        KdPrint(("T2Audio: NewStream rejected: bad pin/capture\n"));
        return STATUS_INVALID_PARAMETER;
    }
    status = T2AudioCreateStream(instance->DeviceContext, PortStream,
                                 DataFormat, Stream);
    KdPrint(("T2Audio: NewStream status=0x%08X\n", status));
    return status;
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
        DataFormat->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX)) {
        KdPrint(("T2Audio: Validate: bad size/null\n"));
        return STATUS_INVALID_PARAMETER;
    }

    if (!IsEqualGUIDAligned(&DataFormat->MajorFormat,
                            &g_T2AudioTypeAudio)) {
        KdPrint(("T2Audio: Validate: major=%08X != audio\n",
                 DataFormat->MajorFormat.Data1));
        return STATUS_INVALID_PARAMETER;
    }
    if (!IsEqualGUIDAligned(&DataFormat->SubFormat, &g_T2AudioSubtypePcm)) {
        return STATUS_INVALID_PARAMETER;
    }

    // WAVEFORMATEXTENSIBLE uses the same WAVEFORMATEX specifier; there is no
    // separate "extensible" specifier. (05589f82-... accepted here before is
    // DirectShow's FORMAT_MPEGVideo, not an audio layout.)
    if (!IsEqualGUIDAligned(&DataFormat->Specifier,
                            &g_T2AudioSpecifierWaveformex)) {
        KdPrint(("T2Audio: Validate: spec=%08X != WFEX\n",
                 DataFormat->Specifier.Data1));
        return STATUS_INVALID_PARAMETER;
    }

    waveFormat = (PKSDATAFORMAT_WAVEFORMATEX)DataFormat;

    // Support both WAVEFORMATEXTENSIBLE (standard for 6-channel audio) and WAVEFORMATEX
    if (waveFormat->WaveFormatEx.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        PKSDATAFORMAT_WAVEFORMATEXTENSIBLE wfext;

        if (DataFormat->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE)) {
            KdPrint(("T2Audio: Validate: wfext size=%u < %u\n",
                     DataFormat->FormatSize,
                     (ULONG)sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE)));
            return STATUS_INVALID_PARAMETER;
        }

        wfext = (PKSDATAFORMAT_WAVEFORMATEXTENSIBLE)DataFormat;
        if (wfext->WaveFormatExt.Format.nChannels != 6 ||
            wfext->WaveFormatExt.Format.nSamplesPerSec != 48000 ||
            wfext->WaveFormatExt.Format.wBitsPerSample != 32 ||
            wfext->WaveFormatExt.Format.nBlockAlign != 24 ||
            wfext->WaveFormatExt.Format.nAvgBytesPerSec != 48000 * 24 ||
            wfext->WaveFormatExt.Format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) ||
            wfext->WaveFormatExt.Samples.wValidBitsPerSample == 0 ||
            wfext->WaveFormatExt.Samples.wValidBitsPerSample > 32 ||
            (wfext->WaveFormatExt.dwChannelMask != KSAUDIO_SPEAKER_5POINT1_SURROUND &&
             wfext->WaveFormatExt.dwChannelMask != KSAUDIO_SPEAKER_5POINT1)) {
            KdPrint(("T2Audio: Validate: wfext ch=%u rate=%u bits=%u block=%u\n",
                     wfext->WaveFormatExt.Format.nChannels,
                     wfext->WaveFormatExt.Format.nSamplesPerSec,
                     wfext->WaveFormatExt.Format.wBitsPerSample,
                     wfext->WaveFormatExt.Format.nBlockAlign));
            return STATUS_INVALID_PARAMETER;
        }

        if (!IsEqualGUIDAligned(&wfext->WaveFormatExt.SubFormat,
                                &g_T2AudioSubtypePcm)) {
            KdPrint(("T2Audio: Validate: wfext subformat != PCM\n"));
            return STATUS_INVALID_PARAMETER;
        }

        return STATUS_SUCCESS;
    } else if (waveFormat->WaveFormatEx.wFormatTag == WAVE_FORMAT_PCM) {
        if (waveFormat->WaveFormatEx.nChannels != 6 ||
            waveFormat->WaveFormatEx.nSamplesPerSec != 48000 ||
            waveFormat->WaveFormatEx.wBitsPerSample != 32 ||
            waveFormat->WaveFormatEx.nBlockAlign != 24 ||
            waveFormat->WaveFormatEx.nAvgBytesPerSec != 48000 * 24) {
            KdPrint(("T2Audio: Validate: wfx ch=%u rate=%u bits=%u block=%u != 6/48000/32/24\n",
                     waveFormat->WaveFormatEx.nChannels,
                     waveFormat->WaveFormatEx.nSamplesPerSec,
                     waveFormat->WaveFormatEx.wBitsPerSample,
                     waveFormat->WaveFormatEx.nBlockAlign));
            return STATUS_INVALID_PARAMETER;
        }

        return STATUS_SUCCESS;
    }

    KdPrint(("T2Audio: Validate: unsupported format tag %u\n",
             waveFormat->WaveFormatEx.wFormatTag));
    return STATUS_INVALID_PARAMETER;
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
