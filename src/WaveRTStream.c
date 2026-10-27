#include "T2AudioMiniport.h"
#include "T2AudioBufferLogic.h"
#include "T2AudioSpeakerLogic.h"
#include <intrin.h>

// How often the system-memory WaveRT buffer is mirrored into the BAR1 hardware
// buffer, in milliseconds (KeSetTimerEx period). The hardware buffer holds
// ~347 ms of audio at 48 kHz/6ch/24-bit, so a few ms keeps the mirror fresh.
#define T2AUDIO_COPY_PERIOD_MS 2

// How far (in frames) the data written into the BAR1 ring runs ahead of the
// T2 read position: 1000 frames (~21 ms at 48 kHz), AppleAudio's target lead.
// At RUN the T2 is assumed to start reading at slot 0; every UPDATE_TIMESTAMP
// then re-checks the lead against the T2's real read position and re-aims it
// when it drifts out of +-T2AUDIO_CLOCK_TOLERANCE (see
// docs/analysis/RE_speaker_layout_and_clock.md).
#define T2AUDIO_LEAD_FRAMES T2AUDIO_CLOCK_TARGET_LEAD

typedef struct _T2AUDIO_WAVERT_STREAM {
    IMiniportWaveRTStream Interface;
    LONG ReferenceCount;
    PT2AUDIO_DEVICE_CONTEXT DeviceContext;
    PPORTWAVERTSTREAM PortStream;
    KSSTATE State;
    ULONG SampleRate;
    // Client format: 2 channels (stereo, laid out over the six speakers) or 6
    // (raw ring channels, diagnostic). BytesPerFrame is the client frame size.
    ULONG Channels;
    ULONG BytesPerFrame;
    KSPIN_LOCK StreamLock;
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
    // Consumed-frame cursor: every source frame before it has already been
    // mirrored into BAR1 (or skipped when MMIO copy is off). This is the play
    // position reported to the audio engine, so the engine never reuses a
    // part of the client buffer that has not been copied yet. Protected by
    // StreamLock.
    ULONG64 LastCopiedFrame;
    // Source frame N is written to ring frame N + DestOffset (mod ring), kept
    // in 0..ringFrames-1. Re-aimed from the T2 clock; protected by StreamLock.
    ULONG64 DestOffset;
    // ClockGeneration of the last T2 anchor this stream checked its lead with.
    ULONG ClockGenerationSeen;
    // Host time (QPC ns) just before this run's START_IO; older anchors
    // belong to the previous run.
    ULONG64 RunStartNs;
    ULONG ClockCorrections;
    // Tweeter high-pass state of the stereo path; reset on a fresh run.
    T2AUDIO_SPEAKER_STATE SpeakerState;
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

// Zero Count ring frames starting at ring frame FirstFrame (mod HwFrames).
// Caller holds the device's HardwareLock and has validated the mapping.
static VOID
T2AudioStreamZeroRingLocked(
    _In_ PUCHAR HwBufferBase,
    _In_ ULONG HwFrames,
    _In_ ULONG64 FirstFrame,
    _In_ ULONG64 Count)
{
    ULONG64 zeroFrames;

    if (HwFrames == 0) {
        return;
    }
    zeroFrames = (Count < HwFrames) ? Count : HwFrames;
    while (zeroFrames != 0) {
        ULONG slot = (ULONG)(FirstFrame % HwFrames);
        ULONG run = HwFrames - slot;

        if (run > zeroFrames) {
            run = (ULONG)zeroFrames;
        }
        RtlZeroMemory(HwBufferBase + (SIZE_T)slot * T2AUDIO_RING_BYTES_PER_FRAME,
                      (SIZE_T)run * T2AUDIO_RING_BYTES_PER_FRAME);
        FirstFrame += run;
        zeroFrames -= run;
    }
}

// If a new T2 clock anchor arrived since the last check, compare the lead of
// the next write (source frame NextFrame) over the T2's current read position
// and re-aim DestOffset when it left the tolerance window, as AppleAudio does.
// Ring slots skipped by a forward jump are silenced. Caller holds StreamLock
// and HardwareLock (lock order StreamLock -> HardwareLock -> ClockLock).
static VOID
T2AudioStreamFollowT2ClockLocked(
    _Inout_ PT2AUDIO_WAVERT_STREAM instance,
    _In_ PT2AUDIO_DEVICE_CONTEXT context,
    _In_ PUCHAR HwBufferBase,
    _In_ ULONG HwFrames,
    _In_ ULONG64 NextFrame,
    _In_ ULONG64 NowQpc)
{
    LARGE_INTEGER qpcFreq;
    ULONG generation;
    ULONG64 anchorFrames;
    ULONG64 anchorQpc;
    ULONG64 anchorNs;
    ULONG64 readFrame;
    ULONG64 newOffset;
    LONGLONG jump;

    if (HwFrames == 0 || !instance->HardwareIoStarted) {
        return;
    }
    KeAcquireSpinLockAtDpcLevel(&context->ClockLock);
    generation = context->ClockGeneration;
    anchorFrames = context->ClockAnchorFrames;
    anchorQpc = context->ClockAnchorQpc;
    anchorNs = context->ClockAnchorNs;
    KeReleaseSpinLockFromDpcLevel(&context->ClockLock);

    if (generation == instance->ClockGenerationSeen) {
        return;
    }
    instance->ClockGenerationSeen = generation;
    // A timestamp of the previous run delivered after this START_IO: no
    // timestamp of this run can carry a host time from before it.
    if (anchorNs < instance->RunStartNs) {
        return;
    }

    KeQueryPerformanceCounter(&qpcFreq);
    readFrame = anchorFrames;
    if (NowQpc > anchorQpc && qpcFreq.QuadPart > 0) {
        readFrame += T2AudioTicksToFrames(NowQpc - anchorQpc, instance->SampleRate,
                                          (ULONG64)qpcFreq.QuadPart);
    }

    if (!T2AudioClockCorrection(NextFrame, instance->DestOffset, readFrame,
                                HwFrames, &newOffset, &jump)) {
        return;
    }
    if (jump > 0) {
        T2AudioStreamZeroRingLocked(HwBufferBase, HwFrames,
                                    NextFrame + instance->DestOffset, (ULONG64)jump);
    }
    instance->ClockCorrections++;
    if (instance->ClockCorrections <= 8 || (instance->ClockCorrections % 100) == 0) {
        KdPrint(("T2Audio: T2 clock: re-aimed lead by %I64d frames (#%u, read=%I64u next=%I64u)\n",
                 jump, instance->ClockCorrections, readFrame, NextFrame));
    }
    instance->DestOffset = newOffset;
}

// Advance the consumed-frame cursor to "now" and mirror the newly consumed
// source frames into the BAR1 hardware ring. Must be called with StreamLock
// held (so at DISPATCH_LEVEL); only advances while the stream is in RUN.
//
// The source cursor runs at the sample rate from the QPC anchor taken on entry
// to RUN. Every source frame up to that cursor is copied before the cursor is
// published as the play position, so the client cannot overwrite a frame
// before it has been mirrored. Source frame N lands at ring frame
// N + DestOffset, about T2AUDIO_LEAD_FRAMES ahead of where the T2 reads; each
// new UPDATE_TIMESTAMP re-checks that lead against the T2 clock.
static VOID
T2AudioStreamAdvanceLocked(_Inout_ PT2AUDIO_WAVERT_STREAM instance)
{
    PT2AUDIO_DEVICE_CONTEXT context = instance->DeviceContext;
    LARGE_INTEGER qpc, qpcFreq;
    ULONG64 playFrame;
    ULONG64 frame;
    ULONG64 pendingFrames;
    ULONG64 skippedFrames = 0;
    ULONG64 skipStart;
    ULONG sourceFrames;
    BOOLEAN canCopy;

    if (instance->State != KSSTATE_RUN || instance->SystemBufferVa == NULL ||
        instance->SourceSize == 0 || instance->BytesPerFrame == 0 ||
        instance->SampleRate == 0) {
        return;
    }

    qpc = KeQueryPerformanceCounter(&qpcFreq);
    playFrame = instance->AnchorFrames;
    if ((ULONG64)qpc.QuadPart > instance->AnchorQpc && qpcFreq.QuadPart > 0) {
        playFrame += T2AudioTicksToFrames((ULONG64)qpc.QuadPart - instance->AnchorQpc,
                                          instance->SampleRate,
                                          (ULONG64)qpcFreq.QuadPart);
    }

    frame = instance->LastCopiedFrame;
    if (playFrame <= frame) {
        return;
    }

    // Frames more than one client buffer behind have already been overwritten
    // by the client; skip them instead of copying stale data.
    sourceFrames = instance->SourceSize / instance->BytesPerFrame;
    pendingFrames = playFrame - frame;
    skipStart = frame;
    if (pendingFrames > sourceFrames) {
        skippedFrames = pendingFrames - sourceFrames;
        frame = playFrame - sourceFrames;
        pendingFrames = sourceFrames;
    }

    if (context == NULL) {
        instance->LastCopiedFrame = playFrame;
        return;
    }

    // Hold the device's hardware lock across the check and the writes so
    // T2AudioUnmapResources cannot unmap BAR1 underneath the copy.
    KeAcquireSpinLockAtDpcLevel(&context->HardwareLock);
    canCopy = context->HardwareReady &&
              context->MmioCopyEnabled && context->SpeakerFormatValid &&
              context->Bar1Mapped != NULL &&
              context->SpeakerBufferSize != 0 &&
              context->SpeakerBufferSize <= MAXULONG &&
              context->SpeakerBufferOffset <= context->Bar1Size &&
              context->SpeakerBufferSize <= context->Bar1Size - context->SpeakerBufferOffset;

    if (canCopy) {
        PUCHAR hwBufferBase = (PUCHAR)context->Bar1Mapped + context->SpeakerBufferOffset;
        PUCHAR sourceBase = (PUCHAR)instance->SystemBufferVa;
        ULONG remainingFrames = (ULONG)pendingFrames;
        ULONG hwFrames = (ULONG)context->SpeakerBufferSize / T2AUDIO_RING_BYTES_PER_FRAME;

        // Silence the ring slots of skipped frames. Left alone they still hold
        // audio from the previous lap (~347 ms earlier), which the T2 would
        // play again as an echo/stutter.
        if (skippedFrames != 0) {
            T2AudioStreamZeroRingLocked(hwBufferBase, hwFrames,
                                        skipStart + instance->DestOffset,
                                        skippedFrames);
        }

        // A new T2 clock anchor arrived: re-check the lead of the next write
        // against the T2's read position and re-aim it if it drifted.
        T2AudioStreamFollowT2ClockLocked(instance, context, hwBufferBase,
                                         hwFrames, frame, (ULONG64)qpc.QuadPart);

        while (remainingFrames > 0) {
            ULONG srcOffset = 0;
            ULONG dstOffset = 0;
            ULONG byteLen = 0;
            ULONG chunkFrames = T2AudioCalculateCopyChunk(
                frame,
                frame + instance->DestOffset,
                remainingFrames,
                instance->BytesPerFrame,
                T2AUDIO_RING_BYTES_PER_FRAME,
                instance->SourceSize,
                (ULONG)context->SpeakerBufferSize,
                &srcOffset,
                &dstOffset,
                &byteLen);

            if (chunkFrames == 0) {
                break;
            }

            // The ring layout comes from the stream's 'pft ' descriptor: the
            // T2 speaker is S24_LE (24 bits in the low bytes), while Windows
            // delivers MSB-aligned 32-bit samples, so each sample is shifted
            // (kaiT2en maps the same descriptor to SNDRV_PCM_FORMAT_S24_LE).
            // Stereo is laid out over the woofer pairs and the high-passed
            // tweeters; the raw 6-channel format is copied as is. Frames are
            // 4-byte aligned in both rings.
            if (instance->Channels == 2) {
                T2AudioRenderStereoToSpeakers(
                    (volatile unsigned int *)(hwBufferBase + dstOffset),
                    (const unsigned int *)(sourceBase + srcOffset),
                    chunkFrames,
                    context->SpeakerSampleShift,
                    context->TweeterMode,
                    &instance->SpeakerState);
            } else {
                T2AudioConvertSamples(
                    (volatile unsigned int *)(hwBufferBase + dstOffset),
                    (const unsigned int *)(sourceBase + srcOffset),
                    byteLen / sizeof(unsigned int),
                    context->SpeakerSampleShift);
            }

            frame += chunkFrames;
            remainingFrames -= chunkFrames;
        }

        // BAR1 may be mapped write-combining; drain the WC buffers so the T2
        // sees the samples before the position that covers them is reported.
        _mm_sfence();
    }
    KeReleaseSpinLockFromDpcLevel(&context->HardwareLock);

    instance->LastCopiedFrame = playFrame;
}

// Byte play cursor reported to the audio engine: the consumed-frame cursor,
// advanced to "now" first while running. Returns 0 when the stream is stopped.
static ULONG
T2AudioStreamPlayPositionBytes(_Inout_ PT2AUDIO_WAVERT_STREAM instance)
{
    ULONG64 frame;
    ULONG bufferSize;
    KIRQL oldIrql;

    KeAcquireSpinLock(&instance->StreamLock, &oldIrql);
    bufferSize = instance->AudioBufferSize;
    if (instance->State == KSSTATE_STOP || bufferSize == 0 ||
        instance->BytesPerFrame == 0) {
        KeReleaseSpinLock(&instance->StreamLock, oldIrql);
        return 0;
    }
    T2AudioStreamAdvanceLocked(instance);
    frame = instance->LastCopiedFrame;
    KeReleaseSpinLock(&instance->StreamLock, oldIrql);

    return (ULONG)((frame * instance->BytesPerFrame) % bufferSize);
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
    PMDL mdl = instance->AudioBufferMdl;
    PVOID systemVa;
    KIRQL oldIrql;
    ULONG action = T2AudioDecideBufferRelease(
        mdl != NULL,
        instance->HardwareBuffer,
        (expected == NULL) || (expected == mdl));

    // A foreign, unknown, or already-released MDL is not ours to release. Leave
    // the stream completely untouched - including its copy timer - so a bogus
    // FreeAudioBuffer cannot stop a live stream.
    if (action == T2AUDIO_RELEASE_NONE) {
        return;
    }

    // Stop mirroring before the source or destination can go away.
    T2AudioStreamStopCopyTimer(instance);

    // Detach the buffer under the lock so a concurrent GetPosition can no
    // longer copy from it.
    KeAcquireSpinLock(&instance->StreamLock, &oldIrql);
    systemVa = instance->SystemBufferVa;
    instance->AudioBufferMdl = NULL;
    instance->AudioBufferSize = 0;
    instance->SystemBufferVa = NULL;
    instance->SourceSize = 0;
    instance->CopySize = 0;
    KeReleaseSpinLock(&instance->StreamLock, oldIrql);

    if (action == T2AUDIO_RELEASE_HARDWARE) {
        T2AudioFreeSpeakerMdl(instance->DeviceContext);
    } else { // T2AUDIO_RELEASE_SYSTEM
        if (instance->PortStream != NULL) {
            // The kernel mapping must be torn down before the pages are freed,
            // or a system PTE keeps pointing at recycled physical memory.
            if (systemVa != NULL) {
                instance->PortStream->lpVtbl->UnmapAllocatedPages(
                    (PVOID)instance->PortStream, systemVa, mdl);
            }
            instance->PortStream->lpVtbl->FreePagesFromMdl(
                (PVOID)instance->PortStream, mdl);
        }
    }

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
        if (stream->PortStream != NULL) {
            stream->PortStream->lpVtbl->Release((PVOID)stream->PortStream);
        }
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

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    if (instance == NULL) {
        return;
    }
    instance->CopyTickCalls++;

    // Backstop for the mirror: GetPosition also advances it, but the engine
    // may poll rarely. The memory-mapped register is kept for diagnostics.
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
        // SetState and FreeAudioBuffer are PASSIVE_LEVEL PortCls callbacks.
        // Drain a DPC that may already be running before releasing its buffer.
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
    ULONG channels = 0;
    NTSTATUS status;

    // The client buffer was sized for the current frame size; a format with
    // another frame size would need a new buffer.
    status = T2AudioValidateRenderFormat(
        DataFormat,
        (instance->DeviceContext != NULL && instance->DeviceContext->RawSpeakerChannels) ? TRUE : FALSE,
        &channels);
    if (NT_SUCCESS(status) && instance->AudioBufferMdl != NULL &&
        channels != instance->Channels) {
        status = STATUS_INVALID_DEVICE_REQUEST;
    }

    T2AudioLogFormat("SetFormat", DataFormat);
    KdPrint(("T2Audio: SetFormat status=0x%08X\n", status));

    if (NT_SUCCESS(status)) {
        KIRQL oldIrql;

        KeAcquireSpinLock(&instance->StreamLock, &oldIrql);
        instance->SampleRate = 48000;
        instance->Channels = channels;
        instance->BytesPerFrame = channels * 4u;
        KeReleaseSpinLock(&instance->StreamLock, oldIrql);
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

            LARGE_INTEGER qpcFreq;
            KIRQL oldIrql;
            LONGLONG currentQpc;

            // A stream may only run once a buffer has been allocated.
            if (instance->AudioBufferMdl == NULL) {
                return STATUS_INVALID_DEVICE_STATE;
            }

            // A fresh run (from STOP, cursor at 0) starts from a silent ring,
            // as kaiT2en clears the bridge ring in prepare before START_IO.
            if (instance->LastCopiedFrame == 0 && instance->DeviceContext != NULL) {
                PT2AUDIO_DEVICE_CONTEXT context = instance->DeviceContext;

                KeAcquireSpinLock(&context->HardwareLock, &oldIrql);
                if (context->HardwareReady &&
                    context->MmioCopyEnabled &&
                    context->Bar1Mapped != NULL &&
                    context->SpeakerBufferSize != 0 &&
                    context->SpeakerBufferOffset <= context->Bar1Size &&
                    context->SpeakerBufferSize <= context->Bar1Size - context->SpeakerBufferOffset) {
                    RtlZeroMemory((PUCHAR)context->Bar1Mapped +
                                  context->SpeakerBufferOffset,
                                  context->SpeakerBufferSize);
                    _mm_sfence();
                }
                KeReleaseSpinLock(&context->HardwareLock, oldIrql);
            }

            // Anchor the clock BEFORE START_IO: the T2 cannot start reading
            // earlier, so the cursor never reports less than it consumed
            // (kaiT2en anchors before START_IO for the same reason).
            currentQpc = KeQueryPerformanceCounter(&qpcFreq).QuadPart;

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

            // Publish the anchor taken above.
            {
                PT2AUDIO_DEVICE_CONTEXT context = instance->DeviceContext;
                ULONG generation = 0;
                ULONG hwFrames = 0;

                if (context != NULL) {
                    KeAcquireSpinLock(&context->ClockLock, &oldIrql);
                    generation = context->ClockGeneration;
                    KeReleaseSpinLock(&context->ClockLock, oldIrql);
                    hwFrames = (ULONG)(context->SpeakerBufferSize / T2AUDIO_RING_BYTES_PER_FRAME);
                }

                KeAcquireSpinLock(&instance->StreamLock, &oldIrql);
                instance->AnchorQpc = currentQpc;
                // Resume from the consumed cursor so no frame is skipped or
                // copied twice across PAUSE -> RUN.
                instance->AnchorFrames = instance->LastCopiedFrame;
                // START_IO restarts the T2 at ring slot 0: aim the next write
                // T2AUDIO_LEAD_FRAMES ahead of it. Anchors older than this
                // START_IO are ignored; the first new one re-checks the lead.
                instance->DestOffset = (hwFrames != 0)
                    ? (T2AUDIO_LEAD_FRAMES + hwFrames -
                       (ULONG)(instance->LastCopiedFrame % hwFrames)) % hwFrames
                    : T2AUDIO_LEAD_FRAMES;
                instance->ClockGenerationSeen = generation;
                instance->RunStartNs = T2AudioQpcToNs((ULONG64)currentQpc,
                                                      (ULONG64)qpcFreq.QuadPart);
                if (instance->LastCopiedFrame == 0) {
                    T2AudioSpeakerStateReset(&instance->SpeakerState);
                }
                instance->State = KSSTATE_RUN;
                KeReleaseSpinLock(&instance->StreamLock, oldIrql);
            }

            // Start the DPC only after its time anchor is initialized.
            if (T2AudioShouldStartCopyTimer(
                    (instance->SystemBufferVa != NULL) ? 1u : 0u,
                    (instance->CopySize != 0) ? 1u : 0u,
                    instance->TimerActive ? 1u : 0u)) {
                T2AudioStreamStartCopyTimer(instance);
            }
        }
    } else {
        // PAUSE / ACQUIRE / STOP: flush the frames played so far before
        // leaving RUN, so the consumed cursor is exact at the transition.
        KIRQL oldIrql;

        T2AudioStreamStopCopyTimer(instance);
        KeAcquireSpinLock(&instance->StreamLock, &oldIrql);
        T2AudioStreamAdvanceLocked(instance);
        instance->AnchorFrames = instance->LastCopiedFrame;
        if (State == KSSTATE_STOP) {
            instance->AnchorFrames = 0;
            instance->PositionRegister = 0;
            instance->LastCopiedFrame = 0;
        }
        instance->State = State;
        KeReleaseSpinLock(&instance->StreamLock, oldIrql);

        KdPrint(("T2Audio: SetState %u posCalls=%u regCalls=%u ticks=%u posReg=%u\n",
                 State, instance->GetPositionCalls,
                 instance->GetPositionRegCalls, instance->CopyTickCalls,
                 instance->PositionRegister));
        if (State == KSSTATE_STOP && instance->DeviceContext != NULL) {
            PT2AUDIO_DEVICE_CONTEXT context = instance->DeviceContext;

            // Silence the hardware ring under the hardware lock, so a
            // concurrent teardown cannot unmap BAR1 mid-write.
            KeAcquireSpinLock(&context->HardwareLock, &oldIrql);
            if (context->HardwareReady &&
                context->MmioCopyEnabled &&
                context->Bar1Mapped != NULL &&
                context->SpeakerBufferSize != 0 &&
                context->SpeakerBufferOffset <= context->Bar1Size &&
                context->SpeakerBufferSize <= context->Bar1Size - context->SpeakerBufferOffset) {
                RtlZeroMemory((PUCHAR)context->Bar1Mapped +
                              context->SpeakerBufferOffset,
                              context->SpeakerBufferSize);
            }
            KeReleaseSpinLock(&context->HardwareLock, oldIrql);
        }
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
    if (instance->State == KSSTATE_STOP || bufferSize == 0) {
        Position->PlayOffset = 0;
        Position->WriteOffset = 0;
        return STATUS_SUCCESS;
    }

    instance->GetPositionCalls++;
    // Advances the mirror first, so the reported position never runs ahead of
    // what has been copied out of the client buffer.
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
        // Map through the port so FreeAudioBuffer can pair it with
        // UnmapAllocatedPages before the pages are released.
        systemVa = instance->PortStream->lpVtbl->MapAllocatedPages(
            (PVOID)instance->PortStream, mdl, MmCached);
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
    PT2AUDIO_WAVERT_STREAM instance = CONTAINING_RECORD(
        Stream, T2AUDIO_WAVERT_STREAM, Interface);

    if (Latency != NULL) {
        ULONG latencyFrames = 0;

        RtlZeroMemory(Latency, sizeof(*Latency));
        if (instance->DeviceContext != NULL) {
            latencyFrames = instance->DeviceContext->SpeakerLatencyFrames;
        }
        // The copy writes each frame about T2AUDIO_LEAD_FRAMES ahead of the
        // T2 read position in the hardware ring, which acts as the FIFO. The codec delay is
        // the stream + device output latency the T2 reports ('ltnc'), in
        // 100 ns units.
        Latency->FifoSize = T2AUDIO_LEAD_FRAMES * instance->BytesPerFrame;
        Latency->ChipsetDelay = 0;
        Latency->CodecDelay = (instance->SampleRate != 0)
            ? (ULONG)(((ULONG64)latencyFrames * 10000000ull) / instance->SampleRate)
            : 0;
    }
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioStreamGetPositionRegister(
    _In_ PMINIPORTWAVERTSTREAM Stream,
    _Out_ KSRTAUDIO_HWREGISTER *Register)
{
    UNREFERENCED_PARAMETER(Stream);

    if (Register == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(Register, sizeof(*Register));

    // A hardware position register must be backed by an MMIO or isolated page.
    // Exposing an address from kernel non-paged pool memory into user space causes
    // memory disclosure and mapping faults. We return STATUS_NOT_SUPPORTED so
    // PortCls and the audio engine poll GetPosition instead.
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
    ULONG channels = 0;
    NTSTATUS status;

    if (Stream == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Stream = NULL;
    status = T2AudioValidateRenderFormat(
        DataFormat,
        (DeviceContext != NULL && DeviceContext->RawSpeakerChannels) ? TRUE : FALSE,
        &channels);
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
    instance->Channels = channels;
    instance->BytesPerFrame = channels * 4u;
    instance->DestOffset = T2AUDIO_LEAD_FRAMES;
    T2AudioSpeakerStateReset(&instance->SpeakerState);
    instance->State = KSSTATE_STOP;
    // The stream uses PortStream until it is destroyed (FreeAudioBuffer and the
    // final Release), so hold a reference for its lifetime.
    if (PortStream != NULL) {
        PortStream->lpVtbl->AddRef((PVOID)PortStream);
    }
    KeInitializeSpinLock(&instance->StreamLock);
    KeInitializeTimer(&instance->CopyTimer);
    KeInitializeDpc(&instance->CopyDpc, T2AudioStreamCopyTick, instance);
    *Stream = &instance->Interface;
    return STATUS_SUCCESS;
}
