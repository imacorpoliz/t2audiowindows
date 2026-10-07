#include "T2AudioMiniport.h"
#include "T2AudioBufferLogic.h"

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
    PMDL AudioBufferMdl;
    ULONG AudioBufferSize;
    BOOLEAN HardwareBuffer;
    BOOLEAN HardwareStarted;
} T2AUDIO_WAVERT_STREAM;

// Release the buffer currently held by the stream (if any). `expected` is the
// MDL passed to FreeAudioBuffer; pass NULL to release unconditionally (stream
// teardown). A foreign or unknown MDL is ignored and never routed to an
// allocator. The WaveRT port stream outlives the miniport stream (PortCls owns
// the miniport stream), so freeing here is safe.
static VOID
T2AudioStreamReleaseBuffer(
    _Inout_ PT2AUDIO_WAVERT_STREAM instance,
    _In_opt_ PMDL expected)
{
    ULONG action = T2AudioDecideBufferRelease(
        instance->AudioBufferMdl != NULL,
        instance->HardwareBuffer,
        (expected == NULL) || (expected == instance->AudioBufferMdl));

    if (action == T2AUDIO_RELEASE_HARDWARE) {
        T2AudioFreeSpeakerMdl(instance->DeviceContext);
    } else if (action == T2AUDIO_RELEASE_SYSTEM) {
        if (instance->PortStream != NULL) {
            instance->PortStream->lpVtbl->FreePagesFromMdl(
                (PVOID)instance->PortStream, instance->AudioBufferMdl);
        }
    } else {
        // Nothing held, or a foreign/already-released MDL: leave state intact.
        return;
    }

    instance->AudioBufferMdl = NULL;
    instance->AudioBufferSize = 0;
    instance->HardwareBuffer = FALSE;
}

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
        // Defensive: PortCls normally calls FreeAudioBuffer first, but release
        // any buffer still held so a failed/partial lifecycle cannot leak it.
        T2AudioStreamReleaseBuffer(stream, NULL);
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
    if (State == KSSTATE_RUN) {
        // (Re)start hardware I/O only if it is not already running.
        if (!instance->HardwareStarted) {
            NTSTATUS status;

            // A stream may only run once a buffer has been allocated.
            if (instance->AudioBufferMdl == NULL) {
                return STATUS_INVALID_DEVICE_STATE;
            }
            // Hardware playback must use the hardware (MMIO) buffer. If a BCE
            // device id appeared after a system buffer was allocated, refuse to
            // start hardware I/O with the wrong buffer.
            if (instance->DeviceContext->SpeakerDeviceId != 0 &&
                !instance->HardwareBuffer) {
                return STATUS_INVALID_DEVICE_STATE;
            }
            status = T2AudioStartIo(instance->DeviceContext);
            if (!NT_SUCCESS(status)) {
                return status;
            }
            instance->HardwareStarted = TRUE;
        }
        if (instance->State != KSSTATE_RUN) {
            LARGE_INTEGER qpcFreq;
            instance->AnchorQpc = KeQueryPerformanceCounter(&qpcFreq).QuadPart;
            instance->AnchorFrames = 0;
        }
    } else {
        // PAUSE / ACQUIRE / STOP: stop hardware I/O only if it was actually
        // started. This avoids issuing a STOP that was never paired with a
        // successful start (e.g. a RUN that failed in diagnostic mode).
        if (instance->HardwareStarted) {
            NTSTATUS status = T2AudioStopIo(instance->DeviceContext);
            if (!NT_SUCCESS(status)) {
                return status;
            }
            instance->HardwareStarted = FALSE;
        }
    }
    instance->State = State;
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
    ULONG bufferSize;

    if (Position == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    bufferSize = instance->AudioBufferSize;
    if (instance->State != KSSTATE_RUN || bufferSize == 0) {
        Position->PlayOffset = 0;
        Position->WriteOffset = 0;
        return STATUS_SUCCESS;
    }

    qpc = KeQueryPerformanceCounter(&qpcFreq);

    // Calculate elapsed frames using QPC interpolation
    elapsedTicks = qpc.QuadPart - instance->AnchorQpc;
    elapsedFrames = (elapsedTicks * instance->SampleRate) / qpcFreq.QuadPart;

    playOffset = (instance->AnchorFrames + elapsedFrames) * instance->BytesPerFrame;
    playOffset %= bufferSize;

    Position->PlayOffset = playOffset;
    // WriteOffset is ahead of PlayOffset by a reasonable FIFO size (512 frames = ~10.6ms at 48kHz)
    Position->WriteOffset = (playOffset + 512 * instance->BytesPerFrame) % bufferSize;

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
    ULONG alignedSize;
    NTSTATUS status;

    if (AudioBufferMdl == NULL || ActualSize == NULL ||
        OffsetFromFirstPage == NULL || CacheType == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *AudioBufferMdl = NULL;
    *ActualSize = 0;
    *OffsetFromFirstPage = 0;
    *CacheType = MmCached;

    if (RequestedSize == 0 || instance->BytesPerFrame == 0 ||
        RequestedSize < instance->BytesPerFrame) {
        return STATUS_INVALID_PARAMETER;
    }

    // WaveRT requires the actual buffer to be at least the requested size.
    // Round up to a whole number of frames, guarding against overflow.
    if (!T2AudioAlignSizeUpToFrame(RequestedSize, instance->BytesPerFrame,
                                   &alignedSize)) {
        return STATUS_INVALID_PARAMETER;
    }

    // Only one buffer may be outstanding per stream.
    if (instance->AudioBufferMdl != NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (instance->DeviceContext->SpeakerDeviceId != 0) {
        // BCE is ready: expose the BAR1 speaker buffer as a zero-copy MMIO
        // WaveRT buffer (matches AppleAudio.sys). The hardware buffer has a
        // fixed size, so it must be able to satisfy the request.
        status = T2AudioCreateSpeakerMdl(instance->DeviceContext,
                                         AudioBufferMdl,
                                         OffsetFromFirstPage,
                                         ActualSize);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        if (!T2AudioHardwareBufferSatisfies(RequestedSize, *ActualSize,
                                            instance->BytesPerFrame)) {
            // The hardware buffer cannot satisfy this request (too small or
            // not frame-aligned). Release it and fail rather than report a
            // buffer smaller than the client asked for.
            T2AudioFreeSpeakerMdl(instance->DeviceContext);
            *AudioBufferMdl = NULL;
            *ActualSize = 0;
            *OffsetFromFirstPage = 0;
            return STATUS_UNSUCCESSFUL;
        }
        *CacheType = MmWriteCombined;
        instance->HardwareBuffer = TRUE;
    } else {
        // No BCE device id yet (diagnostic mode): allocate an ordinary
        // system-memory cyclic buffer through the WaveRT port so the endpoint
        // stays testable without touching device memory. Audio I/O is still
        // gated by T2AudioStartIo/StopIo, so nothing reaches the hardware.
        PHYSICAL_ADDRESS highAddress;
        PMDL mdl;
        ULONG byteCount;

        if (instance->PortStream == NULL) {
            return STATUS_INVALID_DEVICE_STATE;
        }

        highAddress.QuadPart = (LONGLONG)-1;
        mdl = instance->PortStream->lpVtbl->AllocatePagesForMdl(
                  (PVOID)instance->PortStream, highAddress, alignedSize);
        if (mdl == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        byteCount = MmGetMdlByteCount(mdl);
        if (byteCount < alignedSize) {
            // The allocator returned less than requested (it may allocate
            // fewer pages under pressure); do not hand out a short buffer.
            instance->PortStream->lpVtbl->FreePagesFromMdl(
                (PVOID)instance->PortStream, mdl);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        *AudioBufferMdl = mdl;
        *ActualSize = alignedSize;
        *OffsetFromFirstPage = 0;
        *CacheType = MmCached;
        instance->HardwareBuffer = FALSE;
    }

    instance->AudioBufferMdl = *AudioBufferMdl;
    instance->AudioBufferSize = *ActualSize;

    KdPrint(("T2Audio: AllocateAudioBuffer: %s MDL=%p size=%u offset=%u\n",
             instance->HardwareBuffer ? "MMIO" : "system",
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

    // Releases only the MDL this stream handed out; a NULL, foreign, or
    // already-released MDL is ignored (see T2AudioStreamReleaseBuffer).
    T2AudioStreamReleaseBuffer(instance, AudioBufferMdl);
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
