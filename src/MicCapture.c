#include "T2AudioMiniport.h"
#include "BceProtocolLogic.h"
#include "T2AudioBufferLogic.h"
#include "T2AudioSpeakerLogic.h"
#include "T2AudioMicLogic.h"
#include "T2AudioMicDenoise.h"
#include <intrin.h>

// Capture paths (ROADMAP section 5): the built-in microphones
// (Parameters\MicInput=1, "Digital Mic") and the 3.5 mm headset microphone
// (Parameters\HeadsetMicInput=1, "Codec Input"). Both use the same code,
// driven by a T2AUDIO_CAPTURE_PATH (Context->Mic / Context->HeadsetMic).
//
//  1. Registration (T2AudioMicRegister, at probe time, stage 4+ with BCE I/O):
//     a contiguous host ring of Path->RingFrames frames is allocated, its
//     physical address written into the BufferStruct entry Path->Name
//     (input_streams[0], one buffer; appended if absent) and announced with
//     SET_INPUT_STREAM_ADDRESS_RANGES (msg 18) - kaiT2en's
//     t2audio_init_bs_stream_host + t2audio_cmd_set_input_stream_address_
//     ranges, and AppleAudio's 0x46F8 + 0x44E4. After a wake AppleAudio
//     announces the ranges again (T2AudioMicReRegister).
//  2. Capture (WaveRT "WaveMic" + topology "TopologyMic"; for the headset
//     AppleAudio's "WaveMicIn" + "TopologyMicIn"): START_IO on the device
//     (only with Path->StartIo); a 2 ms DPC moves the frames the T2 has
//     written (position from its UPDATE_TIMESTAMP, as kaiT2en's capture
//     pointer) into the client buffer, mono 48 kHz 32-bit - through the
//     16_1 mic DSP for the array, unchanged for the headset. The reader
//     trails the writer by T2AUDIO_MIC_CAPTURE_LAG frames.
//  3. Teardown (T2AudioMicUnregister, PnP stop): STOP_IO if running, clear
//     the BufferStruct entry, then free the ring - never the other way round.

#define T2AUDIO_MIC_COPY_PERIOD_MS 2
#define T2AUDIO_MIC_POOL_TAG       '2TMC'

// The fixed part of both paths; called right after the context is zeroed.
VOID
T2AudioMicInitPaths(_Inout_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    Context->Mic.Name = "Digital Mic";
    Context->Mic.Headset = FALSE;
    Context->Mic.BytesPerFrame = T2AUDIO_MIC_BYTES_PER_FRAME;
    Context->Mic.RingFrames = T2AUDIO_MIC_RING_FRAMES;
    Context->Mic.RingBytes = T2AUDIO_MIC_RING_BYTES;

    Context->HeadsetMic.Name = "Codec Input";
    Context->HeadsetMic.Headset = TRUE;
    Context->HeadsetMic.BytesPerFrame = T2AUDIO_HEADSET_MIC_BYTES_PER_FRAME;
    Context->HeadsetMic.RingFrames = T2AUDIO_HEADSET_MIC_RING_FRAMES;
    Context->HeadsetMic.RingBytes = T2AUDIO_HEADSET_MIC_RING_BYTES;
}

// ---------------------------------------------------------------------------
// BufferStruct and registration
// ---------------------------------------------------------------------------

// Index of the BufferStruct device Path->Name, or -1. Caller holds
// HardwareLock with HardwareReady set (BufferStruct is device memory).
static LONG
T2AudioMicFindDeviceLocked(_In_ PT2AUDIO_DEVICE_CONTEXT Context, _In_ PT2AUDIO_CAPTURE_PATH Path)
{
    SIZE_T nameSize;
    ULONG index;

    if (Context->BufferStruct == NULL || Path->Name == NULL) {
        return -1;
    }
    nameSize = strlen(Path->Name) + 1;
    for (index = 0; index < Context->BufferStructDevices && index < T2AUDIO_MAX_DEVICES; index++) {
        const T2AUDIO_DEVICE_METADATA *device = &Context->BufferStruct->Devices[index];

        if (RtlCompareMemory(device->Name, Path->Name, nameSize) == nameSize) {
            return (LONG)index;
        }
    }
    return -1;
}

// The boot-time table was validated only through BufferStructDevices. Check
// the next entry against the actual mapped BAR before AppleAudio-style append.
// Caller holds HardwareLock.
static BOOLEAN
T2AudioMicCanAppendDeviceLocked(_In_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    SIZE_T baseOffset;
    SIZE_T required;

    if (Context->BufferStruct == NULL || Context->Bar1Mapped == NULL ||
        Context->BufferStructDevices >= T2AUDIO_MAX_DEVICES) {
        return FALSE;
    }
    baseOffset = (SIZE_T)((PUCHAR)Context->BufferStruct - (PUCHAR)Context->Bar1Mapped);
    required = FIELD_OFFSET(T2AUDIO_BUFFER_STRUCT, Devices) +
        ((SIZE_T)Context->BufferStructDevices + 1) * sizeof(T2AUDIO_DEVICE_METADATA);
    return baseOffset <= Context->Bar1Size && required <= Context->Bar1Size - baseOffset;
}

// Find the existing entry or append a new device entry to BufferStruct,
// exactly matching AppleAudio.sys FUN_140004558 (which appends "Digital Mic"
// and increments NumDevices if not present at boot; the boot table has no
// "Codec Input" either, docs/logs/boot_log.csv).
// Caller holds HardwareLock with HardwareReady set.
static LONG
T2AudioMicFindOrAddDeviceLocked(_In_ PT2AUDIO_DEVICE_CONTEXT Context, _In_ PT2AUDIO_CAPTURE_PATH Path)
{
    LONG index = T2AudioMicFindDeviceLocked(Context, Path);
    SIZE_T nameSize;

    if (index >= 0 || Path->Name == NULL) {
        return index;
    }
    nameSize = strlen(Path->Name) + 1;
    if (nameSize <= sizeof(Context->BufferStruct->Devices[0].Name) &&
        T2AudioMicCanAppendDeviceLocked(Context)) {
        // (The new entry must lie inside the mapped BAR1: MapResources only
        // validated the table up to the devices present at boot.)
        ULONG newIdx = Context->BufferStructDevices;
        volatile T2AUDIO_DEVICE_METADATA *device =
            (volatile T2AUDIO_DEVICE_METADATA *)&Context->BufferStruct->Devices[newIdx];

        RtlZeroMemory((PVOID)device, sizeof(*device));
        RtlCopyMemory((PVOID)device->Name, Path->Name, nameSize);
        Context->BufferStructDevices++;
        Context->BufferStruct->NumDevices = (UCHAR)Context->BufferStructDevices;
        _mm_sfence();
        KdPrint(("T2Audio: BufferStruct added device[%u] '%s' (NumDevices=%u)\n",
                 newIdx, Path->Name, Context->BufferStructDevices));
        return (LONG)newIdx;
    }
    return -1;
}

BOOLEAN
T2AudioMicPresentInBufferStruct(_In_ PT2AUDIO_DEVICE_CONTEXT Context, _In_ PT2AUDIO_CAPTURE_PATH Path)
{
    KIRQL oldIrql;
    BOOLEAN present = FALSE;

    if (Context == NULL || Path == NULL || !Context->HardwareReady || Context->BufferStruct == NULL) {
        return FALSE;
    }
    KeAcquireSpinLock(&Context->HardwareLock, &oldIrql);
    // If the entry is already present in BufferStruct, return TRUE.
    if (T2AudioMicFindDeviceLocked(Context, Path) >= 0) {
        present = TRUE;
    } else if (T2AudioMicCanAppendDeviceLocked(Context)) {
        // AppleAudio dynamically appends the entry. Check the actual mapped
        // BAR capacity as well as the logical device-count limit; a short
        // table may have room for fewer entries than MAX_DEVICES.
        present = TRUE;
    }
    KeReleaseSpinLock(&Context->HardwareLock, oldIrql);
    return present;
}

// Write (Address != 0) or clear (Address == 0) input_streams[0] of the
// path's entry. Returns FALSE when the entry is not there or BAR1 is gone.
static BOOLEAN
T2AudioMicWriteBufferStruct(_In_ PT2AUDIO_DEVICE_CONTEXT Context, _In_ PT2AUDIO_CAPTURE_PATH Path,
                            _In_ ULONG64 Address, _In_ ULONG Size)
{
    KIRQL oldIrql;
    LONG index;
    BOOLEAN done = FALSE;

    KeAcquireSpinLock(&Context->HardwareLock, &oldIrql);
    if (Context->HardwareReady) {
        index = (Address != 0) ? T2AudioMicFindOrAddDeviceLocked(Context, Path)
                               : T2AudioMicFindDeviceLocked(Context, Path);
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

// msg 18 in AppleAudio's 837-byte form (stream id, one range); the first
// hardware run sent kaiT2en's bare header, which was acknowledged but did
// not prepare the stream (EXCERPT_20261029_part2_mics.md).
static NTSTATUS
T2AudioMicSendAddressRanges(_In_ PT2AUDIO_CAPTURE_PATH Path)
{
    UCHAR message[T2AUDIO_BCE_SET_INPUT_RANGES_SIZE];
    UCHAR reply[256];
    ULONG length;
    ULONG replySize = 0;
    NTSTATUS status;

    PAGED_CODE();

    length = T2AudioBceBuildSetInputRanges(message, sizeof(message),
                                           Path->BceDeviceId, Path->StreamId);
    if (length == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    status = T2AudioSendBceMessage(message, length, reply, sizeof(reply), &replySize);
    if (NT_SUCCESS(status) &&
        !T2AudioBceParseCommandResponse(reply, replySize,
                                        T2AUDIO_MSG_SET_INPUT_STREAM_ADDRESS_RANGES_RESPONSE)) {
        status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    KdPrint(("T2Audio: %s SET_INPUT_STREAM_ADDRESS_RANGES dev=0x%I64X stream=0x%I64X (%u bytes): 0x%08X (reply %u bytes)\n",
             Path->Name, Path->BceDeviceId, Path->StreamId, length, status, replySize));
    return status;
}

// Send SET_PROPERTY 'nsrt' (msg 9, 66 bytes) with (double)48000.0, matching
// AppleAudio.sys 0x14000445c. Sent right after SET_INPUT_STREAM_ADDRESS_RANGES.
static NTSTATUS
T2AudioMicSetSampleRate(_In_ PT2AUDIO_CAPTURE_PATH Path, _In_ ULONG SampleRateHz)
{
    unsigned char message[T2AUDIO_BCE_SET_PROPERTY_NSRT_SIZE];
    unsigned char reply[64];
    unsigned long length;
    ULONG replySize = 0;
    NTSTATUS status;

    PAGED_CODE();

    length = T2AudioBceBuildSetPropertyNominalSampleRate(message, sizeof(message),
                                                        Path->BceDeviceId, SampleRateHz);
    if (length == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    status = T2AudioSendBceMessage(message, length, reply, sizeof(reply), &replySize);
    if (NT_SUCCESS(status) &&
        !T2AudioBceParseCommandResponse(reply, replySize,
                                        T2AUDIO_MSG_SET_PROPERTY_RESPONSE)) {
        status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    KdPrint(("T2Audio: %s SET_PROPERTY 'nsrt' dev=0x%I64X (rate %u): 0x%08X (reply %u bytes)\n",
             Path->Name, Path->BceDeviceId, SampleRateHz, status, replySize));
    return status;
}

// The ring as a common buffer of a bus-master DMA adapter of our PCI device:
// the address handed to the T2 is then the device-logical one even with DMA
// remapping (Kernel DMA Protection) on. Each path has its own adapter.
static PVOID
T2AudioMicAllocateRing(_Inout_ PT2AUDIO_DEVICE_CONTEXT Context, _Inout_ PT2AUDIO_CAPTURE_PATH Path,
                       _Out_ PHYSICAL_ADDRESS *Logical)
{
    DEVICE_DESCRIPTION desc;
    PDEVICE_OBJECT pdo = NULL;
    PDMA_ADAPTER adapter;
    ULONG mapRegisters = 0;
    PVOID va;

    PAGED_CODE();

    Logical->QuadPart = 0;
    if (!NT_SUCCESS(PcGetPhysicalDeviceObject(Context->DeviceObject, &pdo)) || pdo == NULL) {
        return NULL;
    }
    RtlZeroMemory(&desc, sizeof(desc));
    desc.Version = DEVICE_DESCRIPTION_VERSION;
    desc.Master = TRUE;
    desc.ScatterGather = FALSE;
    desc.Dma32BitAddresses = TRUE;
    desc.Dma64BitAddresses = TRUE;
    desc.InterfaceType = PCIBus;
    desc.MaximumLength = Path->RingBytes;
    adapter = IoGetDmaAdapter(pdo, &desc, &mapRegisters);
    if (adapter == NULL) {
        KdPrint(("T2Audio: %s: IoGetDmaAdapter failed\n", Path->Name));
        return NULL;
    }
    va = adapter->DmaOperations->AllocateCommonBuffer(adapter, Path->RingBytes, Logical, TRUE);
    if (va == NULL) {
        adapter->DmaOperations->PutDmaAdapter(adapter);
        return NULL;
    }
    Path->DmaAdapter = adapter;
    return va;
}

static VOID
T2AudioMicFreeRing(_Inout_ PT2AUDIO_CAPTURE_PATH Path, _In_ PVOID Va)
{
    PDMA_ADAPTER adapter = (PDMA_ADAPTER)Path->DmaAdapter;

    PAGED_CODE();

    if (adapter != NULL) {
        adapter->DmaOperations->FreeCommonBuffer(adapter, Path->RingBytes,
                                                 Path->BufferPa, Va, TRUE);
        adapter->DmaOperations->PutDmaAdapter(adapter);
        Path->DmaAdapter = NULL;
    }
}

NTSTATUS
T2AudioMicRegister(_Inout_ PT2AUDIO_DEVICE_CONTEXT Context, _Inout_ PT2AUDIO_CAPTURE_PATH Path)
{
    PHYSICAL_ADDRESS logical;
    PVOID va;
    NTSTATUS status;

    PAGED_CODE();

    if (Context == NULL || Path == NULL || !Path->Enabled || !Path->FormatValid ||
        Path->BceDeviceId == 0 || Path->StreamId == 0 || Path->Registered ||
        Path->RingBytes == 0) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    va = T2AudioMicAllocateRing(Context, Path, &logical);
    if (va == NULL) {
        KdPrint(("T2Audio: %s ring allocation (%u bytes) failed\n", Path->Name, Path->RingBytes));
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(va, Path->RingBytes);
    Path->BufferVa = va;
    Path->BufferPa = logical;
    Path->BufferSize = Path->RingBytes;
    KdPrint(("T2Audio: %s ring: common buffer va=%p logical=0x%I64X physical=0x%I64X\n",
             Path->Name, va, (ULONG64)logical.QuadPart, (ULONG64)MmGetPhysicalAddress(va).QuadPart));

    if (!T2AudioMicWriteBufferStruct(Context, Path, (ULONG64)Path->BufferPa.QuadPart,
                                     Path->BufferSize)) {
        KdPrint(("T2Audio: %s: no BufferStruct entry and no room to add one\n", Path->Name));
        status = STATUS_OBJECT_NAME_NOT_FOUND;
    } else {
        status = T2AudioMicSendAddressRanges(Path);
        if (NT_SUCCESS(status)) {
            status = T2AudioMicSetSampleRate(Path, 48000);
        }
    }
    if (!NT_SUCCESS(status)) {
        (VOID)T2AudioMicWriteBufferStruct(Context, Path, 0, 0);
        T2AudioMicFreeRing(Path, va);
        Path->BufferVa = NULL;
        Path->BufferSize = 0;
        return status;
    }
    Path->Registered = TRUE;
    KdPrint(("T2Audio: %s ring registered: pa=0x%I64X size=0x%X (%u frames)\n",
             Path->Name, (ULONG64)Path->BufferPa.QuadPart, Path->BufferSize, Path->RingFrames));
    return STATUS_SUCCESS;
}

// After a wake (D0): write the entry again and repeat msg 18, as AppleAudio
// does for every input stream (RE_appleaudio_protocol.md section 7).
NTSTATUS
T2AudioMicReRegister(_Inout_ PT2AUDIO_DEVICE_CONTEXT Context, _Inout_ PT2AUDIO_CAPTURE_PATH Path)
{
    NTSTATUS status;

    PAGED_CODE();

    if (Context == NULL || Path == NULL || !Path->Registered || Path->BufferVa == NULL ||
        Path->IoActive != 0) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (!T2AudioMicWriteBufferStruct(Context, Path, (ULONG64)Path->BufferPa.QuadPart,
                                     Path->BufferSize)) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    status = T2AudioMicSendAddressRanges(Path);
    if (NT_SUCCESS(status)) {
        status = T2AudioMicSetSampleRate(Path, 48000);
    }
    return status;
}

VOID
T2AudioMicUnregister(_Inout_ PT2AUDIO_DEVICE_CONTEXT Context, _Inout_ PT2AUDIO_CAPTURE_PATH Path)
{
    PVOID va;
    KIRQL oldIrql;

    PAGED_CODE();

    if (Context == NULL || Path == NULL || Path->BufferVa == NULL) {
        return;
    }
    // StreamMutex orders this with a capture SetState(RUN): either START_IO
    // went out first (IoActive is 1 and is stopped here), or the stream sees
    // Registered == FALSE and sends none. Without it a START_IO racing the
    // teardown would let the T2 write into the freed ring.
    KeWaitForSingleObject(&Context->StreamMutex, Executive, KernelMode, FALSE, NULL);
    // The T2 must have stopped writing before the ring goes away.
    if (InterlockedExchange(&Path->IoActive, 0) != 0) {
        NTSTATUS status = T2AudioStopIoDevice(Context, Path->DeviceId);

        KdPrint(("T2Audio: %s teardown STOP_IO: 0x%08X\n", Path->Name, status));
    }
    Path->Registered = FALSE;
    KeReleaseMutex(&Context->StreamMutex, FALSE);
    (VOID)T2AudioMicWriteBufferStruct(Context, Path, 0, 0);

    // Detach under the hardware lock so the capture DPC stops reading it.
    KeAcquireSpinLock(&Context->HardwareLock, &oldIrql);
    va = Path->BufferVa;
    Path->BufferVa = NULL;
    KeReleaseSpinLock(&Context->HardwareLock, oldIrql);
    KeFlushQueuedDpcs();

    T2AudioMicFreeRing(Path, va);
    Path->BufferSize = 0;
    KdPrint(("T2Audio: %s ring released\n", Path->Name));
}

// ---------------------------------------------------------------------------
// Capture stream
// ---------------------------------------------------------------------------

typedef struct _T2AUDIO_MIC_STREAM {
    IMiniportWaveRTStream Interface;
    LONG ReferenceCount;
    PT2AUDIO_DEVICE_CONTEXT DeviceContext;
    PT2AUDIO_CAPTURE_PATH Path;
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
    T2AUDIO_MIC_GATE_STATE Gate;
    // Spectral noise suppressor (MicNoiseSuppression=2), 7.7 KB.
    T2AUDIO_MIC_DENOISE_STATE Denoise;
    // Run diagnostics.
    ULONG ReAims;
    ULONG64 SilentFrames;
    ULONG64 CapturedFrames;
    LONG PeakDb;
    ULONG MaxCopyUs;
    KTIMER CopyTimer;
    KDPC CopyDpc;
    BOOLEAN TimerActive;
    // Client container: 32 bits, or 16 / 24 with Parameters\ExtraFormats.
    ULONG BitsPerSample;
    ULONG BytesPerFrame;
    // Event-driven mode (IMiniportWaveRTStreamNotification), as on the render
    // side: events set on every packet boundary. Protected by StreamLock.
#define T2AUDIO_MIC_MAX_NOTIFY_EVENTS 4
    ULONG NotificationCount;
    ULONG64 NotifyPacket;
    PKEVENT NotifyEvents[T2AUDIO_MIC_MAX_NOTIFY_EVENTS];
    ULONG NotifyEventCount;
} T2AUDIO_MIC_STREAM, *PT2AUDIO_MIC_STREAM;

// Move every frame up to "now" into the client buffer. StreamLock held.
static VOID
T2AudioMicAdvanceLocked(_Inout_ PT2AUDIO_MIC_STREAM s)
{
    PT2AUDIO_DEVICE_CONTEXT context = s->DeviceContext;
    PT2AUDIO_CAPTURE_PATH path = s->Path;
    LARGE_INTEGER qpc, qpcFreq;
    ULONG64 target;
    ULONG64 frame;
    ULONG clientFrames;
    BOOLEAN haveWriter = FALSE;
    ULONG64 writeFrame = 0;
    PUCHAR ring = NULL;
    PUCHAR client;

    if (s->State != KSSTATE_RUN || s->SystemBufferVa == NULL || s->BytesPerFrame == 0 ||
        s->AudioBufferSize < s->BytesPerFrame ||
        context == NULL || path == NULL) {
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
    clientFrames = s->AudioBufferSize / s->BytesPerFrame;
    if (target - frame > clientFrames) {
        frame = target - clientFrames;   // the client fell behind: drop the oldest
    }

    // The T2 write position: the latest anchor of this run plus the time
    // since, minus the device's input latency (kaiT2en's capture pointer).
    KeAcquireSpinLockAtDpcLevel(&context->ClockLock);
    if (path->ClockAnchorNs >= s->RunStartNs && path->ClockGeneration != 0 &&
        path->ClockAnchorQpc != 0) {
        ULONG64 elapsed = 0;

        if ((ULONG64)qpc.QuadPart > path->ClockAnchorQpc && qpcFreq.QuadPart > 0) {
            elapsed = T2AudioTicksToFrames((ULONG64)qpc.QuadPart - path->ClockAnchorQpc,
                                           48000, (ULONG64)qpcFreq.QuadPart);
        }
        writeFrame = path->ClockAnchorFrames + elapsed;
        writeFrame = (writeFrame > path->LatencyFrames) ? writeFrame - path->LatencyFrames : 0;
        haveWriter = TRUE;
    }
    KeReleaseSpinLockFromDpcLevel(&context->ClockLock);

    if (haveWriter) {
        if (T2AudioMicCaptureAim(target, &s->T2Offset, writeFrame, s->OffsetValid ? 1 : 0)) {
            if (s->OffsetValid) {
                s->ReAims++;
                if (s->ReAims <= 8 || (s->ReAims % 100) == 0) {
                    KdPrint(("T2Audio: %s clock: re-aimed (#%u, write=%I64u host=%I64u)\n",
                             path->Name, s->ReAims, writeFrame, target));
                }
            }
            s->OffsetValid = TRUE;
        }
    }

    client = (PUCHAR)s->SystemBufferVa;
    KeAcquireSpinLockAtDpcLevel(&context->HardwareLock);
    if (path->Registered && path->BufferVa != NULL && !context->PoweredDown) {
        ring = (PUCHAR)path->BufferVa;
    }
    for (; frame < target; frame++) {
        LONGLONG t2Frame = (LONGLONG)frame + s->T2Offset;
        ULONG out = 0;

        if (ring != NULL && s->OffsetValid && t2Frame >= 0 && (ULONG64)t2Frame < writeFrame) {
            const volatile ULONG *slot = (const volatile ULONG *)(ring +
                (SIZE_T)T2AudioMicRingSlot(t2Frame, path->RingFrames) * path->BytesPerFrame);
            int sample;

            if (path->Headset) {
                sample = T2AudioHeadsetMicSample(slot[0]);
            } else {
                unsigned int raw[3];

                raw[0] = slot[0];
                raw[1] = slot[1];
                raw[2] = slot[2];
                s->Dsp.OutGainQ16 = g_T2AudioMicGainQ16[(context->MicGainDb > T2AUDIO_MIC_GAIN_DB_MAX)
                                                            ? T2AUDIO_MIC_GAIN_DB_DEFAULT : context->MicGainDb];
                sample = T2AudioMicProcessFrameEx(&s->Dsp, raw, context->MicDsp, context->MicVoiceMic);
            }
            // MicNoiseSuppression: 1 = gate (pauses only), 2 = spectral
            // suppressor (also under speech).
            if (context->MicNoiseSuppression >= 2) {
                out = (ULONG)T2AudioMicDenoise(&s->Denoise, sample);
            } else {
                out = (ULONG)T2AudioMicNoiseGate(&s->Gate, sample, context->MicNoiseSuppression);
            }
            s->CapturedFrames++;
        } else {
            s->SilentFrames++;
        }
        (VOID)T2AudioMicStoreSample(client + (SIZE_T)(ULONG)(frame % clientFrames) * s->BytesPerFrame,
                                    (int)out, s->BitsPerSample);
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
    ULONG bytesPerFrame;

    KeAcquireSpinLock(&s->StreamLock, &oldIrql);
    size = s->AudioBufferSize;
    bytesPerFrame = s->BytesPerFrame;
    if (s->State == KSSTATE_STOP || bytesPerFrame == 0 || size < bytesPerFrame) {
        KeReleaseSpinLock(&s->StreamLock, oldIrql);
        return 0;
    }
    T2AudioMicAdvanceLocked(s);
    frame = s->LastFrame;
    // Event-driven clients: one signal per packet boundary crossed.
    if (s->State == KSSTATE_RUN && s->NotifyEventCount != 0) {
        ULONG64 packet = T2AudioNotificationPacket(frame, bytesPerFrame, size, s->NotificationCount);

        if (packet != s->NotifyPacket) {
            ULONG i;

            s->NotifyPacket = packet;
            for (i = 0; i < s->NotifyEventCount; i++) {
                KeSetEvent(s->NotifyEvents[i], 0, FALSE);
            }
        }
    }
    KeReleaseSpinLock(&s->StreamLock, oldIrql);
    return (ULONG)((frame * bytesPerFrame) % size);
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
        IsEqualGUIDAligned(InterfaceId, &IID_IMiniportWaveRTStream) ||
        IsEqualGUIDAligned(InterfaceId, &IID_IMiniportWaveRTStreamNotification)) {
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
        if (s->Path != NULL) {
            (VOID)InterlockedCompareExchangePointer(&s->Path->RunningStream, NULL, s);
        }
        T2AudioMicReleaseBuffer(s);
        if (s->PortStream != NULL) {
            s->PortStream->lpVtbl->Release((PVOID)s->PortStream);
        }
        ExFreePoolWithTag(s, T2AUDIO_MIC_POOL_TAG);
    }
    return (ULONG)references;
}

// Mono, 48 kHz PCM (WAVEFORMATEX or EXTENSIBLE with a mono mask) in a 32-bit
// container, or with AllowExtra (Parameters\ExtraFormats) also 16 and packed
// 24 bits for exclusive-mode recorders. *Bits receives the container size.
static NTSTATUS
T2AudioMicValidateFormat(_In_opt_ PKSDATAFORMAT DataFormat, _In_ BOOLEAN AllowExtra,
                         _Out_opt_ PULONG Bits)
{
    PKSDATAFORMAT_WAVEFORMATEX wf;
    ULONG bits;
    ULONG block;

    if (Bits != NULL) {
        *Bits = 0;
    }
    if (DataFormat == NULL || DataFormat->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX) ||
        !IsEqualGUIDAligned(&DataFormat->MajorFormat, &KSDATAFORMAT_TYPE_AUDIO) ||
        !IsEqualGUIDAligned(&DataFormat->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM) ||
        !IsEqualGUIDAligned(&DataFormat->Specifier, &KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) {
        return STATUS_INVALID_PARAMETER;
    }
    wf = (PKSDATAFORMAT_WAVEFORMATEX)DataFormat;
    bits = wf->WaveFormatEx.wBitsPerSample;
    if (bits != 32 && !(AllowExtra && (bits == 16 || bits == 24))) {
        return STATUS_INVALID_PARAMETER;
    }
    block = bits / 8u;
    if (wf->WaveFormatEx.nChannels != 1 || wf->WaveFormatEx.nSamplesPerSec != 48000 ||
        wf->WaveFormatEx.nBlockAlign != block ||
        wf->WaveFormatEx.nAvgBytesPerSec != 48000u * block) {
        return STATUS_INVALID_PARAMETER;
    }
    if (wf->WaveFormatEx.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        PKSDATAFORMAT_WAVEFORMATEXTENSIBLE ext = (PKSDATAFORMAT_WAVEFORMATEXTENSIBLE)DataFormat;

        if (DataFormat->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE) ||
            !IsEqualGUIDAligned(&ext->WaveFormatExt.SubFormat, &KSDATAFORMAT_SUBTYPE_PCM) ||
            ext->WaveFormatExt.Samples.wValidBitsPerSample > bits ||
            (ext->WaveFormatExt.dwChannelMask != KSAUDIO_SPEAKER_MONO &&
             ext->WaveFormatExt.dwChannelMask != 0)) {
            return STATUS_INVALID_PARAMETER;
        }
    } else if (wf->WaveFormatEx.wFormatTag != WAVE_FORMAT_PCM) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Bits != NULL) {
        *Bits = bits;
    }
    return STATUS_SUCCESS;
}

static BOOLEAN
T2AudioMicExtraFormats(_In_opt_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    return (Context != NULL && Context->ExtraFormats) ? TRUE : FALSE;
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioMicStreamSetFormat(_In_ PMINIPORTWAVERTSTREAM Stream, _In_ PKSDATAFORMAT DataFormat)
{
    PT2AUDIO_MIC_STREAM s = CONTAINING_RECORD(Stream, T2AUDIO_MIC_STREAM, Interface);
    ULONG bits = 0;
    NTSTATUS status = T2AudioMicValidateFormat(DataFormat, T2AudioMicExtraFormats(s->DeviceContext), &bits);

    // The client buffer was sized for the current frame size.
    if (NT_SUCCESS(status) && s->AudioBufferMdl != NULL && bits != s->BitsPerSample) {
        status = STATUS_INVALID_DEVICE_REQUEST;
    }
    if (NT_SUCCESS(status)) {
        KIRQL oldIrql;

        KeAcquireSpinLock(&s->StreamLock, &oldIrql);
        s->BitsPerSample = bits;
        s->BytesPerFrame = bits / 8u;
        KeReleaseSpinLock(&s->StreamLock, oldIrql);
    }
    return status;
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
        if (!s->Path->StartIo) {
            KdPrint(("T2Audio: %s RUN without START_IO (%s=0): the endpoint records silence\n",
                     s->Path->Name, s->Path->Headset ? "HeadsetMicStartIo" : "MicStartIo"));
        } else if (context->BceIoEnabled && s->Path->Registered && s->Path->DeviceId != 0 &&
            !s->HardwareIoStarted) {
            status = T2AudioStartIoDevice(context, s->Path->DeviceId);
            KdPrint(("T2Audio: %s RUN START_IO: 0x%08X\n", s->Path->Name, status));
            if (!NT_SUCCESS(status)) {
                return status;
            }
            s->HardwareIoStarted = TRUE;
            InterlockedExchange(&s->Path->IoActive, 1);
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
        s->NotifyPacket = T2AudioNotificationPacket(s->LastFrame, s->BytesPerFrame,
                                                    s->AudioBufferSize, s->NotificationCount);
        if (s->LastFrame == 0) {
            T2AudioMicReset(&s->Dsp);
            T2AudioMicGateReset(&s->Gate);
            T2AudioMicDenoiseReset(&s->Denoise);
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
    KdPrint(("T2Audio: %s run summary: captured=%I64u silent=%I64u reAims=%u maxCopyUs=%u\n",
             s->Path->Name, s->CapturedFrames, s->SilentFrames, s->ReAims, s->MaxCopyUs));
    if (context->MicNoiseSuppression >= 2) {
        const T2AUDIO_MIC_DENOISE_STATE *d = &s->Denoise;

        KdPrint(("T2Audio: %s denoise: hops=%u pos=%u gs[2]=%d gs[10]=%d gs[60]=%d noise[10]=%I64d ps[10]=%I64d in[0]=%d out[0]=%d\n",
                 s->Path->Name, d->Hops, d->Pos, d->Gs[2], d->Gs[10], d->Gs[60], d->Noise[10], d->Ps[10],
                 d->In[0], d->Out[0]));
    }
    if (!s->Path->Headset && context->MicDsp == 2 && s->Dsp.Anc.Samples != 0) {
        const T2AUDIO_MIC_ANC_STATE *a = &s->Dsp.Anc;

        // Mean powers (x^2/16 of 24-bit samples), adaption share, resets.
        KdPrint(("T2Audio: mic canceller: voice=%I64u refs=%I64u out=%I64u adapted=%I64u/%I64u resets=%u mic0=%I64u mic1=%I64u mic2=%I64u voiceMic=%u\n",
                 a->SumVoice / a->Samples, a->SumRef / a->Samples, a->SumOut / a->Samples,
                 a->AdaptSamples, a->Samples, a->Resets,
                 a->SumMic[0] / a->Samples, a->SumMic[1] / a->Samples, a->SumMic[2] / a->Samples,
                 context->MicVoiceMic, a->MatchGain));
    }
    if (s->HardwareIoStarted) {
        if (InterlockedExchange(&s->Path->IoActive, 0) != 0) {
            NTSTATUS status = T2AudioStopIoDevice(context, s->Path->DeviceId);

            KdPrint(("T2Audio: %s leave RUN STOP_IO: 0x%08X\n", s->Path->Name, status));
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

    if (State < KSSTATE_STOP || State > KSSTATE_RUN || context == NULL || s->Path == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    KeWaitForSingleObject(&context->StreamMutex, Executive, KernelMode, FALSE, NULL);
    status = T2AudioMicSetStateLocked(s, State);
    if (s->State == KSSTATE_RUN) {
        s->Path->RunningStream = s;
    } else if (s->Path->RunningStream == (PVOID)s) {
        s->Path->RunningStream = NULL;
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

    if (s == NULL || s->DeviceContext == NULL || s->Path == NULL || s->State != KSSTATE_RUN ||
        !s->HardwareIoStarted || s->Path->IoActive != 0) {
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
    if (s->BytesPerFrame == 0 || RequestedSize < s->BytesPerFrame || s->AudioBufferMdl != NULL || s->PortStream == NULL ||
        !T2AudioAlignSizeUpToFrame(RequestedSize, s->BytesPerFrame, &size)) {
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
        ULONG frames = (s->Path != NULL) ? s->Path->LatencyFrames : 0;

        RtlZeroMemory(Latency, sizeof(*Latency));
        Latency->FifoSize = T2AUDIO_MIC_CAPTURE_LAG * s->BytesPerFrame;
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

// Event-driven mode (IMiniportWaveRTStreamNotification), as for rendering.
static NTSTATUS STDMETHODCALLTYPE
T2AudioMicStreamAllocateBufferWithNotification(
    _In_ PMINIPORTWAVERTSTREAM Stream,
    _In_ ULONG NotificationCount,
    _In_ ULONG RequestedSize,
    _Out_ PMDL *AudioBufferMdl,
    _Out_ ULONG *ActualSize,
    _Out_ ULONG *OffsetFromFirstPage,
    _Out_ MEMORY_CACHING_TYPE *CacheType)
{
    PT2AUDIO_MIC_STREAM s = CONTAINING_RECORD(Stream, T2AUDIO_MIC_STREAM, Interface);
    NTSTATUS status;
    KIRQL oldIrql;

    if (NotificationCount < 1 || NotificationCount > 2) {
        return STATUS_INVALID_PARAMETER;
    }
    status = T2AudioMicStreamAllocateAudioBuffer(Stream, RequestedSize, AudioBufferMdl, ActualSize,
                                                 OffsetFromFirstPage, CacheType);
    if (NT_SUCCESS(status)) {
        KeAcquireSpinLock(&s->StreamLock, &oldIrql);
        s->NotificationCount = NotificationCount;
        s->NotifyPacket = 0;
        KeReleaseSpinLock(&s->StreamLock, oldIrql);
    }
    return status;
}

static VOID STDMETHODCALLTYPE
T2AudioMicStreamFreeBufferWithNotification(_In_ PMINIPORTWAVERTSTREAM Stream, _In_ PMDL AudioBufferMdl,
                                           _In_ ULONG BufferSize)
{
    PT2AUDIO_MIC_STREAM s = CONTAINING_RECORD(Stream, T2AUDIO_MIC_STREAM, Interface);
    KIRQL oldIrql;

    T2AudioMicStreamFreeAudioBuffer(Stream, AudioBufferMdl, BufferSize);
    KeAcquireSpinLock(&s->StreamLock, &oldIrql);
    s->NotificationCount = 0;
    KeReleaseSpinLock(&s->StreamLock, oldIrql);
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioMicStreamRegisterNotificationEvent(_In_ PMINIPORTWAVERTSTREAM Stream, _In_ PKEVENT NotificationEvent)
{
    PT2AUDIO_MIC_STREAM s = CONTAINING_RECORD(Stream, T2AUDIO_MIC_STREAM, Interface);
    NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;
    KIRQL oldIrql;
    ULONG i;

    if (NotificationEvent == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    KeAcquireSpinLock(&s->StreamLock, &oldIrql);
    for (i = 0; i < s->NotifyEventCount; i++) {
        if (s->NotifyEvents[i] == NotificationEvent) {
            status = STATUS_SUCCESS;
            break;
        }
    }
    if (i == s->NotifyEventCount && i < T2AUDIO_MIC_MAX_NOTIFY_EVENTS) {
        s->NotifyEvents[s->NotifyEventCount++] = NotificationEvent;
        status = STATUS_SUCCESS;
    }
    KeReleaseSpinLock(&s->StreamLock, oldIrql);
    return status;
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioMicStreamUnregisterNotificationEvent(_In_ PMINIPORTWAVERTSTREAM Stream, _In_ PKEVENT NotificationEvent)
{
    PT2AUDIO_MIC_STREAM s = CONTAINING_RECORD(Stream, T2AUDIO_MIC_STREAM, Interface);
    NTSTATUS status = STATUS_NOT_FOUND;
    KIRQL oldIrql;
    ULONG i;

    KeAcquireSpinLock(&s->StreamLock, &oldIrql);
    for (i = 0; i < s->NotifyEventCount; i++) {
        if (s->NotifyEvents[i] == NotificationEvent) {
            s->NotifyEvents[i] = s->NotifyEvents[s->NotifyEventCount - 1];
            s->NotifyEvents[--s->NotifyEventCount] = NULL;
            status = STATUS_SUCCESS;
            break;
        }
    }
    KeReleaseSpinLock(&s->StreamLock, oldIrql);
    return status;
}

static const IMiniportWaveRTStreamNotificationVtbl g_T2AudioMicStreamVtbl = {
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
    (void *)T2AudioMicStreamGetRegister,
    (void *)T2AudioMicStreamAllocateBufferWithNotification,
    (void *)T2AudioMicStreamFreeBufferWithNotification,
    (void *)T2AudioMicStreamRegisterNotificationEvent,
    (void *)T2AudioMicStreamUnregisterNotificationEvent
};

// ---------------------------------------------------------------------------
// Capture WaveRT miniport ("WaveMic")
// ---------------------------------------------------------------------------

typedef struct _T2AUDIO_MIC_MINIPORT {
    IMiniportWaveRT Interface;
    LONG ReferenceCount;
    PT2AUDIO_DEVICE_CONTEXT DeviceContext;
    PT2AUDIO_CAPTURE_PATH Path;
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
    PT2AUDIO_MIC_MINIPORT m = CONTAINING_RECORD((PMINIPORTWAVERT)Miniport, T2AUDIO_MIC_MINIPORT, Interface);

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
        IsEqualGUIDAligned(&DataRange->Specifier, &KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) {
        // A complete format: accept it as it is (16/24 bits with ExtraFormats).
        if (!NT_SUCCESS(T2AudioMicValidateFormat(DataRange, T2AudioMicExtraFormats(m->DeviceContext), NULL))) {
            return STATUS_NO_MATCH;
        }
        if (OutputBufferLength == 0) {
            *ResultantFormatLength = DataRange->FormatSize;
            return STATUS_BUFFER_OVERFLOW;
        }
        if (OutputBufferLength < DataRange->FormatSize || ResultantFormat == NULL) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        RtlCopyMemory(ResultantFormat, DataRange, DataRange->FormatSize);
        *ResultantFormatLength = DataRange->FormatSize;
        return STATUS_SUCCESS;
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
    ULONG bits = 0;

    if (Stream == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Stream = NULL;
    if (m->Path == NULL || !Capture || Pin != T2AUDIO_MIC_WAVE_PIN_CAPTURE ||
        !NT_SUCCESS(T2AudioMicValidateFormat(DataFormat, T2AudioMicExtraFormats(m->DeviceContext), &bits))) {
        return STATUS_INVALID_PARAMETER;
    }
    s = (PT2AUDIO_MIC_STREAM)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*s), T2AUDIO_MIC_POOL_TAG);
    if (s == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(s, sizeof(*s));
    s->Interface.lpVtbl = (IMiniportWaveRTStreamVtbl *)&g_T2AudioMicStreamVtbl;
    s->ReferenceCount = 1;
    s->DeviceContext = m->DeviceContext;
    s->BitsPerSample = bits;
    s->BytesPerFrame = bits / 8u;
    s->Path = m->Path;
    s->PortStream = PortStream;
    s->State = KSSTATE_STOP;
    if (PortStream != NULL) {
        PortStream->lpVtbl->AddRef((PVOID)PortStream);
    }
    T2AudioMicReset(&s->Dsp);
    T2AudioMicGateReset(&s->Gate);
    T2AudioMicDenoiseReset(&s->Denoise);
    KeInitializeSpinLock(&s->StreamLock);
    KeInitializeTimer(&s->CopyTimer);
    KeInitializeDpc(&s->CopyDpc, T2AudioMicCopyTick, s);
    *Stream = &s->Interface;
    KdPrint(("T2Audio: %s NewStream\n", s->Path->Name));
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
// Microphone topology ("TopologyMic"): microphone -> bridge. The endpoint is a
// plain mono microphone: the three capsules are combined in the driver (MicDsp),
// so Windows must not treat it as an array and expect one channel per capsule.
// ---------------------------------------------------------------------------

static NTSTATUS
T2AudioMicBasicSupport(_Inout_ PPCPROPERTY_REQUEST Request)
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
    Request->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
    return STATUS_BUFFER_OVERFLOW;
}

// ---------------------------------------------------------------------------
// Microphone jack state. The headset pin ("TopologyMicIn") carries the
// 3.5 mm jack ('inpt'/'jack' of "Codec Input", polled in BceTransport.c);
// the built-in array pin yields to it with auto-switch: "unplugged" while a
// headset is plugged in, so Windows moves the default recording device over
// and back by itself. Both use KSPROPERTY_JACK_DESCRIPTION(2) and
// KSEVENT_PINCAPS_JACKINFOCHANGE, as the headphone/speaker topology does.
// ---------------------------------------------------------------------------

typedef struct _T2AUDIO_MIC_TOPOLOGY {
    IMiniportTopology Interface;
    LONG ReferenceCount;
    PT2AUDIO_DEVICE_CONTEXT DeviceContext;
    PT2AUDIO_CAPTURE_PATH Path;
} T2AUDIO_MIC_TOPOLOGY, *PT2AUDIO_MIC_TOPOLOGY;

static BOOLEAN
T2AudioHeadsetMicJackConnected(_In_opt_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    if (Context == NULL) {
        return TRUE;
    }
    return T2AudioJackReportConnected(Context->JackEvents, Context->HeadsetMicJackSeen ? 1 : 0,
                                      Context->HeadsetMicJackPlugged) ? TRUE : FALSE;
}

// Whether the built-in microphones follow the headset jack: auto-switch on
// and a registered headset endpoint to switch to.
static BOOLEAN
T2AudioBuiltInMicYields(_In_opt_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    return (Context != NULL && Context->HeadphoneAutoMute && Context->JackEvents &&
            Context->HeadsetMic.Enabled && Context->HeadsetMic.FiltersRegistered) ? TRUE : FALSE;
}

// Built-in array: "unplugged" while a headset is plugged in. The headset
// counts only while the headphone jack (verified on hardware) reads plugged
// too - it is the same combo jack - so a stuck 'inpt' read cannot hide the
// built-in microphones for good.
static BOOLEAN
T2AudioBuiltInMicJackConnected(_In_opt_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    if (Context == NULL) {
        return TRUE;
    }
    return T2AudioJackYieldConnected(T2AudioBuiltInMicYields(Context) ? 1u : 0u, Context->JackEvents,
                                     (Context->HeadsetMicJackSeen && Context->JackSeenPlugged) ? 1 : 0,
                                     (Context->HeadsetMicJackPlugged != 0 &&
                                      Context->HeadphoneJackPlugged != 0) ? 1 : 0) ? TRUE : FALSE;
}

static NTSTATUS
T2AudioMicJackProperty(_Inout_ PPCPROPERTY_REQUEST Request)
{
    PT2AUDIO_MIC_TOPOLOGY t;
    ULONG pinId = (ULONG)-1;
    ULONG needed;
    PKSMULTIPLE_ITEM item;
    BOOLEAN connected;
    BOOLEAN detect;

    if (Request == NULL || Request->PropertyItem == NULL || Request->MajorTarget == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    t = CONTAINING_RECORD((PMINIPORTTOPOLOGY)Request->MajorTarget, T2AUDIO_MIC_TOPOLOGY, Interface);
    if (Request->InstanceSize >= sizeof(ULONG) && Request->Instance != NULL) {
        pinId = *(PULONG)Request->Instance;
    }
    if (t->Path == NULL || (pinId != T2AUDIO_MIC_TOPO_PIN_MIC && pinId != (ULONG)-1)) {
        return STATUS_NOT_SUPPORTED;
    }
    if (Request->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        return T2AudioMicBasicSupport(Request);
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
    if (t->Path->Headset) {
        connected = T2AudioHeadsetMicJackConnected(t->DeviceContext);
        detect = (t->DeviceContext != NULL && t->DeviceContext->JackEvents) ? TRUE : FALSE;
    } else {
        connected = T2AudioBuiltInMicJackConnected(t->DeviceContext);
        detect = T2AudioBuiltInMicYields(t->DeviceContext);
    }
    item = (PKSMULTIPLE_ITEM)Request->Value;
    item->Size = needed;
    item->Count = 1;
    if (Request->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION) {
        PKSJACK_DESCRIPTION jack = (PKSJACK_DESCRIPTION)(item + 1);

        RtlZeroMemory(jack, sizeof(*jack));
        jack->ChannelMapping = KSAUDIO_SPEAKER_MONO;
        jack->Color = 0;
        if (t->Path->Headset) {
            jack->ConnectionType = eConnType3Point5mm;
            jack->GeoLocation = eGeoLocLeft;      // the combo jack of the MacBook Pro 16"
            jack->GenLocation = eGenLocPrimaryBox;
            jack->PortConnection = ePortConnJack;
        } else {
            jack->ConnectionType = eConnTypeUnknown;
            jack->GeoLocation = eGeoLocNotApplicable;
            jack->GenLocation = eGenLocInternal;
            jack->PortConnection = ePortConnIntegratedDevice;
        }
        jack->IsConnected = connected;
    } else {
        PKSJACK_DESCRIPTION2 jack2 = (PKSJACK_DESCRIPTION2)(item + 1);

        RtlZeroMemory(jack2, sizeof(*jack2));
        jack2->DeviceStateInfo = connected ? 1 : 0;
        jack2->JackCapabilities = detect ? JACKDESC2_PRESENCE_DETECT_CAPABILITY : 0;
    }
    Request->ValueSize = needed;
    return STATUS_SUCCESS;
}

static NTSTATUS
T2AudioMicJackEvent(_In_ PPCEVENT_REQUEST Request)
{
    PT2AUDIO_MIC_TOPOLOGY t;
    PPORTEVENTS events;

    if (Request == NULL || Request->MajorTarget == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    t = CONTAINING_RECORD((PMINIPORTTOPOLOGY)Request->MajorTarget, T2AUDIO_MIC_TOPOLOGY, Interface);
    events = (t->Path != NULL) ? (PPORTEVENTS)t->Path->PortEvents : NULL;
    switch (Request->Verb) {
    case PCEVENT_VERB_ADD:
        if (events == NULL || Request->EventEntry == NULL) {
            return STATUS_UNSUCCESSFUL;
        }
        events->lpVtbl->AddEventToEventList((PVOID)events, Request->EventEntry);
        return STATUS_SUCCESS;
    case PCEVENT_VERB_REMOVE:
        // PortCls removes the entry from its list itself.
        return STATUS_SUCCESS;
    case PCEVENT_VERB_SUPPORT:
        return (t->Path != NULL) ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
    default:
        return STATUS_INVALID_PARAMETER;
    }
}

// Built-in microphone pin: the jack it reports while yielding to a headset.
static PCPROPERTY_ITEM g_T2AudioMicProperties[] = {

    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION,
        PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT,
        T2AudioMicJackProperty
    },
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION2,
        PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT,
        T2AudioMicJackProperty
    }
};

static PCPROPERTY_ITEM g_T2AudioHeadsetMicProperties[] = {
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION,
        PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT,
        T2AudioMicJackProperty
    },
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION2,
        PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT,
        T2AudioMicJackProperty
    }
};

static PCEVENT_ITEM g_T2AudioMicJackEvents[] = {
    {
        &KSEVENTSETID_PinCapsChange,
        KSEVENT_PINCAPS_JACKINFOCHANGE,
        KSEVENT_TYPE_ENABLE | KSEVENT_TYPE_BASICSUPPORT,
        T2AudioMicJackEvent
    }
};

static PCAUTOMATION_TABLE g_T2AudioMicPinAutomation = {
    sizeof(PCPROPERTY_ITEM),
    SIZEOF_ARRAY(g_T2AudioMicProperties),
    g_T2AudioMicProperties,
    0, 0, NULL,
    sizeof(PCEVENT_ITEM),
    SIZEOF_ARRAY(g_T2AudioMicJackEvents),
    g_T2AudioMicJackEvents,
    0
};

static PCAUTOMATION_TABLE g_T2AudioHeadsetMicPinAutomation = {
    sizeof(PCPROPERTY_ITEM),
    SIZEOF_ARRAY(g_T2AudioHeadsetMicProperties),
    g_T2AudioHeadsetMicProperties,
    0, 0, NULL,
    sizeof(PCEVENT_ITEM),
    SIZEOF_ARRAY(g_T2AudioMicJackEvents),
    g_T2AudioMicJackEvents,
    0
};

static VOID
T2AudioMicSignalJack(_In_ PT2AUDIO_CAPTURE_PATH Path)
{
    PPORTEVENTS events = (PPORTEVENTS)Path->PortEvents;

    if (!Path->FiltersRegistered || events == NULL) {
        return;
    }
    events->lpVtbl->GenerateEventList((PVOID)events, (GUID *)&KSEVENTSETID_PinCapsChange,
                                      KSEVENT_PINCAPS_JACKINFOCHANGE,
                                      FALSE, (ULONG)-1,
                                      FALSE, (ULONG)-1);
}

// Called at PASSIVE_LEVEL when the polled headset or headphone jack changed
// (BceTransport.c, Topology.c): the headset pin and the built-in pin that
// yields to it.
VOID
T2AudioNotifyHeadsetMicJackChange(_In_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    if (Context == NULL || !Context->HeadsetMic.FiltersRegistered) {
        return;
    }
    T2AudioMicSignalJack(&Context->HeadsetMic);
    if (T2AudioBuiltInMicYields(Context)) {
        T2AudioMicSignalJack(&Context->Mic);
    }
    KdPrint(("T2Audio: mic jack change signalled to Windows (headset %s, built-in mics %s)\n",
             T2AudioHeadsetMicJackConnected(Context) ? "connected" : "unplugged",
             T2AudioBuiltInMicJackConnected(Context) ? "available" : "yield to headset"));
}

// PnP stop/removal (after the jack poll stopped): drop the IPortEvents
// references taken at Init.
VOID
T2AudioReleaseHeadsetMicEvents(_Inout_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    PPORTEVENTS events;

    if (Context == NULL) {
        return;
    }
    events = (PPORTEVENTS)InterlockedExchangePointer(&Context->HeadsetMic.PortEvents, NULL);
    if (events != NULL) {
        events->lpVtbl->Release((PVOID)events);
    }
    events = (PPORTEVENTS)InterlockedExchangePointer(&Context->Mic.PortEvents, NULL);
    if (events != NULL) {
        events->lpVtbl->Release((PVOID)events);
    }
}

// ---------------------------------------------------------------------------
// Topology filters
// ---------------------------------------------------------------------------

static PCPIN_DESCRIPTOR g_T2AudioMicTopoPins[] = {
    // Pin 0: the built-in microphone array (physical source).
    {
        1, 1, 0, NULL,
        {
            0, NULL, 0, NULL,
            SIZEOF_ARRAY(g_T2AudioMicBridgeRanges), g_T2AudioMicBridgeRanges,
            KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_MICROPHONE, NULL, 0
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

static PCPIN_DESCRIPTOR g_T2AudioHeadsetMicTopoPins[] = {
    // Pin 0: the headset microphone on the 3.5 mm jack (physical source).
    {
        1, 1, 0, NULL,
        {
            0, NULL, 0, NULL,
            SIZEOF_ARRAY(g_T2AudioMicBridgeRanges), g_T2AudioMicBridgeRanges,
            KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_HEADSET_MICROPHONE, NULL, 0
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
    0, &g_T2AudioMicPinAutomation,       // filter-level KSP_PIN properties and jack event
    sizeof(PCPIN_DESCRIPTOR), SIZEOF_ARRAY(g_T2AudioMicTopoPins), g_T2AudioMicTopoPins,
    sizeof(PCNODE_DESCRIPTOR), 0, NULL,
    SIZEOF_ARRAY(g_T2AudioMicTopoConnections), g_T2AudioMicTopoConnections,
    0, NULL
};

static PCFILTER_DESCRIPTOR g_T2AudioHeadsetMicTopoDescriptor = {
    0, &g_T2AudioHeadsetMicPinAutomation,
    sizeof(PCPIN_DESCRIPTOR), SIZEOF_ARRAY(g_T2AudioHeadsetMicTopoPins), g_T2AudioHeadsetMicTopoPins,
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
    PT2AUDIO_MIC_TOPOLOGY t = CONTAINING_RECORD((PMINIPORTTOPOLOGY)Miniport, T2AUDIO_MIC_TOPOLOGY, Interface);

    if (Description == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Description = (t->Path != NULL && t->Path->Headset) ? &g_T2AudioHeadsetMicTopoDescriptor
                                                         : &g_T2AudioMicTopoDescriptor;
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
    PT2AUDIO_MIC_TOPOLOGY t = CONTAINING_RECORD(Miniport, T2AUDIO_MIC_TOPOLOGY, Interface);

    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    // Keep the port's IPortEvents to signal jack changes (the headset jack,
    // and the built-in pin that yields to it; T2AudioNotifyHeadsetMicJackChange).
    // Released in the PnP stop path.
    if (t->Path != NULL && Port != NULL) {
        PVOID events = NULL;

        if (NT_SUCCESS(Port->lpVtbl->QueryInterface((PVOID)Port, &IID_IPortEvents, &events)) &&
            events != NULL &&
            InterlockedCompareExchangePointer(&t->Path->PortEvents, events, NULL) != NULL) {
            ((PPORTEVENTS)events)->lpVtbl->Release(events);
        }
    }
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

// Register the topology + wave pair of a capture path ("TopologyMic" +
// "WaveMic", or for the headset AppleAudio's "TopologyMicIn" + "WaveMicIn")
// and wire topology pin 1 -> wave pin 1.
NTSTATUS
T2AudioRegisterMicFilters(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList,
    _In_ PT2AUDIO_DEVICE_CONTEXT Context,
    _In_ PT2AUDIO_CAPTURE_PATH Path)
{
    PPORT base = NULL;
    PPORTTOPOLOGY topoPort = NULL;
    PPORTWAVERT wavePort = NULL;
    PT2AUDIO_MIC_TOPOLOGY topo = NULL;
    PT2AUDIO_MIC_MINIPORT wave = NULL;
    PWSTR topoName;
    PWSTR waveName;
    NTSTATUS status;

    PAGED_CODE();

    if (Path == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    // Parameters\ExtraFormats: the capture pins also offer 16 and 24 bits
    // (shared mode keeps the 32-bit format the intersection proposes).
    g_T2AudioMicRange.MinimumBitsPerSample = (Context != NULL && Context->ExtraFormats) ? 16u : 32u;
    topoName = Path->Headset ? L"TopologyMicIn" : L"TopologyMic";
    waveName = Path->Headset ? L"WaveMicIn" : L"WaveMic";
    topo = (PT2AUDIO_MIC_TOPOLOGY)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*topo), T2AUDIO_MIC_POOL_TAG);
    wave = (PT2AUDIO_MIC_MINIPORT)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*wave), T2AUDIO_MIC_POOL_TAG);
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
    topo->DeviceContext = Context;
    topo->Path = Path;
    RtlZeroMemory(wave, sizeof(*wave));
    wave->Interface.lpVtbl = &g_T2AudioMicMiniportVtbl;
    wave->ReferenceCount = 1;
    wave->DeviceContext = Context;
    wave->Path = Path;

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
        status = PcRegisterSubdevice(DeviceObject, topoName, (PUNKNOWN)topoPort);
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
        status = PcRegisterSubdevice(DeviceObject, waveName, (PUNKNOWN)wavePort);
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
