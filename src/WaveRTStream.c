#include "T2AudioMiniport.h"

typedef struct _T2AUDIO_WAVERT_STREAM {
    IMiniportWaveRTStream Interface;
    LONG ReferenceCount;
    PT2AUDIO_DEVICE_CONTEXT DeviceContext;
    PPORTWAVERTSTREAM PortStream;
    KSSTATE State;
    ULONG SampleRate;
    ULONG BytesPerFrame;
    ULONG64 AnchorQpc;
    ULONG64 AnchorFrames;
} T2AUDIO_WAVERT_STREAM;

static NTSTATUS STDMETHODCALLTYPE
T2AudioStreamQueryInterface(
    _In_ PMINIPORTWAVERTSTREAM Unknown,
    _In_ REFIID InterfaceId,
    _COM_Outptr_ PVOID *Interface)
{
    PT2AUDIO_WAVERT_STREAM stream = CONTAINING_RECORD(
        Unknown, T2AUDIO_WAVERT_STREAM, Interface);

    if (Interface == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Interface = NULL;
    if (IsEqualGUIDAligned(InterfaceId, &IID_IUnknown) ||
        IsEqualGUIDAligned(InterfaceId, &IID_IMiniportWaveRTStream)) {
        *Interface = &stream->Interface;
        InterlockedIncrement(&stream->ReferenceCount);
        return STATUS_SUCCESS;
    }
    return STATUS_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE
T2AudioStreamAddRef(_In_ PMINIPORTWAVERTSTREAM Unknown)
{
    PT2AUDIO_WAVERT_STREAM stream = CONTAINING_RECORD(
        Unknown, T2AUDIO_WAVERT_STREAM, Interface);
    return (ULONG)InterlockedIncrement(&stream->ReferenceCount);
}

static ULONG STDMETHODCALLTYPE
T2AudioStreamRelease(_In_ PMINIPORTWAVERTSTREAM Unknown)
{
    PT2AUDIO_WAVERT_STREAM stream = CONTAINING_RECORD(
        Unknown, T2AUDIO_WAVERT_STREAM, Interface);
    LONG references = InterlockedDecrement(&stream->ReferenceCount);

    if (references == 0) {
        ExFreePoolWithTag(stream, '2TAS');
    }
    return (ULONG)references;
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioStreamSetFormat(
    _In_ PMINIPORTWAVERTSTREAM Stream,
    _In_ PKSDATAFORMAT DataFormat)
{
    PT2AUDIO_WAVERT_STREAM instance = CONTAINING_RECORD(
        Stream, T2AUDIO_WAVERT_STREAM, Interface);
    NTSTATUS status = T2AudioValidateSixChannelFormat(DataFormat);

    if (NT_SUCCESS(status)) {
        instance->SampleRate = 48000;
        instance->BytesPerFrame = 24;
    }
    return status;
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioStreamSetState(
    _In_ PMINIPORTWAVERTSTREAM Stream,
    _In_ KSSTATE State)
{
    PT2AUDIO_WAVERT_STREAM instance = CONTAINING_RECORD(
        Stream, T2AUDIO_WAVERT_STREAM, Interface);

    if (State < KSSTATE_STOP || State > KSSTATE_RUN) {
        return STATUS_INVALID_PARAMETER;
    }
    if (State == KSSTATE_RUN && instance->State != KSSTATE_RUN) {
        NTSTATUS status = T2AudioStartIo(instance->DeviceContext);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    } else if (State == KSSTATE_STOP && instance->State != KSSTATE_STOP) {
        NTSTATUS status = T2AudioStopIo(instance->DeviceContext);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }
    instance->State = State;
    if (State == KSSTATE_RUN) {
        LARGE_INTEGER qpcFreq;
        instance->AnchorQpc = KeQueryPerformanceCounter(&qpcFreq).QuadPart;
        instance->AnchorFrames = 0;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioStreamGetPosition(
    _In_ PMINIPORTWAVERTSTREAM Stream,
    _Out_ PKSAUDIO_POSITION Position)
{
    PT2AUDIO_WAVERT_STREAM instance = CONTAINING_RECORD(
        Stream, T2AUDIO_WAVERT_STREAM, Interface);
    LARGE_INTEGER qpc, qpcFreq;
    ULONG64 elapsedTicks, elapsedFrames, playOffset;

    if (Position == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (instance->State != KSSTATE_RUN) {
        Position->PlayOffset = 0;
        Position->WriteOffset = 0;
        return STATUS_SUCCESS;
    }

    qpc = KeQueryPerformanceCounter(&qpcFreq);

    // Calculate elapsed frames using QPC interpolation
    elapsedTicks = qpc.QuadPart - instance->AnchorQpc;
    elapsedFrames = (elapsedTicks * instance->SampleRate) / qpcFreq.QuadPart;

    playOffset = (instance->AnchorFrames + elapsedFrames) * instance->BytesPerFrame;
    playOffset %= instance->DeviceContext->SpeakerBufferSize;

    Position->PlayOffset = playOffset;
    // WriteOffset is ahead of PlayOffset by a reasonable FIFO size (512 frames = ~10.6ms at 48kHz)
    Position->WriteOffset = (playOffset + 512 * instance->BytesPerFrame) %
                            instance->DeviceContext->SpeakerBufferSize;

    return STATUS_SUCCESS;
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioStreamAllocateAudioBuffer(
    _In_ PMINIPORTWAVERTSTREAM Stream,
    _In_ ULONG RequestedSize,
    _Out_ PMDL *AudioBufferMdl,
    _Out_ ULONG *ActualSize,
    _Out_ ULONG *OffsetFromFirstPage,
    _Out_ MEMORY_CACHING_TYPE *CacheType)
{
    PT2AUDIO_WAVERT_STREAM instance = CONTAINING_RECORD(
        Stream, T2AUDIO_WAVERT_STREAM, Interface);
    NTSTATUS status;

    if (AudioBufferMdl == NULL || ActualSize == NULL ||
        OffsetFromFirstPage == NULL || CacheType == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    // Diagnostic mode: BCE transport is disabled (SpeakerDeviceId == 0).
    // Do not expose hardware buffer memory to PortCls; refuse the stream
    // and always initialize the out parameters.
    if (instance->DeviceContext->SpeakerDeviceId == 0) {
        *AudioBufferMdl = NULL;
        *ActualSize = 0;
        *OffsetFromFirstPage = 0;
        *CacheType = MmNonCached;
        KdPrint(("T2Audio: AllocateAudioBuffer blocked: diagnostic mode (no hardware MDL)\n"));
        return STATUS_NOT_SUPPORTED;
    }

    status = T2AudioCreateSpeakerMdl(instance->DeviceContext,
                                     AudioBufferMdl,
                                     OffsetFromFirstPage,
                                     ActualSize);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    *CacheType = MmWriteCombined;
    KdPrint(("T2Audio: AllocateAudioBuffer: MDL=%p size=%u offset=%u\n",
             *AudioBufferMdl, *ActualSize, *OffsetFromFirstPage));
    return STATUS_SUCCESS;
}

static VOID STDMETHODCALLTYPE
T2AudioStreamFreeAudioBuffer(
    _In_ PMINIPORTWAVERTSTREAM Stream,
    _In_opt_ PMDL AudioBufferMdl,
    _In_ ULONG BufferSize)
{
    PT2AUDIO_WAVERT_STREAM instance = CONTAINING_RECORD(
        Stream, T2AUDIO_WAVERT_STREAM, Interface);

    UNREFERENCED_PARAMETER(BufferSize);
    UNREFERENCED_PARAMETER(AudioBufferMdl);
    
    T2AudioFreeSpeakerMdl(instance->DeviceContext);
    KdPrint(("T2Audio: FreeAudioBuffer\n"));
}

static VOID STDMETHODCALLTYPE
T2AudioStreamGetHWLatency(
    _In_ PMINIPORTWAVERTSTREAM Stream,
    _Out_ KSRTAUDIO_HWLATENCY *Latency)
{
    UNREFERENCED_PARAMETER(Stream);
    if (Latency != NULL) {
        RtlZeroMemory(Latency, sizeof(*Latency));
        // Estimated FIFO: 512 frames * 24 bytes/frame = 12288 bytes (~10.6ms at 48kHz)
        Latency->FifoSize = 12288;
        Latency->ChipsetDelay = 0;
        Latency->CodecDelay = 0;
    }
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioStreamGetPositionRegister(
    _In_ PMINIPORTWAVERTSTREAM Stream,
    _Out_ KSRTAUDIO_HWREGISTER *Register)
{
    UNREFERENCED_PARAMETER(Stream);
    if (Register != NULL) {
        RtlZeroMemory(Register, sizeof(*Register));
    }
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioStreamGetClockRegister(
    _In_ PMINIPORTWAVERTSTREAM Stream,
    _Out_ KSRTAUDIO_HWREGISTER *Register)
{
    UNREFERENCED_PARAMETER(Stream);
    if (Register != NULL) {
        RtlZeroMemory(Register, sizeof(*Register));
    }
    return STATUS_NOT_SUPPORTED;
}

static const IMiniportWaveRTStreamVtbl g_T2AudioStreamVtbl = {
    (void *)T2AudioStreamQueryInterface,
    (void *)T2AudioStreamAddRef,
    (void *)T2AudioStreamRelease,
    (void *)T2AudioStreamSetFormat,
    (void *)T2AudioStreamSetState,
    (void *)T2AudioStreamGetPosition,
    (void *)T2AudioStreamAllocateAudioBuffer,
    (void *)T2AudioStreamFreeAudioBuffer,
    (void *)T2AudioStreamGetHWLatency,
    (void *)T2AudioStreamGetPositionRegister,
    (void *)T2AudioStreamGetClockRegister
};

NTSTATUS
T2AudioCreateStream(
    _In_ PT2AUDIO_DEVICE_CONTEXT DeviceContext,
    _In_ PPORTWAVERTSTREAM PortStream,
    _In_ PKSDATAFORMAT DataFormat,
    _Out_ PMINIPORTWAVERTSTREAM *Stream)
{
    PT2AUDIO_WAVERT_STREAM instance;
    NTSTATUS status;

    if (Stream == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Stream = NULL;
    status = T2AudioValidateSixChannelFormat(DataFormat);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    instance = ExAllocatePoolWithTag(NonPagedPoolNx,
                                     sizeof(*instance), '2TAS');
    if (instance == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(instance, sizeof(*instance));
    instance->Interface.lpVtbl = &g_T2AudioStreamVtbl;
    instance->ReferenceCount = 1;
    instance->DeviceContext = DeviceContext;
    instance->PortStream = PortStream;
    instance->SampleRate = 48000;
    instance->BytesPerFrame = 24;
    instance->State = KSSTATE_STOP;
    *Stream = &instance->Interface;
    return STATUS_SUCCESS;
}
