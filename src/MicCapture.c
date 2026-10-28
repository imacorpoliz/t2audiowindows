#include "T2AudioMiniport.h"
#include "BceProtocolLogic.h"
#include "T2AudioBufferLogic.h"
#include "T2AudioSpeakerLogic.h"
#include "T2AudioMicLogic.h"
#include <intrin.h>

// Built-in microphones (Parameters\MicInput=1; ROADMAP section 5).
//
//  1. Registration (T2AudioMicRegister, at probe time, stage 4+ with BCE I/O):
//     a contiguous host ring of T2AUDIO_MIC_RING_FRAMES frames (3 x 32-bit)
//     is allocated, its physical address written into the BufferStruct entry
//     of "Digital Mic" (input_streams[0], one buffer) and announced with
//     SET_INPUT_STREAM_ADDRESS_RANGES (msg 18) - kaiT2en's
//     t2audio_init_bs_stream_host + t2audio_cmd_set_input_stream_address_
//     ranges, and AppleAudio's 0x46F8 + 0x44E4. After a wake AppleAudio
//     announces the ranges again (T2AudioMicReRegister).
//  2. Capture (a WaveRT filter "WaveMic" + topology "TopologyMic"): START_IO
//     on the microphone device; a 2 ms DPC moves the frames the T2 has
//     written (position from its UPDATE_TIMESTAMP, as kaiT2en's capture
//     pointer) through the 16_1 mic DSP into the client buffer, mono 48 kHz
//     32-bit. The reader trails the writer by T2AUDIO_MIC_CAPTURE_LAG frames.
//  3. Teardown (T2AudioMicUnregister, PnP stop): STOP_IO if running, clear
//     the BufferStruct entry, then free the ring - never the other way round.

#define T2AUDIO_MIC_COPY_PERIOD_MS 2
#define T2AUDIO_MIC_POOL_TAG       '2TMC'

// ---------------------------------------------------------------------------
// BufferStruct and registration
// ---------------------------------------------------------------------------

// Index of the BufferStruct device "Digital Mic", or -1. Caller holds
// HardwareLock with HardwareReady set (BufferStruct is device memory).
static LONG
T2AudioMicFindDeviceLocked(_In_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    static const CHAR name[] = "Digital Mic";
    ULONG index;

    if (Context->BufferStruct == NULL) {
        return -1;
    }
    for (index = 0; index < Context->BufferStructDevices && index < T2AUDIO_MAX_DEVICES; index++) {
        const T2AUDIO_DEVICE_METADATA *device = &Context->BufferStruct->Devices[index];

        if (RtlCompareMemory(device->Name, name, sizeof(name)) == sizeof(name)) {
            return (LONG)index;
        }
    }
    return -1;
}

BOOLEAN
T2AudioMicPresentInBufferStruct(_In_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    KIRQL oldIrql;
    LONG index = -1;

    if (Context == NULL) {
        return FALSE;
    }
    KeAcquireSpinLock(&Context->HardwareLock, &oldIrql);
    if (Context->HardwareReady) {
        index = T2AudioMicFindDeviceLocked(Context);
    }
    KeReleaseSpinLock(&Context->HardwareLock, oldIrql);
    return (index >= 0) ? TRUE : FALSE;
}

// Write (Address != 0) or clear (Address == 0) input_streams[0] of the mic
// entry. Returns FALSE when the entry is not there or BAR1 is gone.
static BOOLEAN
T2AudioMicWriteBufferStruct(_In_ PT2AUDIO_DEVICE_CONTEXT Context, _In_ ULONG64 Address, _In_ ULONG Size)
{
    KIRQL oldIrql;
    LONG index;
    BOOLEAN done = FALSE;

    KeAcquireSpinLock(&Context->HardwareLock, &oldIrql);
    if (Context->HardwareReady) {
        index = T2AudioMicFindDeviceLocked(Context);
        if (index >= 0) {
            volatile T2AUDIO_DEVICE_METADATA *device =
                (volatile T2AUDIO_DEVICE_METADATA *)&Context->BufferStruct->Devices[index];
            volatile T2AUDIO_STREAM_METADATA *stream = &device->InputStreams[0];
            ULONG b;
            ULONG old = stream->NumBuffers;

            if (old > T2AUDIO_MAX_BUFFERS) {
                old = T2AUDIO_MAX_BUFFERS;
            }
            // Drop whatever an earlier driver registered (AppleAudio left 3
            // buffers of its own there, EXCERPT_20261028_step4.md).
            for (b = 1; b < old; b++) {
                stream->Buffers[b].Address = 0;
                stream->Buffers[b].Size = 0;
            }
            stream->Buffers[0].Address = (SIZE_T)Address;
            stream->Buffers[0].Size = (SIZE_T)Size;
            stream->NumBuffers = (Address != 0) ? 1 : 0;
            device->NumInputStreams = (Address != 0) ? 1 : 0;
            _mm_sfence();
            done = TRUE;
        }
    }
    KeReleaseSpinLock(&Context->HardwareLock, oldIrql);
    return done;
}

static NTSTATUS
T2AudioMicSendAddressRanges(_In_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    UCHAR message[64];
    UCHAR reply[256];
    SIZE_T length = 0;
    ULONG replySize = 0;
    NTSTATUS status;

    PAGED_CODE();

    status = T2AudioBuildIoCommand(T2AUDIO_MSG_SET_INPUT_STREAM_ADDRESS_RANGES,
                                   Context->BceMicDeviceId, message, sizeof(message), &length);
    if (NT_SUCCESS(status)) {
        status = T2AudioSendBceMessage(message, (ULONG)length, reply, sizeof(reply), &replySize);
    }
    if (NT_SUCCESS(status) &&
        !T2AudioBceParseCommandResponse(reply, replySize,
                                        T2AUDIO_MSG_SET_INPUT_STREAM_ADDRESS_RANGES_RESPONSE)) {
        status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    KdPrint(("T2Audio: mic SET_INPUT_STREAM_ADDRESS_RANGES dev=0x%I64X: 0x%08X (reply %u bytes)\n",
             Context->BceMicDeviceId, status, replySize));
    return status;
}

NTSTATUS
T2AudioMicRegister(_Inout_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    PHYSICAL_ADDRESS low, high, boundary;
    PVOID va;
    NTSTATUS status;

    PAGED_CODE();

    if (Context == NULL || !Context->MicInput || !Context->MicFormatValid ||
        Context->BceMicDeviceId == 0 || Context->MicRegistered) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    low.QuadPart = 0;
    high.QuadPart = (LONGLONG)-1;
    boundary.QuadPart = 0;
    va = MmAllocateContiguousMemorySpecifyCache(T2AUDIO_MIC_RING_BYTES, low, high, boundary, MmCached);
    if (va == NULL) {
        KdPrint(("T2Audio: mic ring allocation (%u bytes) failed\n", T2AUDIO_MIC_RING_BYTES));
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(va, T2AUDIO_MIC_RING_BYTES);
    Context->MicBufferVa = va;
    Context->MicBufferPa = MmGetPhysicalAddress(va);
    Context->MicBufferSize = T2AUDIO_MIC_RING_BYTES;

    if (!T2AudioMicWriteBufferStruct(Context, (ULONG64)Context->MicBufferPa.QuadPart,
                                     Context->MicBufferSize)) {
        KdPrint(("T2Audio: mic: no \"Digital Mic\" entry in BufferStruct\n"));
        status = STATUS_OBJECT_NAME_NOT_FOUND;
    } else {
        status = T2AudioMicSendAddressRanges(Context);
    }
    if (!NT_SUCCESS(status)) {
        (VOID)T2AudioMicWriteBufferStruct(Context, 0, 0);
        MmFreeContiguousMemorySpecifyCache(va, T2AUDIO_MIC_RING_BYTES, MmCached);
        Context->MicBufferVa = NULL;
        Context->MicBufferSize = 0;
        return status;
    }
    Context->MicRegistered = TRUE;
    KdPrint(("T2Audio: mic ring registered: pa=0x%I64X size=0x%X (%u frames)\n",
             (ULONG64)Context->MicBufferPa.QuadPart, Context->MicBufferSize, T2AUDIO_MIC_RING_FRAMES));
    return STATUS_SUCCESS;
}

// After a wake (D0): write the entry again and repeat msg 18, as AppleAudio
// does for every input stream (RE_appleaudio_protocol.md section 7).
NTSTATUS
T2AudioMicReRegister(_Inout_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    PAGED_CODE();

    if (Context == NULL || !Context->MicRegistered || Context->MicBufferVa == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (!T2AudioMicWriteBufferStruct(Context, (ULONG64)Context->MicBufferPa.QuadPart,
                                     Context->MicBufferSize)) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    return T2AudioMicSendAddressRanges(Context);
}

VOID
T2AudioMicUnregister(_Inout_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    PVOID va;
    KIRQL oldIrql;

    PAGED_CODE();

    if (Context == NULL || Context->MicBufferVa == NULL) {
        return;
    }
    // The T2 must have stopped writing before the ring goes away.
    if (InterlockedExchange(&Context->MicIoActive, 0) != 0) {
        NTSTATUS status = T2AudioStopIoDevice(Context, Context->MicDeviceId);

        KdPrint(("T2Audio: mic teardown STOP_IO: 0x%08X\n", status));
    }
    (VOID)T2AudioMicWriteBufferStruct(Context, 0, 0);

    // Detach under the hardware lock so the capture DPC stops reading it.
    KeAcquireSpinLock(&Context->HardwareLock, &oldIrql);
    va = Context->MicBufferVa;
    Context->MicBufferVa = NULL;
    Context->MicRegistered = FALSE;
    KeReleaseSpinLock(&Context->HardwareLock, oldIrql);
    KeFlushQueuedDpcs();

    MmFreeContiguousMemorySpecifyCache(va, T2AUDIO_MIC_RING_BYTES, MmCached);
    Context->MicBufferSize = 0;
    KdPrint(("T2Audio: mic ring released\n"));
}

// ---------------------------------------------------------------------------
// Capture stream
// ---------------------------------------------------------------------------

typedef struct _T2AUDIO_MIC_STREAM {
    IMiniportWaveRTStream Interface;
    LONG ReferenceCount;
    PT2AUDIO_DEVICE_CONTEXT DeviceContext;
    PPORTWAVERTSTREAM PortStream;
    KSSTATE State;
    KSPIN_LOCK StreamLock;
    PMDL AudioBufferMdl;
    ULONG AudioBufferSize;
    PVOID SystemBufferVa;
    // Frames delivered to the client buffer (the reported position) and the
    // QPC anchor that drives it, as on the render side.
    ULONG64 LastFrame;
    ULONG64 AnchorQpc;
    ULONG64 AnchorFrames;
    // Host frame H reads T2 frame H + T2Offset; valid after the first anchor
    // of this run (newer than RunStartNs).
    LONGLONG T2Offset;
    BOOLEAN OffsetValid;
    ULONG64 RunStartNs;
    BOOLEAN HardwareIoStarted;
    T2AUDIO_MIC_STATE Dsp;
    // Run diagnostics.
    ULONG ReAims;
    ULONG64 SilentFrames;
    ULONG64 CapturedFrames;
    LONG PeakDb;
    ULONG MaxCopyUs;
    KTIMER CopyTimer;
    KDPC CopyDpc;
    BOOLEAN TimerActive;
} T2AUDIO_MIC_STREAM, *PT2AUDIO_MIC_STREAM;

// Move every frame up to "now" into the client buffer. StreamLock held.
static VOID
T2AudioMicAdvanceLocked(_Inout_ PT2AUDIO_MIC_STREAM s)
{
    PT2AUDIO_DEVICE_CONTEXT context = s->DeviceContext;
    LARGE_INTEGER qpc, qpcFreq;
    ULONG64 target;
    ULONG64 frame;
    ULONG clientFrames;
    BOOLEAN haveWriter = FALSE;
    ULONG64 writeFrame = 0;
    PUCHAR ring = NULL;
    PULONG client;

    if (s->State != KSSTATE_RUN || s->SystemBufferVa == NULL || s->AudioBufferSize < 4 ||
        context == NULL) {
        return;
    }
    qpc = KeQueryPerformanceCounter(&qpcFreq);
    target = s->AnchorFrames;
    if ((ULONG64)qpc.QuadPart > s->AnchorQpc && qpcFreq.QuadPart > 0) {
        target += T2AudioTicksToFrames((ULONG64)qpc.QuadPart - s->AnchorQpc, 48000,
                                       (ULONG64)qpcFreq.QuadPart);
    }
    frame = s->LastFrame;
    if (target <= frame) {
        return;
    }
    clientFrames = s->AudioBufferSize / 4u;
    if (target - frame > clientFrames) {
        frame = target - clientFrames;   // the client fell behind: drop the oldest
    }

    // The T2 write position: the latest anchor of this run plus the time
    // since, minus the device's input latency (kaiT2en's capture pointer).
    KeAcquireSpinLockAtDpcLevel(&context->ClockLock);
    if (context->MicClockAnchorNs >= s->RunStartNs && context->MicClockGeneration != 0 &&
        context->MicClockAnchorQpc != 0) {
        ULONG64 elapsed = 0;

        if ((ULONG64)qpc.QuadPart > context->MicClockAnchorQpc && qpcFreq.QuadPart > 0) {
            elapsed = T2AudioTicksToFrames((ULONG64)qpc.QuadPart - context->MicClockAnchorQpc,
                                           48000, (ULONG64)qpcFreq.QuadPart);
        }
        writeFrame = context->MicClockAnchorFrames + elapsed;
        writeFrame = (writeFrame > context->MicLatencyFrames) ? writeFrame - context->MicLatencyFrames : 0;
        haveWriter = TRUE;
    }
    KeReleaseSpinLockFromDpcLevel(&context->ClockLock);

    if (haveWriter) {
        if (T2AudioMicCaptureAim(target, &s->T2Offset, writeFrame, s->OffsetValid ? 1 : 0)) {
            if (s->OffsetValid) {
                s->ReAims++;
                if (s->ReAims <= 8 || (s->ReAims % 100) == 0) {
                    KdPrint(("T2Audio: mic clock: re-aimed (#%u, write=%I64u host=%I64u)\n",
                             s->ReAims, writeFrame, target));
                }
            }
            s->OffsetValid = TRUE;
        }
    }

    client = (PULONG)s->SystemBufferVa;
    KeAcquireSpinLockAtDpcLevel(&context->HardwareLock);
    if (context->MicRegistered && context->MicBufferVa != NULL && !context->PoweredDown) {
        ring = (PUCHAR)context->MicBufferVa;
    }
    for (; frame < target; frame++) {
        LONGLONG t2Frame = (LONGLONG)frame + s->T2Offset;
        ULONG out = 0;

        if (ring != NULL && s->OffsetValid && t2Frame >= 0 && (ULONG64)t2Frame < writeFrame) {
            const volatile ULONG *slot = (const volatile ULONG *)(ring +
                (SIZE_T)T2AudioMicRingSlot(t2Frame, T2AUDIO_MIC_RING_FRAMES) * T2AUDIO_MIC_BYTES_PER_FRAME);
            unsigned int raw[3];

            raw[0] = slot[0];
            raw[1] = slot[1];
            raw[2] = slot[2];
            out = (ULONG)T2AudioMicProcessFrame(&s->Dsp, raw, context->MicDsp);
            s->CapturedFrames++;
        } else {
            s->SilentFrames++;
        }
        client[(ULONG)(frame % clientFrames)] = out;
    }
    KeReleaseSpinLockFromDpcLevel(&context->HardwareLock);
    s->LastFrame = target;

    {
        LARGE_INTEGER done = KeQueryPerformanceCounter(NULL);

        if (qpcFreq.QuadPart > 0 && done.QuadPart > qpc.QuadPart) {
            ULONG64 us = (((ULONG64)done.QuadPart - (ULONG64)qpc.QuadPart) * 1000000ull) /
                         (ULONG64)qpcFreq.QuadPart;
            if (us > s->MaxCopyUs) {
                s->MaxCopyUs = (us > MAXULONG) ? MAXULONG : (ULONG)us;
            }
        }
    }
}

static ULONG
T2AudioMicPositionBytes(_Inout_ PT2AUDIO_MIC_STREAM s)
{
    KIRQL oldIrql;
    ULONG64 frame;
    ULONG size;

    KeAcquireSpinLock(&s->StreamLock, &oldIrql);
    size = s->AudioBufferSize;
    if (s->State == KSSTATE_STOP || size < 4) {
        KeReleaseSpinLock(&s->StreamLock, oldIrql);
        return 0;
    }
    T2AudioMicAdvanceLocked(s);
    frame = s->LastFrame;
    KeReleaseSpinLock(&s->StreamLock, oldIrql);
    return (ULONG)((frame * 4u) % size);
}

static KDEFERRED_ROUTINE T2AudioMicCopyTick;

static VOID
T2AudioMicCopyTick(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    if (DeferredContext != NULL) {
        (VOID)T2AudioMicPositionBytes((PT2AUDIO_MIC_STREAM)DeferredContext);
    }
}

static VOID
T2AudioMicStopTimer(_Inout_ PT2AUDIO_MIC_STREAM s)
{
    if (s->TimerActive) {
        KeCancelTimer(&s->CopyTimer);
        KeFlushQueuedDpcs();
        s->TimerActive = FALSE;
    }
}

static VOID
T2AudioMicReleaseBuffer(_Inout_ PT2AUDIO_MIC_STREAM s)
{
    KIRQL oldIrql;
    PMDL mdl;
    PVOID va;

    T2AudioMicStopTimer(s);
    KeAcquireSpinLock(&s->StreamLock, &oldIrql);
    mdl = s->AudioBufferMdl;
    va = s->SystemBufferVa;
    s->AudioBufferMdl = NULL;
    s->SystemBufferVa = NULL;
    s->AudioBufferSize = 0;
    KeReleaseSpinLock(&s->StreamLock, oldIrql);
    if (mdl != NULL && s->PortStream != NULL) {
        if (va != NULL) {
            s->PortStream->lpVtbl->UnmapAllocatedPages((PVOID)s->PortStream, va, mdl);
        }
        s->PortStream->lpVtbl->FreePagesFromMdl((PVOID)s->PortStream, mdl);
    }
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioMicStreamQueryInterface(
    _In_ PMINIPORTWAVERTSTREAM Unknown,
    _In_ REFIID InterfaceId,
    _COM_Outptr_ PVOID *Interface)
{
    PT2AUDIO_MIC_STREAM s = CONTAINING_RECORD(Unknown, T2AUDIO_MIC_STREAM, Interface);

    if (Interface == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Interface = NULL;
    if (IsEqualGUIDAligned(InterfaceId, &IID_IUnknown) ||
        IsEqualGUIDAligned(InterfaceId, &IID_IMiniportWaveRTStream)) {
        *Interface = &s->Interface;
        InterlockedIncrement(&s->ReferenceCount);
        return STATUS_SUCCESS;
    }
    return STATUS_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE
T2AudioMicStreamAddRef(_In_ PMINIPORTWAVERTSTREAM Unknown)
{
    PT2AUDIO_MIC_STREAM s = CONTAINING_RECORD(Unknown, T2AUDIO_MIC_STREAM, Interface);
    return (ULONG)InterlockedIncrement(&s->ReferenceCount);
}

static ULONG STDMETHODCALLTYPE
T2AudioMicStreamRelease(_In_ PMINIPORTWAVERTSTREAM Unknown)
{
    PT2AUDIO_MIC_STREAM s = CONTAINING_RECORD(Unknown, T2AUDIO_MIC_STREAM, Interface);
    LONG references = InterlockedDecrement(&s->ReferenceCount);

    if (references == 0) {
        if (s->DeviceContext != NULL) {
            (VOID)InterlockedCompareExchangePointer(&s->DeviceContext->MicRunningStream, NULL, s);
        }
        T2AudioMicReleaseBuffer(s);
        if (s->PortStream != NULL) {
            s->PortStream->lpVtbl->Release((PVOID)s->PortStream);
        }
        ExFreePoolWithTag(s, T2AUDIO_MIC_POOL_TAG);
    }
    return (ULONG)references;
}

// Mono, 48 kHz, 32-bit PCM (WAVEFORMATEX or EXTENSIBLE with a mono mask).
static NTSTATUS
T2AudioMicValidateFormat(_In_opt_ PKSDATAFORMAT DataFormat)
{
    PKSDATAFORMAT_WAVEFORMATEX wf;

    if (DataFormat == NULL || DataFormat->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX) ||
        !IsEqualGUIDAligned(&DataFormat->MajorFormat, &KSDATAFORMAT_TYPE_AUDIO) ||
        !IsEqualGUIDAligned(&DataFormat->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM) ||
        !IsEqualGUIDAligned(&DataFormat->Specifier, &KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) {
        return STATUS_INVALID_PARAMETER;
    }
    wf = (PKSDATAFORMAT_WAVEFORMATEX)DataFormat;
    if (wf->WaveFormatEx.nChannels != 1 || wf->WaveFormatEx.nSamplesPerSec != 48000 ||
        wf->WaveFormatEx.wBitsPerSample != 32 || wf->WaveFormatEx.nBlockAlign != 4 ||
        wf->WaveFormatEx.nAvgBytesPerSec != 48000u * 4u) {
        return STATUS_INVALID_PARAMETER;
    }
    if (wf->WaveFormatEx.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        PKSDATAFORMAT_WAVEFORMATEXTENSIBLE ext = (PKSDATAFORMAT_WAVEFORMATEXTENSIBLE)DataFormat;

        if (DataFormat->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE) ||
            !IsEqualGUIDAligned(&ext->WaveFormatExt.SubFormat, &KSDATAFORMAT_SUBTYPE_PCM) ||
            (ext->WaveFormatExt.dwChannelMask != KSAUDIO_SPEAKER_MONO &&
             ext->WaveFormatExt.dwChannelMask != 0)) {
            return STATUS_INVALID_PARAMETER;
        }
    } else if (wf->WaveFormatEx.wFormatTag != WAVE_FORMAT_PCM) {
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioMicStreamSetFormat(_In_ PMINIPORTWAVERTSTREAM Stream, _In_ PKSDATAFORMAT DataFormat)
{
    UNREFERENCED_PARAMETER(Stream);
    return T2AudioMicValidateFormat(DataFormat);
}

// StreamMutex held (T2AudioMicStreamSetState or the resume work item).
static NTSTATUS
T2AudioMicSetStateLocked(_Inout_ PT2AUDIO_MIC_STREAM s, _In_ KSSTATE State)
{
    PT2AUDIO_DEVICE_CONTEXT context = s->DeviceContext;
    KIRQL oldIrql;

    if (State == KSSTATE_RUN) {
        LARGE_INTEGER qpc, qpcFreq;
        NTSTATUS status;

        if (s->State == KSSTATE_RUN) {
            return STATUS_SUCCESS;
        }
        if (s->AudioBufferMdl == NULL) {
            return STATUS_INVALID_DEVICE_STATE;
        }
        qpc = KeQueryPerformanceCounter(&qpcFreq);
        if (context->BceIoEnabled && context->MicRegistered && context->MicDeviceId != 0 &&
            !s->HardwareIoStarted) {
            status = T2AudioStartIoDevice(context, context->MicDeviceId);
            KdPrint(("T2Audio: mic RUN START_IO: 0x%08X\n", status));
            if (!NT_SUCCESS(status)) {
                return status;
            }
            s->HardwareIoStarted = TRUE;
            InterlockedExchange(&context->MicIoActive, 1);
        }
        KeAcquireSpinLock(&s->StreamLock, &oldIrql);
        s->AnchorQpc = (ULONG64)qpc.QuadPart;
        s->AnchorFrames = s->LastFrame;
        s->RunStartNs = T2AudioQpcToNs((ULONG64)qpc.QuadPart, (ULONG64)qpcFreq.QuadPart);
        s->OffsetValid = FALSE;
        s->ReAims = 0;
        s->SilentFrames = 0;
        s->CapturedFrames = 0;
        s->MaxCopyUs = 0;
        if (s->LastFrame == 0) {
            T2AudioMicReset(&s->Dsp);
        }
        s->State = KSSTATE_RUN;
        KeReleaseSpinLock(&s->StreamLock, oldIrql);
        if (!s->TimerActive) {
            LARGE_INTEGER due;

            due.QuadPart = -10000;
            KeSetTimerEx(&s->CopyTimer, due, T2AUDIO_MIC_COPY_PERIOD_MS, &s->CopyDpc);
            s->TimerActive = TRUE;
        }
        return STATUS_SUCCESS;
    }

    T2AudioMicStopTimer(s);
    KeAcquireSpinLock(&s->StreamLock, &oldIrql);
    T2AudioMicAdvanceLocked(s);
    if (State == KSSTATE_STOP) {
        s->LastFrame = 0;
        s->AnchorFrames = 0;
    }
    s->State = State;
    KeReleaseSpinLock(&s->StreamLock, oldIrql);
    KdPrint(("T2Audio: mic run summary: captured=%I64u silent=%I64u reAims=%u maxCopyUs=%u\n",
             s->CapturedFrames, s->SilentFrames, s->ReAims, s->MaxCopyUs));
    if (s->HardwareIoStarted) {
        if (InterlockedExchange(&context->MicIoActive, 0) != 0) {
            NTSTATUS status = T2AudioStopIoDevice(context, context->MicDeviceId);

            KdPrint(("T2Audio: mic leave RUN STOP_IO: 0x%08X\n", status));
        }
        s->HardwareIoStarted = FALSE;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioMicStreamSetState(_In_ PMINIPORTWAVERTSTREAM Stream, _In_ KSSTATE State)
{
    PT2AUDIO_MIC_STREAM s = CONTAINING_RECORD(Stream, T2AUDIO_MIC_STREAM, Interface);
    PT2AUDIO_DEVICE_CONTEXT context = s->DeviceContext;
    NTSTATUS status;

    if (State < KSSTATE_STOP || State > KSSTATE_RUN || context == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    KeWaitForSingleObject(&context->StreamMutex, Executive, KernelMode, FALSE, NULL);
    status = T2AudioMicSetStateLocked(s, State);
    if (s->State == KSSTATE_RUN) {
        context->MicRunningStream = s;
    } else if (context->MicRunningStream == (PVOID)s) {
        context->MicRunningStream = NULL;
    }
    KeReleaseMutex(&context->StreamMutex, FALSE);
    return status;
}

// Resume after sleep (Power.c work item, StreamMutex held): restart the mic
// I/O that the power-down stopped, like a PAUSE -> RUN.
VOID
T2AudioMicStreamResumeAfterPower(_In_ PVOID Stream)
{
    PT2AUDIO_MIC_STREAM s = (PT2AUDIO_MIC_STREAM)Stream;
    NTSTATUS status;

    PAGED_CODE();

    if (s == NULL || s->DeviceContext == NULL || s->State != KSSTATE_RUN ||
        !s->HardwareIoStarted || s->DeviceContext->MicIoActive != 0) {
        return;
    }
    s->HardwareIoStarted = FALSE;
    status = T2AudioMicSetStateLocked(s, KSSTATE_PAUSE);
    if (NT_SUCCESS(status)) {
        status = T2AudioMicSetStateLocked(s, KSSTATE_RUN);
    }
    KdPrint(("T2Audio: resume after sleep: mic status=0x%08X\n", status));
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioMicStreamGetPosition(_In_ PMINIPORTWAVERTSTREAM Stream, _Out_ PKSAUDIO_POSITION Position)
{
    PT2AUDIO_MIC_STREAM s = CONTAINING_RECORD(Stream, T2AUDIO_MIC_STREAM, Interface);
    ULONG pos;

    if (Position == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    pos = T2AudioMicPositionBytes(s);
    Position->PlayOffset = pos;
    Position->WriteOffset = pos;
    return STATUS_SUCCESS;
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioMicStreamAllocateAudioBuffer(
    _In_ PMINIPORTWAVERTSTREAM Stream,
    _In_ ULONG RequestedSize,
    _Out_ PMDL *AudioBufferMdl,
    _Out_ ULONG *ActualSize,
    _Out_ ULONG *OffsetFromFirstPage,
    _Out_ MEMORY_CACHING_TYPE *CacheType)
{
    PT2AUDIO_MIC_STREAM s = CONTAINING_RECORD(Stream, T2AUDIO_MIC_STREAM, Interface);
    PHYSICAL_ADDRESS high;
    ULONG size;
    PMDL mdl;
    PVOID va;

    if (AudioBufferMdl == NULL || ActualSize == NULL || OffsetFromFirstPage == NULL ||
        CacheType == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *AudioBufferMdl = NULL;
    *ActualSize = 0;
    *OffsetFromFirstPage = 0;
    *CacheType = MmCached;
    if (RequestedSize < 4 || s->AudioBufferMdl != NULL || s->PortStream == NULL ||
        !T2AudioAlignSizeUpToFrame(RequestedSize, 4, &size)) {
        return STATUS_INVALID_PARAMETER;
    }
    high.QuadPart = (LONGLONG)-1;
    mdl = s->PortStream->lpVtbl->AllocatePagesForMdl((PVOID)s->PortStream, high, size);
    if (mdl == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (MmGetMdlByteCount(mdl) < size) {
        s->PortStream->lpVtbl->FreePagesFromMdl((PVOID)s->PortStream, mdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    va = s->PortStream->lpVtbl->MapAllocatedPages((PVOID)s->PortStream, mdl, MmCached);
    if (va == NULL) {
        s->PortStream->lpVtbl->FreePagesFromMdl((PVOID)s->PortStream, mdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(va, size);
    s->AudioBufferMdl = mdl;
    s->SystemBufferVa = va;
    s->AudioBufferSize = size;
    *AudioBufferMdl = mdl;
    *ActualSize = size;
    KdPrint(("T2Audio: mic AllocateAudioBuffer size=%u\n", size));
    return STATUS_SUCCESS;
}

static VOID STDMETHODCALLTYPE
T2AudioMicStreamFreeAudioBuffer(_In_ PMINIPORTWAVERTSTREAM Stream, _In_opt_ PMDL AudioBufferMdl,
                                _In_ ULONG BufferSize)
{
    PT2AUDIO_MIC_STREAM s = CONTAINING_RECORD(Stream, T2AUDIO_MIC_STREAM, Interface);

    UNREFERENCED_PARAMETER(BufferSize);
    if (AudioBufferMdl != NULL && AudioBufferMdl == s->AudioBufferMdl) {
        T2AudioMicReleaseBuffer(s);
    }
}

static VOID STDMETHODCALLTYPE
T2AudioMicStreamGetHWLatency(_In_ PMINIPORTWAVERTSTREAM Stream, _Out_ KSRTAUDIO_HWLATENCY *Latency)
{
    PT2AUDIO_MIC_STREAM s = CONTAINING_RECORD(Stream, T2AUDIO_MIC_STREAM, Interface);

    if (Latency != NULL) {
        ULONG frames = (s->DeviceContext != NULL) ? s->DeviceContext->MicLatencyFrames : 0;

        RtlZeroMemory(Latency, sizeof(*Latency));
        Latency->FifoSize = T2AUDIO_MIC_CAPTURE_LAG * 4u;
        Latency->CodecDelay = (ULONG)(((ULONG64)frames * 10000000ull) / 48000u);
    }
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioMicStreamGetRegister(_In_ PMINIPORTWAVERTSTREAM Stream, _Out_ KSRTAUDIO_HWREGISTER *Register)
{
    UNREFERENCED_PARAMETER(Stream);
    if (Register != NULL) {
        RtlZeroMemory(Register, sizeof(*Register));
    }
    return STATUS_NOT_SUPPORTED;
}

static const IMiniportWaveRTStreamVtbl g_T2AudioMicStreamVtbl = {
    (void *)T2AudioMicStreamQueryInterface,
    (void *)T2AudioMicStreamAddRef,
    (void *)T2AudioMicStreamRelease,
    (void *)T2AudioMicStreamSetFormat,
    (void *)T2AudioMicStreamSetState,
    (void *)T2AudioMicStreamGetPosition,
    (void *)T2AudioMicStreamAllocateAudioBuffer,
    (void *)T2AudioMicStreamFreeAudioBuffer,
    (void *)T2AudioMicStreamGetHWLatency,
    (void *)T2AudioMicStreamGetRegister,
    (void *)T2AudioMicStreamGetRegister
};

// ---------------------------------------------------------------------------
// Capture WaveRT miniport ("WaveMic")
// ---------------------------------------------------------------------------

typedef struct _T2AUDIO_MIC_MINIPORT {
    IMiniportWaveRT Interface;
    LONG ReferenceCount;
    PT2AUDIO_DEVICE_CONTEXT DeviceContext;
} T2AUDIO_MIC_MINIPORT, *PT2AUDIO_MIC_MINIPORT;

static KSDATARANGE_AUDIO g_T2AudioMicRange = {
    {
        sizeof(KSDATARANGE_AUDIO), 0, 0, 0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
    },
    1, 32, 32, 48000, 48000
};
static PKSDATARANGE g_T2AudioMicRanges[] = { (PKSDATARANGE)&g_T2AudioMicRange };

static KSDATARANGE g_T2AudioMicBridgeRange = {
    sizeof(KSDATARANGE), 0, 0, 0,
    STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
    STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
    STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
};
static PKSDATARANGE g_T2AudioMicBridgeRanges[] = { &g_T2AudioMicBridgeRange };

static PCPIN_DESCRIPTOR g_T2AudioMicWavePins[] = {
    // Pin 0: capture streaming pin (the audio engine reads from it).
    {
        1, 1, 0, NULL,
        {
            0, NULL, 0, NULL,
            SIZEOF_ARRAY(g_T2AudioMicRanges), g_T2AudioMicRanges,
            KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO, NULL, 0
        }
    },
    // Pin 1: bridge from the microphone topology filter.
    {
        1, 1, 0, NULL,
        {
            0, NULL, 0, NULL,
            SIZEOF_ARRAY(g_T2AudioMicBridgeRanges), g_T2AudioMicBridgeRanges,
            KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO, NULL, 0
        }
    }
};

static PCCONNECTION_DESCRIPTOR g_T2AudioMicWaveConnections[] = {
    { PCFILTER_NODE, T2AUDIO_MIC_WAVE_PIN_BRIDGE, PCFILTER_NODE, T2AUDIO_MIC_WAVE_PIN_CAPTURE }
};

static PCFILTER_DESCRIPTOR g_T2AudioMicWaveDescriptor = {
    0, NULL,
    sizeof(PCPIN_DESCRIPTOR), SIZEOF_ARRAY(g_T2AudioMicWavePins), g_T2AudioMicWavePins,
    0, 0, NULL,
    SIZEOF_ARRAY(g_T2AudioMicWaveConnections), g_T2AudioMicWaveConnections,
    0, NULL
};

static NTSTATUS STDMETHODCALLTYPE
T2AudioMicMiniportQueryInterface(_In_ PMINIPORTWAVERT Unknown, _In_ REFIID InterfaceId,
                                 _COM_Outptr_ PVOID *Interface)
{
    PT2AUDIO_MIC_MINIPORT m = CONTAINING_RECORD(Unknown, T2AUDIO_MIC_MINIPORT, Interface);

    if (Interface == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Interface = NULL;
    if (IsEqualGUIDAligned(InterfaceId, &IID_IUnknown) ||
        IsEqualGUIDAligned(InterfaceId, &IID_IMiniport) ||
        IsEqualGUIDAligned(InterfaceId, &IID_IMiniportWaveRT)) {
        *Interface = &m->Interface;
        InterlockedIncrement(&m->ReferenceCount);
        return STATUS_SUCCESS;
    }
    return STATUS_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE
T2AudioMicMiniportAddRef(_In_ PMINIPORTWAVERT Unknown)
{
    PT2AUDIO_MIC_MINIPORT m = CONTAINING_RECORD(Unknown, T2AUDIO_MIC_MINIPORT, Interface);
    return (ULONG)InterlockedIncrement(&m->ReferenceCount);
}

static ULONG STDMETHODCALLTYPE
T2AudioMicMiniportRelease(_In_ PMINIPORTWAVERT Unknown)
{
    PT2AUDIO_MIC_MINIPORT m = CONTAINING_RECORD(Unknown, T2AUDIO_MIC_MINIPORT, Interface);
    LONG references = InterlockedDecrement(&m->ReferenceCount);

    if (references == 0) {
        ExFreePoolWithTag(m, T2AUDIO_MIC_POOL_TAG);
    }
    return (ULONG)references;
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioMicMiniportGetDescription(_In_ PMINIPORT Miniport, _Out_ PPCFILTER_DESCRIPTOR *Description)
{
    UNREFERENCED_PARAMETER(Miniport);
    if (Description == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Description = &g_T2AudioMicWaveDescriptor;
    return STATUS_SUCCESS;
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioMicMiniportDataRangeIntersection(
    _In_ PMINIPORT Miniport,
    _In_ ULONG PinId,
    _In_ PKSDATARANGE DataRange,
    _In_ PKSDATARANGE MatchingDataRange,
    _In_ ULONG OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength) PVOID ResultantFormat,
    _Out_ PULONG ResultantFormatLength)
{
    PKSDATAFORMAT_WAVEFORMATEXTENSIBLE result;

    UNREFERENCED_PARAMETER(Miniport);
    UNREFERENCED_PARAMETER(MatchingDataRange);
    if (ResultantFormatLength == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *ResultantFormatLength = 0;
    if (PinId == T2AUDIO_MIC_WAVE_PIN_BRIDGE) {
        return STATUS_NOT_IMPLEMENTED;
    }
    if (PinId != T2AUDIO_MIC_WAVE_PIN_CAPTURE || DataRange == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (DataRange->FormatSize != sizeof(KSDATARANGE_AUDIO) &&
        DataRange->FormatSize >= sizeof(KSDATAFORMAT_WAVEFORMATEX) &&
        IsEqualGUIDAligned(&DataRange->Specifier, &KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) &&
        !NT_SUCCESS(T2AudioMicValidateFormat(DataRange))) {
        return STATUS_NO_MATCH;
    }
    if (DataRange->FormatSize == sizeof(KSDATARANGE_AUDIO)) {
        PKSDATARANGE_AUDIO r = (PKSDATARANGE_AUDIO)DataRange;

        if (r->MaximumChannels < 1 || r->MinimumBitsPerSample > 32 || r->MaximumBitsPerSample < 32 ||
            r->MinimumSampleFrequency > 48000 || r->MaximumSampleFrequency < 48000) {
            return STATUS_NO_MATCH;
        }
    }
    if (OutputBufferLength == 0) {
        *ResultantFormatLength = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
        return STATUS_BUFFER_OVERFLOW;
    }
    if (OutputBufferLength < sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE) || ResultantFormat == NULL) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    result = (PKSDATAFORMAT_WAVEFORMATEXTENSIBLE)ResultantFormat;
    RtlZeroMemory(result, sizeof(*result));
    result->DataFormat.FormatSize = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
    result->DataFormat.SampleSize = 4;
    result->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    result->DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    result->DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    result->WaveFormatExt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    result->WaveFormatExt.Format.nChannels = 1;
    result->WaveFormatExt.Format.nSamplesPerSec = 48000;
    result->WaveFormatExt.Format.wBitsPerSample = 32;
    result->WaveFormatExt.Format.nBlockAlign = 4;
    result->WaveFormatExt.Format.nAvgBytesPerSec = 48000 * 4;
    result->WaveFormatExt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    result->WaveFormatExt.Samples.wValidBitsPerSample = 32;
    result->WaveFormatExt.dwChannelMask = KSAUDIO_SPEAKER_MONO;
    result->WaveFormatExt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    *ResultantFormatLength = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
    return STATUS_SUCCESS;
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioMicMiniportInit(_In_ PMINIPORTWAVERT Miniport, _In_ PUNKNOWN UnknownAdapter,
                       _In_ PRESOURCELIST ResourceList, _In_ PPORTWAVERT Port)
{
    UNREFERENCED_PARAMETER(Miniport);
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(Port);
    return STATUS_SUCCESS;
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioMicMiniportNewStream(
    _In_ PMINIPORTWAVERT Miniport,
    _Out_ PMINIPORTWAVERTSTREAM *Stream,
    _In_ PPORTWAVERTSTREAM PortStream,
    _In_ ULONG Pin,
    _In_ BOOLEAN Capture,
    _In_ PKSDATAFORMAT DataFormat)
{
    PT2AUDIO_MIC_MINIPORT m = CONTAINING_RECORD(Miniport, T2AUDIO_MIC_MINIPORT, Interface);
    PT2AUDIO_MIC_STREAM s;

    if (Stream == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Stream = NULL;
    if (!Capture || Pin != T2AUDIO_MIC_WAVE_PIN_CAPTURE ||
        !NT_SUCCESS(T2AudioMicValidateFormat(DataFormat))) {
        return STATUS_INVALID_PARAMETER;
    }
    s = (PT2AUDIO_MIC_STREAM)ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*s), T2AUDIO_MIC_POOL_TAG);
    if (s == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(s, sizeof(*s));
    s->Interface.lpVtbl = &g_T2AudioMicStreamVtbl;
    s->ReferenceCount = 1;
    s->DeviceContext = m->DeviceContext;
    s->PortStream = PortStream;
    s->State = KSSTATE_STOP;
    if (PortStream != NULL) {
        PortStream->lpVtbl->AddRef((PVOID)PortStream);
    }
    T2AudioMicReset(&s->Dsp);
    KeInitializeSpinLock(&s->StreamLock);
    KeInitializeTimer(&s->CopyTimer);
    KeInitializeDpc(&s->CopyDpc, T2AudioMicCopyTick, s);
    *Stream = &s->Interface;
    KdPrint(("T2Audio: mic NewStream\n"));
    return STATUS_SUCCESS;
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioMicMiniportGetDeviceDescription(_In_ PMINIPORTWAVERT Miniport, _Out_ PDEVICE_DESCRIPTION Description)
{
    UNREFERENCED_PARAMETER(Miniport);
    if (Description == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(Description, sizeof(*Description));
    Description->Version = DEVICE_DESCRIPTION_VERSION;
    Description->Master = TRUE;
    Description->Dma32BitAddresses = TRUE;
    Description->Dma64BitAddresses = TRUE;
    Description->InterfaceType = PCIBus;
    return STATUS_SUCCESS;
}

static const IMiniportWaveRTVtbl g_T2AudioMicMiniportVtbl = {
    (void *)T2AudioMicMiniportQueryInterface,
    (void *)T2AudioMicMiniportAddRef,
    (void *)T2AudioMicMiniportRelease,
    (void *)T2AudioMicMiniportGetDescription,
    (void *)T2AudioMicMiniportDataRangeIntersection,
    (void *)T2AudioMicMiniportInit,
    (void *)T2AudioMicMiniportNewStream,
    (void *)T2AudioMicMiniportGetDeviceDescription
};

// ---------------------------------------------------------------------------
// Microphone topology ("TopologyMic"): microphone array -> bridge
// ---------------------------------------------------------------------------

typedef struct _T2AUDIO_MIC_TOPOLOGY {
    IMiniportTopology Interface;
    LONG ReferenceCount;
} T2AUDIO_MIC_TOPOLOGY, *PT2AUDIO_MIC_TOPOLOGY;

static PCPIN_DESCRIPTOR g_T2AudioMicTopoPins[] = {
    // Pin 0: the built-in microphone array (physical source).
    {
        1, 1, 0, NULL,
        {
            0, NULL, 0, NULL,
            SIZEOF_ARRAY(g_T2AudioMicBridgeRanges), g_T2AudioMicBridgeRanges,
            KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_MICROPHONE_ARRAY, NULL, 0
        }
    },
    // Pin 1: bridge to the capture WaveRT filter.
    {
        1, 1, 0, NULL,
        {
            0, NULL, 0, NULL,
            SIZEOF_ARRAY(g_T2AudioMicBridgeRanges), g_T2AudioMicBridgeRanges,
            KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO, NULL, 0
        }
    }
};

static PCCONNECTION_DESCRIPTOR g_T2AudioMicTopoConnections[] = {
    { PCFILTER_NODE, T2AUDIO_MIC_TOPO_PIN_MIC, PCFILTER_NODE, T2AUDIO_MIC_TOPO_PIN_BRIDGE }
};

static PCFILTER_DESCRIPTOR g_T2AudioMicTopoDescriptor = {
    0, NULL,
    sizeof(PCPIN_DESCRIPTOR), SIZEOF_ARRAY(g_T2AudioMicTopoPins), g_T2AudioMicTopoPins,
    sizeof(PCNODE_DESCRIPTOR), 0, NULL,
    SIZEOF_ARRAY(g_T2AudioMicTopoConnections), g_T2AudioMicTopoConnections,
    0, NULL
};

static NTSTATUS STDMETHODCALLTYPE
T2AudioMicTopoQueryInterface(_In_ PMINIPORTTOPOLOGY Unknown, _In_ REFIID InterfaceId,
                             _COM_Outptr_ PVOID *Interface)
{
    PT2AUDIO_MIC_TOPOLOGY t = CONTAINING_RECORD(Unknown, T2AUDIO_MIC_TOPOLOGY, Interface);

    if (Interface == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Interface = NULL;
    if (IsEqualGUIDAligned(InterfaceId, &IID_IUnknown) ||
        IsEqualGUIDAligned(InterfaceId, &IID_IMiniport) ||
        IsEqualGUIDAligned(InterfaceId, &IID_IMiniportTopology)) {
        *Interface = &t->Interface;
        InterlockedIncrement(&t->ReferenceCount);
        return STATUS_SUCCESS;
    }
    return STATUS_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE
T2AudioMicTopoAddRef(_In_ PMINIPORTTOPOLOGY Unknown)
{
    PT2AUDIO_MIC_TOPOLOGY t = CONTAINING_RECORD(Unknown, T2AUDIO_MIC_TOPOLOGY, Interface);
    return (ULONG)InterlockedIncrement(&t->ReferenceCount);
}

static ULONG STDMETHODCALLTYPE
T2AudioMicTopoRelease(_In_ PMINIPORTTOPOLOGY Unknown)
{
    PT2AUDIO_MIC_TOPOLOGY t = CONTAINING_RECORD(Unknown, T2AUDIO_MIC_TOPOLOGY, Interface);
    LONG references = InterlockedDecrement(&t->ReferenceCount);

    if (references == 0) {
        ExFreePoolWithTag(t, T2AUDIO_MIC_POOL_TAG);
    }
    return (ULONG)references;
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioMicTopoGetDescription(_In_ PMINIPORT Miniport, _Out_ PPCFILTER_DESCRIPTOR *Description)
{
    UNREFERENCED_PARAMETER(Miniport);
    if (Description == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Description = &g_T2AudioMicTopoDescriptor;
    return STATUS_SUCCESS;
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioMicTopoDataRangeIntersection(
    _In_ PMINIPORT Miniport,
    _In_ ULONG PinId,
    _In_ PKSDATARANGE DataRange,
    _In_ PKSDATARANGE MatchingDataRange,
    _In_ ULONG OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength) PVOID ResultantFormat,
    _Out_ PULONG ResultantFormatLength)
{
    UNREFERENCED_PARAMETER(Miniport);
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(DataRange);
    UNREFERENCED_PARAMETER(MatchingDataRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);
    UNREFERENCED_PARAMETER(ResultantFormatLength);
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioMicTopoInit(_In_ PMINIPORTTOPOLOGY Miniport, _In_ PUNKNOWN UnknownAdapter,
                   _In_ PRESOURCELIST ResourceList, _In_ PPORTTOPOLOGY Port)
{
    UNREFERENCED_PARAMETER(Miniport);
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(Port);
    return STATUS_SUCCESS;
}

static const IMiniportTopologyVtbl g_T2AudioMicTopoVtbl = {
    (void *)T2AudioMicTopoQueryInterface,
    (void *)T2AudioMicTopoAddRef,
    (void *)T2AudioMicTopoRelease,
    (void *)T2AudioMicTopoGetDescription,
    (void *)T2AudioMicTopoDataRangeIntersection,
    (void *)T2AudioMicTopoInit
};

// Register "TopologyMic" + "WaveMic" and wire topology pin 1 -> wave pin 1.
NTSTATUS
T2AudioRegisterMicFilters(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList,
    _In_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    PPORT base = NULL;
    PPORTTOPOLOGY topoPort = NULL;
    PPORTWAVERT wavePort = NULL;
    PT2AUDIO_MIC_TOPOLOGY topo = NULL;
    PT2AUDIO_MIC_MINIPORT wave = NULL;
    NTSTATUS status;

    PAGED_CODE();

    topo = (PT2AUDIO_MIC_TOPOLOGY)ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*topo), T2AUDIO_MIC_POOL_TAG);
    wave = (PT2AUDIO_MIC_MINIPORT)ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*wave), T2AUDIO_MIC_POOL_TAG);
    if (topo == NULL || wave == NULL) {
        if (topo != NULL) {
            ExFreePoolWithTag(topo, T2AUDIO_MIC_POOL_TAG);
        }
        if (wave != NULL) {
            ExFreePoolWithTag(wave, T2AUDIO_MIC_POOL_TAG);
        }
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(topo, sizeof(*topo));
    topo->Interface.lpVtbl = &g_T2AudioMicTopoVtbl;
    topo->ReferenceCount = 1;
    RtlZeroMemory(wave, sizeof(*wave));
    wave->Interface.lpVtbl = &g_T2AudioMicMiniportVtbl;
    wave->ReferenceCount = 1;
    wave->DeviceContext = Context;

    status = PcNewPort(&base, &CLSID_PortTopology);
    if (NT_SUCCESS(status)) {
        status = base->lpVtbl->QueryInterface(base, &IID_IPortTopology, (PVOID *)&topoPort);
        base->lpVtbl->Release(base);
        base = NULL;
    }
    if (NT_SUCCESS(status)) {
        status = topoPort->lpVtbl->Init(topoPort, DeviceObject, Irp, (PUNKNOWN)&topo->Interface,
                                        NULL, ResourceList);
    }
    if (NT_SUCCESS(status)) {
        status = PcRegisterSubdevice(DeviceObject, L"TopologyMic", (PUNKNOWN)topoPort);
    }
    if (NT_SUCCESS(status)) {
        status = PcNewPort(&base, &CLSID_PortWaveRT);
    }
    if (NT_SUCCESS(status)) {
        status = base->lpVtbl->QueryInterface(base, &IID_IPortWaveRT, (PVOID *)&wavePort);
        base->lpVtbl->Release(base);
        base = NULL;
    }
    if (NT_SUCCESS(status)) {
        status = wavePort->lpVtbl->Init(wavePort, DeviceObject, Irp, (PUNKNOWN)&wave->Interface,
                                        NULL, ResourceList);
    }
    if (NT_SUCCESS(status)) {
        status = PcRegisterSubdevice(DeviceObject, L"WaveMic", (PUNKNOWN)wavePort);
    }
    if (NT_SUCCESS(status)) {
        status = PcRegisterPhysicalConnection(DeviceObject,
                                              (PUNKNOWN)topoPort, T2AUDIO_MIC_TOPO_PIN_BRIDGE,
                                              (PUNKNOWN)wavePort, T2AUDIO_MIC_WAVE_PIN_BRIDGE);
    }
    wave->Interface.lpVtbl->Release(&wave->Interface);
    topo->Interface.lpVtbl->Release(&topo->Interface);
    if (wavePort != NULL) {
        wavePort->lpVtbl->Release(wavePort);
    }
    if (topoPort != NULL) {
        topoPort->lpVtbl->Release(topoPort);
    }
    return status;
}
