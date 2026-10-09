#include "T2AudioMiniport.h"
#include "T2AudioBufferLogic.h"

// How often the system-memory WaveRT buffer is mirrored into the BAR1 hardware
// buffer, in milliseconds (KeSetTimerEx period). The hardware buffer holds
// ~347 ms of audio at 48 kHz/6ch/24-bit, so a few ms keeps the mirror fresh.
#define T2AUDIO_COPY_PERIOD_MS 2

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
    // TRUE only after this stream successfully issued START_IO. A forced
    // system-buffer (software) stream keeps this FALSE, so leaving RUN never
    // sends an unmatched STOP_IO.
    BOOLEAN HardwareIoStarted;
    PVOID SystemBufferVa;
    ULONG SourceSize;
    ULONG CopySize;
    KTIMER CopyTimer;
    KDPC CopyDpc;
    BOOLEAN TimerActive;
    // Memory-mapped position register handed to the WaveRT port. The audio
    // engine reads it directly to learn how far the "hardware" has played; the
    // copy DPC advances it at the sample rate. Value is the play cursor in bytes.
    volatile ULONG PositionRegister;
    ULONG GetPositionCalls;
    ULONG GetPositionRegCalls;
    ULONG CopyTickCalls;
} T2AUDIO_WAVERT_STREAM;

static VOID T2AudioStreamStopCopyTimer(_Inout_ PT2AUDIO_WAVERT_STREAM instance);

// Byte play cursor derived from QPC, anchored when the stream entered RUN.
// Used both to answer GetPosition and to advance the memory-mapped position
// register the audio engine polls. Returns 0 when the stream is not running.
static ULONG
T2AudioStreamPlayPositionBytes(_In_ PT2AUDIO_WAVERT_STREAM instance)
{
    LARGE_INTEGER qpc, qpcFreq;
    ULONG64 elapsedTicks, elapsedFrames, playOffset;
    ULONG bufferSize = instance->AudioBufferSize;

    if (instance->State != KSSTATE_RUN || bufferSize == 0 ||
        instance->BytesPerFrame == 0 || instance->SampleRate == 0) {
        return 0;
    }

    qpc = KeQueryPerformanceCounter(&qpcFreq);
    elapsedTicks = (ULONG64)qpc.QuadPart - instance->AnchorQpc;
    elapsedFrames = (elapsedTicks * instance->SampleRate) / (ULONG64)qpcFreq.QuadPart;
    playOffset = (instance->AnchorFrames + elapsedFrames) * instance->BytesPerFrame;
    return (ULONG)(playOffset % bufferSize);
}

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

    // A foreign, unknown, or already-released MDL is not ours to release. Leave
    // the stream completely untouched - including its copy timer - so a bogus
    // FreeAudioBuffer cannot stop a live stream.
    if (action == T2AUDIO_RELEASE_NONE) {
        return;
    }

    // Stop mirroring before the source or destination can go away.
    T2AudioStreamStopCopyTimer(instance);

    if (action == T2AUDIO_RELEASE_HARDWARE) {
        T2AudioFreeSpeakerMdl(instance->DeviceContext);
    } else { // T2AUDIO_RELEASE_SYSTEM
        if (instance->PortStream != NULL) {
            instance->PortStream->lpVtbl->FreePagesFromMdl(
                (PVOID)instance->PortStream, instance->AudioBufferMdl);
        }
    }

    instance->AudioBufferMdl = NULL;
    instance->AudioBufferSize = 0;
    instance->HardwareBuffer = FALSE;
    instance->SystemBufferVa = NULL;
    instance->SourceSize = 0;
    instance->CopySize = 0;
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

// Periodic DPC that mirrors the system-memory WaveRT buffer into the BAR1
// hardware buffer. PortCls refuses to expose BAR memory to user mode, so the
// client writes into ordinary system RAM and this copy delivers the samples to
// the hardware buffer the T2 audio engine reads from.
static VOID
T2AudioStreamCopyTick(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PT2AUDIO_WAVERT_STREAM instance = (PT2AUDIO_WAVERT_STREAM)DeferredContext;
    PT2AUDIO_DEVICE_CONTEXT context;
    PUCHAR destination;
    PUCHAR source;
    ULONG sourceSize;
    ULONG remaining;
    ULONG sourceOffset;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    if (instance == NULL) {
        return;
    }
    context = instance->DeviceContext;
    instance->CopyTickCalls++;

    // Mirror the system buffer into the BAR1 hardware buffer, but ONLY when the
    // current test session explicitly enabled device-memory writes
    // (MmioCopyEnabled, set from the volatile TestSession key). On a normal boot
    // the key is absent, so this copy never runs and the driver cannot corrupt
    // device memory. Only write when the whole copy provably lands inside the
    // located speaker buffer (and inside the mapped BAR1): a stale or oversized
    // CopySize must never scribble past the hardware buffer into adjacent
    // device memory.
    if (instance->SystemBufferVa != NULL && instance->CopySize != 0 &&
        instance->SourceSize != 0 && context != NULL &&
        context->HardwareReady && context->MmioCopyEnabled &&
        context->Bar1Mapped != NULL &&
        context->SpeakerBufferSize != 0 &&
        instance->CopySize <= context->SpeakerBufferSize &&
        context->SpeakerBufferOffset <= context->Bar1Size &&
        instance->CopySize <= context->Bar1Size - context->SpeakerBufferOffset) {
        // Tile the client's (possibly smaller) buffer across the whole hardware
        // buffer so the engine never reads stale bytes, whatever portion of the
        // hardware ring it happens to be consuming.
        destination = (PUCHAR)context->Bar1Mapped + context->SpeakerBufferOffset;
        source = (PUCHAR)instance->SystemBufferVa;
        sourceSize = instance->SourceSize;
        remaining = instance->CopySize;
        sourceOffset = 0;

        while (remaining > 0) {
            ULONG chunk = sourceSize - sourceOffset;
            if (chunk > remaining) {
                chunk = remaining;
            }
            RtlCopyMemory(destination, source + sourceOffset, chunk);
            destination += chunk;
            remaining -= chunk;
            sourceOffset += chunk;
            if (sourceOffset >= sourceSize) {
                sourceOffset = 0;
            }
        }
    }

    // Advance the memory-mapped play cursor the audio engine reads, in every
    // mode, so the engine can drain the buffer.
    instance->PositionRegister = T2AudioStreamPlayPositionBytes(instance);
}

static VOID
T2AudioStreamStartCopyTimer(_Inout_ PT2AUDIO_WAVERT_STREAM instance)
{
    LARGE_INTEGER dueTime;

    // First fire after 1 ms, then every T2AUDIO_COPY_PERIOD_MS.
    dueTime.QuadPart = -10000;
    KeSetTimerEx(&instance->CopyTimer, dueTime, T2AUDIO_COPY_PERIOD_MS,
                 &instance->CopyDpc);
    instance->TimerActive = TRUE;
}

static VOID
T2AudioStreamStopCopyTimer(_Inout_ PT2AUDIO_WAVERT_STREAM instance)
{
    if (instance->TimerActive) {
        KeCancelTimer(&instance->CopyTimer);
        // Wait for a tick that may already be executing before the caller frees
        // anything the DPC touches.
        KeFlushQueuedDpcs();
        instance->TimerActive = FALSE;
    }
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioStreamSetFormat(
    _In_ PMINIPORTWAVERTSTREAM Stream,
    _In_ PKSDATAFORMAT DataFormat)
{
    PT2AUDIO_WAVERT_STREAM instance = CONTAINING_RECORD(
        Stream, T2AUDIO_WAVERT_STREAM, Interface);
    NTSTATUS status = T2AudioValidateSixChannelFormat(DataFormat);

    T2AudioLogFormat("SetFormat", DataFormat);
    KdPrint(("T2Audio: SetFormat status=0x%08X\n", status));

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

    KdPrint(("T2Audio: SetState %u (forceSys=%u bceIo=%u irql=%u)\n", State,
             instance->DeviceContext->ForceSystemBuffer ? 1u : 0u,
             instance->DeviceContext->BceIoEnabled ? 1u : 0u,
             (ULONG)KeGetCurrentIrql()));

    if (State < KSSTATE_STOP || State > KSSTATE_RUN) {
        return STATUS_INVALID_PARAMETER;
    }
    if (State == KSSTATE_RUN) {
        // Only act on the STOP/PAUSE -> RUN edge; a redundant RUN is a no-op.
        if (instance->State != KSSTATE_RUN) {
            NTSTATUS status;

            // A stream may only run once a buffer has been allocated.
            if (instance->AudioBufferMdl == NULL) {
                return STATUS_INVALID_DEVICE_STATE;
            }

            // Hardware I/O only in the real device path. ForceSystemBuffer (or a
            // stream with no wired speaker) must never issue START_IO, and a
            // stream that already started it must not issue it twice. The
            // EnableBceIo gate keeps stage 4 able to wire the speaker without
            // sending any command.
            if (instance->DeviceContext->BceIoEnabled &&
                T2AudioDecideHardwareIo(
                    instance->DeviceContext->ForceSystemBuffer ? 1u : 0u,
                    (instance->DeviceContext->SpeakerDeviceId != 0) ? 1u : 0u,
                    instance->HardwareIoStarted ? 1u : 0u)) {
                KdPrint(("T2Audio: SetState RUN about to StartIo irql=%u\n",
                         (ULONG)KeGetCurrentIrql()));
                status = T2AudioStartIo(instance->DeviceContext);
                KdPrint(("T2Audio: SetState RUN StartIo status=0x%08X\n", status));
                if (!NT_SUCCESS(status)) {
                    return status;
                }
                instance->HardwareIoStarted = TRUE;
            }

            // Begin mirroring the system buffer into the hardware buffer and
            // advancing the play cursor. In diagnostic mode the DPC skips the
            // BAR1 copy but the cursor still advances so the engine drains.
            if (T2AudioShouldStartCopyTimer(
                    (instance->SystemBufferVa != NULL) ? 1u : 0u,
                    (instance->CopySize != 0) ? 1u : 0u,
                    instance->TimerActive ? 1u : 0u)) {
                KdPrint(("T2Audio: SetState RUN starting copy timer va=%p copy=%u\n",
                         instance->SystemBufferVa, instance->CopySize));
                T2AudioStreamStartCopyTimer(instance);
            } else {
                KdPrint(("T2Audio: SetState RUN no timer va=%p copy=%u\n",
                         instance->SystemBufferVa, instance->CopySize));
            }

            // Anchor the play cursor on entry to RUN.
            {
                LARGE_INTEGER qpcFreq;
                instance->AnchorQpc = KeQueryPerformanceCounter(&qpcFreq).QuadPart;
                instance->AnchorFrames = 0;
                instance->PositionRegister = 0;
            }
        }
    } else {
        // PAUSE / ACQUIRE / STOP: stop mirroring, and stop hardware I/O only if
        // this stream actually started it. A forced/software stream never
        // started it and must not issue an unmatched STOP_IO.
        KdPrint(("T2Audio: SetState %u posCalls=%u regCalls=%u ticks=%u posReg=%u\n",
                 State, instance->GetPositionCalls,
                 instance->GetPositionRegCalls, instance->CopyTickCalls,
                 instance->PositionRegister));
        T2AudioStreamStopCopyTimer(instance);
        if (T2AudioDecideStopHardwareIo(instance->HardwareIoStarted ? 1u : 0u)) {
            NTSTATUS status;
            KdPrint(("T2Audio: SetState %u about to StopIo irql=%u\n",
                     State, (ULONG)KeGetCurrentIrql()));
            status = T2AudioStopIo(instance->DeviceContext);
            if (!NT_SUCCESS(status)) {
                // The STOP_IO failed (for example the BCE transport was poisoned
                // after a send timeout). This must not wedge the stream in RUN
                // with the timer already stopped: clear the started flag so we do
                // not retry a request the transport can no longer carry, log it,
                // and still complete the state transition below.
                KdPrint(("T2Audio: SetState %u StopIo failed: 0x%08X (continuing)\n",
                         State, status));
            }
            instance->HardwareIoStarted = FALSE;
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
    ULONG playOffset;
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

    instance->GetPositionCalls++;
    playOffset = T2AudioStreamPlayPositionBytes(instance);

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

    KdPrint(("T2Audio: AllocateAudioBuffer req=%u aligned=%u speakerId=0x%I64X forceSys=%u\n",
             RequestedSize, alignedSize,
             instance->DeviceContext->SpeakerDeviceId,
             instance->DeviceContext->ForceSystemBuffer ? 1u : 0u));

    // The WaveRT buffer must live in system memory. PortCls maps it to user
    // mode with MmMapLockedPagesSpecifyCache(UserMode, ...), which cannot map
    // device (BAR) pages -- an MMIO MDL is rejected by the port and the stream
    // fails to initialize (0x8007001F). Instead, hand out an ordinary
    // system-memory cyclic buffer and mirror it into the BAR1 hardware buffer
    // from a periodic DPC (T2AudioStreamCopyTick).
    {
        PHYSICAL_ADDRESS highAddress;
        PMDL mdl;
        PVOID systemVa;
        ULONG byteCount;
        ULONG desiredSize = alignedSize;

        if (instance->PortStream == NULL) {
            return STATUS_INVALID_DEVICE_STATE;
        }

        // Hand out exactly the client-requested (frame-aligned) size. Growing
        // the system buffer to the hardware buffer size made PortCls reject the
        // stream at Initialize (0x8007001F): the hardware size (0x61800) is not
        // page-aligned and the port would not map it. The copy DPC mirrors the
        // client buffer into the BAR1 ring instead.

        highAddress.QuadPart = (LONGLONG)-1;
        mdl = instance->PortStream->lpVtbl->AllocatePagesForMdl(
                  (PVOID)instance->PortStream, highAddress, desiredSize);
        if (mdl == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        byteCount = MmGetMdlByteCount(mdl);
        if (byteCount < desiredSize) {
            // The allocator returned less than requested (it may allocate
            // fewer pages under pressure); do not hand out a short buffer.
            instance->PortStream->lpVtbl->FreePagesFromMdl(
                (PVOID)instance->PortStream, mdl);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        systemVa = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
        if (systemVa == NULL) {
            instance->PortStream->lpVtbl->FreePagesFromMdl(
                (PVOID)instance->PortStream, mdl);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        // Start from silence so the hardware buffer is well-defined until the
        // client writes real samples.
        RtlZeroMemory(systemVa, desiredSize);

        *AudioBufferMdl = mdl;
        *ActualSize = desiredSize;
        *OffsetFromFirstPage = 0;
        *CacheType = MmCached;
        instance->HardwareBuffer = FALSE;
        instance->SystemBufferVa = systemVa;
        instance->SourceSize = desiredSize;
        // Copy exactly the client buffer each tick. Tiling across the (larger)
        // hardware ring would saturate the bus from a DPC, so keep the mirror
        // light and revisit if the engine needs the whole ring primed.
        instance->CopySize = desiredSize;
    }

    instance->AudioBufferMdl = *AudioBufferMdl;
    instance->AudioBufferSize = *ActualSize;

    KdPrint(("T2Audio: AllocateAudioBuffer: %s MDL=%p size=%u offset=%u src=%u copy=%u\n",
             instance->HardwareBuffer ? "MMIO" : "system",
             *AudioBufferMdl, *ActualSize, *OffsetFromFirstPage,
             instance->SourceSize, instance->CopySize));
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

    // Release only the MDL this stream handed out. A NULL, foreign, or
    // already-released MDL is ignored. T2AudioStreamReleaseBuffer(NULL) forces
    // release and is reserved for internal teardown, so it must not be reachable
    // from here with a NULL argument.
    if (AudioBufferMdl != NULL && AudioBufferMdl == instance->AudioBufferMdl) {
        T2AudioStreamReleaseBuffer(instance, AudioBufferMdl);
    }
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
    PT2AUDIO_WAVERT_STREAM instance = CONTAINING_RECORD(
        Stream, T2AUDIO_WAVERT_STREAM, Interface);

    if (Register == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    instance->GetPositionRegCalls++;
    RtlZeroMemory(Register, sizeof(*Register));

    // Expose a memory-mapped play cursor (bytes) that the audio engine reads
    // directly. The copy DPC keeps it advancing at the sample rate; without it
    // the engine never drains the buffer and the stream stays silent.
    Register->Register = (PVOID)&instance->PositionRegister;
    Register->Width = 32;
    // For a position register the WDK requires only Register, Width and
    // Accuracy; Numerator/Denominator are clock-register specific and stay zero.
    // Accuracy is the maximum error of a reading expressed in BYTES, not in time
    // units: the DPC refreshes the cursor every T2AUDIO_COPY_PERIOD_MS, so the
    // worst-case error is one refresh worth of audio.
    {
        ULONG framesPerPeriod =
            (instance->SampleRate * T2AUDIO_COPY_PERIOD_MS) / 1000;
        ULONG accuracy = framesPerPeriod * instance->BytesPerFrame;
        if (accuracy == 0) {
            accuracy = instance->BytesPerFrame; // at least one frame
        }
        Register->Accuracy = accuracy;
    }
    return STATUS_SUCCESS;
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
    KeInitializeTimer(&instance->CopyTimer);
    KeInitializeDpc(&instance->CopyDpc, T2AudioStreamCopyTick, instance);
    *Stream = &instance->Interface;
    return STATUS_SUCCESS;
}
