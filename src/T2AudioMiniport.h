#ifndef T2_AUDIO_MINIPORT_H
#define T2_AUDIO_MINIPORT_H

#include <ntddk.h>
#include <portcls.h>
#include <ksmedia.h>

#define T2AUDIO_SIG ((ULONG)0x19870423)
#define T2AUDIO_DEVICE_STRIDE ((SIZE_T)0xBDEC)
#define T2AUDIO_MAX_DEVICES 20u
#define T2AUDIO_MAX_STREAMS 5u
#define T2AUDIO_MAX_BUFFERS 100u
#define T2AUDIO_GPR_OFFSET ((SIZE_T)0xC000)

// Pin indices. The WaveRT filter exposes a render streaming pin (0) and a
// bridge pin (1). The topology filter exposes a bridge input pin (0) and a
// speaker output pin (1). The bridge pins are wired together with
// PcRegisterPhysicalConnection.
#define T2AUDIO_WAVE_PIN_RENDER_SINK 0u
#define T2AUDIO_WAVE_PIN_BRIDGE      1u
#define T2AUDIO_TOPO_PIN_BRIDGE      0u
#define T2AUDIO_TOPO_PIN_SPEAKER     1u
// The headphone filters (Parameters\HeadphoneOutput=1) use the same pin
// layout; topology pin 1 is the headphone jack there.
#define T2AUDIO_TOPO_PIN_HEADPHONE   1u

// Headphone ring ("Codec Output"): 2 channels, S24_LE in 32-bit slots.
#define T2AUDIO_HP_RING_CHANNELS        2u
#define T2AUDIO_HP_RING_BYTES_PER_FRAME 8u

#pragma pack(push, 4)
typedef struct _T2AUDIO_BUFFER_ENTRY {
    SIZE_T Address;
    SIZE_T Size;
    SIZE_T Pad[4];
} T2AUDIO_BUFFER_ENTRY;

typedef struct _T2AUDIO_STREAM_METADATA {
    UCHAR NumBuffers;
    T2AUDIO_BUFFER_ENTRY Buffers[T2AUDIO_MAX_BUFFERS];
    UCHAR Filler[32];
} T2AUDIO_STREAM_METADATA;

typedef struct _T2AUDIO_DEVICE_METADATA {
    CHAR Name[128];
    UCHAR NumInputStreams;
    UCHAR NumOutputStreams;
    T2AUDIO_STREAM_METADATA InputStreams[T2AUDIO_MAX_STREAMS];
    T2AUDIO_STREAM_METADATA OutputStreams[T2AUDIO_MAX_STREAMS];
    UCHAR Filler[128];
} T2AUDIO_DEVICE_METADATA;

typedef struct _T2AUDIO_BUFFER_STRUCT {
    ULONG Version;
    ULONG Signature;
    ULONG Flags;
    UCHAR NumDevices;
    UCHAR Reserved[3];
    T2AUDIO_DEVICE_METADATA Devices[T2AUDIO_MAX_DEVICES];
} T2AUDIO_BUFFER_STRUCT;
#pragma pack(pop)

C_ASSERT(sizeof(T2AUDIO_BUFFER_ENTRY) == 0x30);
C_ASSERT(sizeof(T2AUDIO_STREAM_METADATA) == 0x12E4);
C_ASSERT(sizeof(T2AUDIO_DEVICE_METADATA) == T2AUDIO_DEVICE_STRIDE);
C_ASSERT(FIELD_OFFSET(T2AUDIO_BUFFER_STRUCT, Devices) == 0x10);

typedef struct _T2AUDIO_DEVICE_CONTEXT {
    PDEVICE_OBJECT PhysicalDeviceObject;
    PDEVICE_OBJECT FunctionalDeviceObject;
    PDEVICE_OBJECT LowerDeviceObject;
    PDEVICE_OBJECT DeviceObject;
    PVOID Bar1Mapped;
    PHYSICAL_ADDRESS Bar1Physical;
    SIZE_T Bar1Size;
    BOOLEAN Bar1WriteCombined;
    PVOID Bar2Mapped;
    SIZE_T Bar2Size;
    T2AUDIO_BUFFER_STRUCT *BufferStruct;
    SIZE_T SpeakerBufferOffset;
    SIZE_T SpeakerBufferSize;
    ULONG64 SpeakerDeviceId;
    PMDL SpeakerBufferMdl;
    // Guards HardwareReady and the BAR mappings against teardown while a
    // writer (copy path, STOP zeroing) is touching BAR1. Any code that writes
    // device memory checks HardwareReady and writes under this lock;
    // T2AudioUnmapResources clears both under it before unmapping. A zeroed
    // KSPIN_LOCK is a valid released lock, so a fresh extension is usable.
    KSPIN_LOCK HardwareLock;
    BOOLEAN HardwareReady;
    // TRUE while the adapter is in D1..D3 (set by PowerChangeState under
    // HardwareLock): the copy DPC does not write BAR0 then.
    BOOLEAN PoweredDown;
    // 1 while START_IO is in effect. Whoever clears it with
    // InterlockedExchange (the stream leaving RUN, or the power callbacks
    // before D3) sends the one matching STOP_IO.
    volatile LONG HardwareIoActive;
    ULONG64 BceSpeakerDeviceId;
    // Headphone jack (first step, read-only): the BCE device "Codec Output"
    // and its 'jack' state at probe time (1 = plugged). Logged only; the
    // headphone output itself is not driven yet (docs/HEADPHONES.md).
    ULONG64 BceHeadphoneDeviceId;
    volatile LONG HeadphoneJackPlugged;
    // Jack events (Parameters\JackEvents=1): a PROPERTY_LISTENER on
    // 'outp'/'jack' of the headphone device; each PROPERTY_CHANGED queues a
    // PASSIVE work item that re-reads the jack (JackWorkPending coalesces).
    // HeadphoneAutoMute (needs JackEvents) silences the speakers while the
    // jack reads plugged in.
    ULONG JackEvents;
    ULONG HeadphoneAutoMute;
    BOOLEAN JackListenerRegistered;
    volatile LONG JackWorkPending;
    volatile LONG JackEventCount;
    // Jack polling (with JackEvents): bridgeOS sent no PROPERTY_CHANGED on
    // plug/unplug under Windows (EXCERPT_20261028_step4.md), so a 1 s timer
    // also queues the re-read. JackPollStop/JackWorkRunning let the PnP stop
    // path cancel the timer and wait for a queued re-read to finish.
    KTIMER JackPollTimer;
    KDPC JackPollDpc;
    BOOLEAN JackPollStarted;
    volatile LONG JackPollStop;
    volatile LONG JackWorkRunning;
    volatile LONG JackPollCount;
    // JackSeenPlugged: a jack read once returned "plugged", so the read is
    // known to follow the jack; only then does the headphone endpoint report
    // "unplugged" to Windows (otherwise it would stay hidden for good).
    // JackProbeAll (Parameters\JackProbeAll=1, experiment): each poll also
    // reads the jack at other addresses (T2AudioJackProbeVariants).
    BOOLEAN JackSeenPlugged;
    ULONG JackProbeAll;
#define T2AUDIO_JACK_PROBE_VARIANTS 8
    LONG JackProbeLast[T2AUDIO_JACK_PROBE_VARIANTS];
    ULONG64 BceHeadsetMicDeviceId;
    // Headphone output (Parameters\HeadphoneOutput=1, default 0; ROADMAP 4.4).
    // A second WaveRT + topology filter pair renders to the "Codec Output"
    // device: its ring is located in BufferStruct at MapResources, its
    // stream format read at the BCE probe, and its START_IO/STOP_IO use the
    // headphone device id. Plain path (no speaker DSP), with its own safety
    // cut HeadphoneAttenuation (6 dB steps, default 2 = -12 dB). The clock
    // anchor mirrors the speaker one, from the headphone device's
    // UPDATE_TIMESTAMP. HeadphoneIoActive works like HardwareIoActive.
    // HeadphonePortEvents (IPortEvents of the headphone topology port, held
    // with a reference) signals jack changes to Windows.
    ULONG HeadphoneOutput;
    ULONG HeadphoneAttenuation;
    BOOLEAN HeadphoneFiltersRegistered;
    SIZE_T HeadphoneBufferOffset;
    SIZE_T HeadphoneBufferSize;
    ULONG64 HeadphoneDeviceId;
    ULONG64 HeadphoneStreamId;
    BOOLEAN HeadphoneFormatValid;
    ULONG HeadphoneSampleShift;
    ULONG HeadphoneLatencyFrames;
    volatile LONG HeadphoneIoActive;
    ULONG HpClockGeneration;
    ULONG64 HpClockAnchorFrames;
    ULONG64 HpClockAnchorQpc;
    ULONG64 HpClockAnchorNs;
    PVOID HeadphonePortEvents;
    // Built-in microphones (Parameters\MicInput=1, default 0; ROADMAP 5.2-5.4,
    // MicCapture.c). The driver allocates a contiguous host ring
    // (T2AUDIO_MIC_RING_BYTES), writes its physical address into BufferStruct
    // input_streams[0] of "Digital Mic" and sends SET_INPUT_STREAM_ADDRESS_
    // RANGES (msg 18), as kaiT2en and AppleAudio do; a capture WaveRT +
    // topology pair exposes a mono microphone array. MicDsp (default 1) runs
    // kaiT2en's 16_1 mic chain (T2AudioMicLogic.h); 0 passes mic 0 raw.
    // The ring is freed only after STOP_IO and after the BufferStruct entry
    // was cleared (MicUnregister in the PnP stop path).
    ULONG MicInput;
    ULONG MicDsp;
    // Parameters\ExtraFormats=1 (default 0; ROADMAP 2.8): the render pins
    // also accept 16- and 24-bit PCM (exclusive-mode apps). Shared mode keeps
    // using 32-bit, the format the intersection proposes.
    ULONG ExtraFormats;
    BOOLEAN MicFiltersRegistered;
    BOOLEAN MicRegistered;
    BOOLEAN MicFormatValid;
    ULONG64 BceMicDeviceId;
    ULONG64 MicDeviceId;
    ULONG64 MicStreamId;
    ULONG MicLatencyFrames;
    PVOID MicBufferVa;
    PHYSICAL_ADDRESS MicBufferPa;
    ULONG MicBufferSize;
    volatile LONG MicIoActive;
    ULONG MicClockGeneration;
    ULONG64 MicClockAnchorFrames;
    ULONG64 MicClockAnchorQpc;
    ULONG64 MicClockAnchorNs;
    PVOID MicRunningStream;
    // Resume after sleep (Parameters\ResumeAfterSleep=1, default 0; ROADMAP
    // 2.1): streams register here while in RUN; on D0 a PASSIVE work item
    // restarts the I/O of a stream the power-down had stopped. StreamMutex
    // serialises that with SetState. Index 0 = speaker, 1 = headphones.
    ULONG ResumeAfterSleep;
    KMUTEX StreamMutex;
    PVOID RunningStream[2];
    volatile LONG ResumeWorkPending;
    // Parameters\ExtendedProbe=1: log every BCE device's streams, formats,
    // latencies and jacks, and the whole BufferStruct table (read-only), as
    // groundwork for the headphone and microphone endpoints (docs/ROADMAP.md).
    ULONG ExtendedProbe;
    // Device count of the BufferStruct table as validated by MapResources.
    ULONG BufferStructDevices;
    BOOLEAN BceProbed;
    // Speaker output stream as reported by the T2 (BCE probe, stage 3+).
    // SpeakerFormatValid is set only when its 'pft ' physical format was read
    // and is a layout the copy path can produce; SpeakerSampleShift is the
    // right shift applied to each 32-bit host sample (8 for the S24_LE ring
    // the T2 speaker uses). The copy DPC never writes BAR1 without it.
    ULONG64 SpeakerStreamId;
    BOOLEAN SpeakerFormatValid;
    ULONG SpeakerSampleShift;
    ULONG SpeakerLatencyFrames;
    ULONG SpeakerSafetyOffsetFrames;
    // Messages bridgeOS pushed to us through AppleUSBVHCI (IOCTL 0x222020):
    // counters and the latest UPDATE_TIMESTAMP for the speaker. Written by
    // T2AudioHandleVhciMessage only.
    volatile LONG VhciMessageCount;
    volatile LONG VhciTimestampCount;
    ULONG64 LastT2Timestamp;
    ULONG64 LastT2SampleFrames;
    ULONG64 LastT2UpdateSeed;
    // T2 clock anchor for the copy path: the T2 read position ClockAnchorFrames
    // (sampleTime, advanced to the moment of reception) at QPC ClockAnchorQpc.
    // ClockGeneration increments on every new anchor, so the copy DPC knows
    // when to re-check its lead. Guarded by ClockLock (taken inside
    // StreamLock, never the other way round).
    KSPIN_LOCK ClockLock;
    ULONG ClockGeneration;
    ULONG64 ClockAnchorFrames;
    ULONG64 ClockAnchorQpc;
    // Host time (QPC ns) the anchor stands for: hostTime if plausible, else
    // the reception time. Streams drop anchors older than their START_IO.
    ULONG64 ClockAnchorNs;
    // Speaker path options. TweeterMode (Parameters\TweeterMode, default 1 =
    // high-pass, 0 = mute) applies to the stereo format. RawSpeakerChannels
    // (TestSession\RawSpeakerChannels=1) offers the 6 raw ring channels to
    // Windows instead of stereo, for per-speaker diagnostics only: the
    // client then drives the tweeters directly, with no protection filter.
    // OutputAttenuation (Parameters\OutputAttenuation, 6 dB steps, default
    // T2AUDIO_ATTENUATION_DEFAULT, clamped to T2AUDIO_ATTENUATION_MAX) is a
    // safety cut applied to every sample before it reaches the ring.
    // ClockRateTracking (Parameters\ClockRateTracking, default 0 = off): run
    // the source cursor at the T2's rate instead of re-aiming on drift only
    // (T2AudioClockRatePpb; tests/T2AudioPipelineTest.c). Not hardware-tested.
    ULONG TweeterMode;
    ULONG OutputAttenuation;
    BOOLEAN ClockRateTracking;
    // SpeakerDsp (Parameters\SpeakerDsp, default 3 since 2026-10-28; 0 = plain
    // path): 1 runs the
    // stereo path through T2AudioDsp.h (KAIT2EN 16_1 linear chain: sub-bass
    // high-pass, crosstalk cancellation, EQ, per-driver crossover, limiter)
    // instead of woofers full-range + tweeters high-passed; 2 adds the
    // virtual bass in front; 3 also the multiband compressor; 4 also the
    // woofer buzz compressor (110-145 Hz). Values above
    // T2DSP_LEVEL_MAX act as the maximum. Not hardware-tested.
    ULONG SpeakerDsp;
    // DspGainDb (Parameters\DspGainDb, default 6, 0..12): make-up gain after
    // the speaker DSP, in front of its -1 dBFS limiter (T2AudioDsp.h).
    ULONG DspGainDb;
    // TweeterTrimDb (Parameters\TweeterTrimDb, default 0, 0..12): extra gain
    // on the tweeters in the DSP path only (brightness / "air").
    ULONG TweeterTrimDb;
    // WooferLimitDb (Parameters\WooferLimitDb, default 0, 0..12): lowers the
    // woofers' limiter ceiling below -1 dBFS in the DSP path (overload /
    // rattle at full volume with heavy bass).
    ULONG WooferLimitDb;
    // DspAutoGain (Parameters\DspAutoGain, default 0, 0/1): slow autogain
    // toward -12 dBFS RMS (+6/-12 dB) in front of the DSP chain.
    ULONG DspAutoGain;
    // Tone controls in the DSP path (Parameters\BassDb, TrebleDb, ClarityDb:
    // signed dB stored as DWORD, -12..+12, default 0) and the compressor
    // detector (Parameters\CompStyle: 0 = peak/hard knee, the approved sound;
    // 1 = lsp defaults, RMS of mid + soft knee). See T2DspSetTuning.
    LONG BassDb;
    LONG TrebleDb;
    LONG ClarityDb;
    ULONG CompStyle;
    BOOLEAN RawSpeakerChannels;
    BOOLEAN ForceSystemBuffer;
    ULONG MdlVariant;
    // Test-session gates. Both are read from the VOLATILE registry key
    // HKLM\SYSTEM\CurrentControlSet\Services\T2AudioMiniport\TestSession, which
    // is wiped on every reboot (including after a bugcheck). The driver refuses
    // to start at all (no BAR mapping, no BCE, no audio) unless TestModeEnabled
    // is set, and the copy DPC never writes device memory unless
    // MmioCopyEnabled is set. On a normal boot neither is present.
    BOOLEAN TestModeEnabled;
    // Persistent mode (Parameters\EnableProduction=1, no TestSession): the
    // driver runs the stage-5 + BCE I/O configuration on every boot under the
    // crash guard (T2AudioCrashGuard.h). CrashGuardArmed mirrors the registry
    // flag; ShutdownRegistered tracks IoRegisterShutdownNotification.
    BOOLEAN ProductionMode;
    BOOLEAN CrashGuardArmed;
    BOOLEAN ShutdownRegistered;
    BOOLEAN MmioCopyEnabled;
    // Whether SetState may actually issue START_IO/STOP_IO (session
    // EnableBceIo=1). Off by default, so stage 4 can wire the speaker and hold
    // the BCE transport open WITHOUT sending any command - that isolates the
    // send itself from the stream/transport setup. The send is also refused
    // above PASSIVE_LEVEL regardless of this flag (see T2AudioSendBceMessage).
    BOOLEAN BceIoEnabled;
    // Diagnostic stage selected by the test session (DiagStage). Higher stages
    // include lower ones:
    //   1 = RAM only       - no BAR mapping, no BCE, system-memory buffer only
    //   2 = BAR metadata   - map BARs and locate the speaker buffer
    //   3 = BCE discovery  - open the transport and resolve the speaker
    //   4 = hardware I/O   - wire the speaker (and hold the transport open);
    //                        START_IO/STOP_IO only if EnableBceIo=1
    //   5 = MMIO copy      - the copy DPC writes PCM into BAR1
    // It is read only when TestModeEnabled is set, so a normal boot never maps
    // device memory regardless of this value.
    ULONG DiagStage;
} T2AUDIO_DEVICE_CONTEXT, *PT2AUDIO_DEVICE_CONTEXT;

typedef struct _T2AUDIO_STREAM_CONTEXT {
    T2AUDIO_DEVICE_CONTEXT *DeviceContext;
    ULONG SampleRate;
    ULONG Channels;
    ULONG BitsPerSample;
    ULONG BytesPerFrame;
    ULONG64 AnchorQpc;
    ULONG64 AnchorFrames;
    BOOLEAN Started;
} T2AUDIO_STREAM_CONTEXT, *PT2AUDIO_STREAM_CONTEXT;

typedef struct _T2AUDIO_MINIPORT T2AUDIO_MINIPORT, *PT2AUDIO_MINIPORT;
typedef struct _T2AUDIO_WAVERT_STREAM T2AUDIO_WAVERT_STREAM, *PT2AUDIO_WAVERT_STREAM;

#ifdef __cplusplus
extern "C" {
#endif

DRIVER_INITIALIZE DriverEntry;
DRIVER_ADD_DEVICE T2AudioAddDevice;

static __forceinline PT2AUDIO_DEVICE_CONTEXT
T2AudioGetContext(_In_ PDEVICE_OBJECT DeviceObject)
{
    return (PT2AUDIO_DEVICE_CONTEXT)((PUCHAR)DeviceObject->DeviceExtension + PORT_CLASS_DEVICE_EXTENSION_SIZE);
}

NTSTATUS T2AudioStartDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList);

NTSTATUS T2AudioMapResources(
    _Inout_ PT2AUDIO_DEVICE_CONTEXT Context,
    _In_ PRESOURCELIST ResourceList);

VOID T2AudioUnmapResources(_Inout_ PT2AUDIO_DEVICE_CONTEXT Context);
VOID T2AudioCrashGuardSetArmed(_In_ PT2AUDIO_DEVICE_CONTEXT Context, _In_ BOOLEAN Armed);

NTSTATUS T2AudioFindOutputBuffer(
    _In_ const T2AUDIO_BUFFER_STRUCT *BufferStruct,
    _In_ SIZE_T Bar1Size,
    _In_ ULONG NumDevices,
    _In_z_ PCSTR Name,
    _Out_ SIZE_T *BufferOffset,
    _Out_ SIZE_T *BufferSize);

NTSTATUS T2AudioFindSpeakerBuffer(
    _In_ const T2AUDIO_BUFFER_STRUCT *BufferStruct,
    _In_ SIZE_T Bar1Size,
    _In_ ULONG NumDevices,
    _Out_ SIZE_T *BufferOffset,
    _Out_ SIZE_T *BufferSize);

NTSTATUS T2AudioValidateRenderFormat(
    _In_ PKSDATAFORMAT DataFormat,
    _In_ BOOLEAN AllowRawChannels,
    _Out_opt_ PULONG Channels);

// Like T2AudioValidateRenderFormat; with AllowExtraBits also 16- and 24-bit
// (packed) containers. *BitsPerSample is 16, 24 or 32.
NTSTATUS T2AudioValidateRenderFormatEx(
    _In_ PKSDATAFORMAT DataFormat,
    _In_ BOOLEAN AllowRawChannels,
    _In_ BOOLEAN AllowExtraBits,
    _Out_opt_ PULONG Channels,
    _Out_opt_ PULONG BitsPerSample);

VOID T2AudioLogFormat(
    _In_z_ PCSTR Tag,
    _In_opt_ PKSDATAFORMAT DataFormat);

NTSTATUS T2AudioCreateStream(
    _In_ PT2AUDIO_DEVICE_CONTEXT DeviceContext,
    _In_ BOOLEAN Headphone,
    _In_ PPORTWAVERTSTREAM PortStream,
    _In_ PKSDATAFORMAT DataFormat,
    _Out_ PMINIPORTWAVERTSTREAM *Stream);

NTSTATUS T2AudioCreateMiniport(
    _In_ PT2AUDIO_DEVICE_CONTEXT DeviceContext,
    _In_ BOOLEAN Headphone,
    _Out_ PMINIPORTWAVERT *Miniport);

NTSTATUS T2AudioCreateTopology(
    _In_ PT2AUDIO_DEVICE_CONTEXT DeviceContext,
    _In_ BOOLEAN Headphone,
    _Out_ PMINIPORTTOPOLOGY *Topology);

NTSTATUS T2AudioCreateSpeakerMdl(
    _Inout_ PT2AUDIO_DEVICE_CONTEXT Context,
    _Out_ PMDL *Mdl,
    _Out_ PULONG OffsetFromFirstPage,
    _Out_ PULONG ActualSize);

VOID T2AudioFreeSpeakerMdl(_Inout_ PT2AUDIO_DEVICE_CONTEXT Context);

NTSTATUS T2AudioBuildIoCommand(
    _In_ ULONG Message,
    _In_ ULONG64 DeviceId,
    _Out_writes_bytes_(BufferLength) PUCHAR Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ PSIZE_T MessageLength);

NTSTATUS T2AudioStartIo(_In_ PT2AUDIO_DEVICE_CONTEXT Context);
NTSTATUS T2AudioStopIo(_In_ PT2AUDIO_DEVICE_CONTEXT Context);
NTSTATUS T2AudioStartIoDevice(_In_ PT2AUDIO_DEVICE_CONTEXT Context, _In_ ULONG64 DeviceId);
NTSTATUS T2AudioStopIoDevice(_In_ PT2AUDIO_DEVICE_CONTEXT Context, _In_ ULONG64 DeviceId);
VOID T2AudioNotifyJackChange(_In_ PT2AUDIO_DEVICE_CONTEXT Context);
VOID T2AudioReleaseHeadphoneEvents(_Inout_ PT2AUDIO_DEVICE_CONTEXT Context);
VOID T2AudioStreamResumeAfterPower(_In_ PVOID Stream);

// Microphones (MicCapture.c).
#define T2AUDIO_MIC_WAVE_PIN_CAPTURE  0u
#define T2AUDIO_MIC_WAVE_PIN_BRIDGE   1u
#define T2AUDIO_MIC_TOPO_PIN_MIC      0u
#define T2AUDIO_MIC_TOPO_PIN_BRIDGE   1u
BOOLEAN T2AudioMicPresentInBufferStruct(_In_ PT2AUDIO_DEVICE_CONTEXT Context);
NTSTATUS T2AudioMicRegister(_Inout_ PT2AUDIO_DEVICE_CONTEXT Context);
NTSTATUS T2AudioMicReRegister(_Inout_ PT2AUDIO_DEVICE_CONTEXT Context);
VOID T2AudioMicUnregister(_Inout_ PT2AUDIO_DEVICE_CONTEXT Context);
NTSTATUS T2AudioRegisterMicFilters(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList,
    _In_ PT2AUDIO_DEVICE_CONTEXT Context);
VOID T2AudioMicStreamResumeAfterPower(_In_ PVOID Stream);
NTSTATUS T2AudioRegisterPowerManagement(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PT2AUDIO_DEVICE_CONTEXT Context);

VOID T2AudioInitializeBceTransport(VOID);
NTSTATUS T2AudioOpenBceTransport(VOID);
VOID T2AudioCloseBceTransport(VOID);
NTSTATUS T2AudioSendBceMessage(
    _In_reads_bytes_(MessageSize) const UCHAR *Message,
    _In_ ULONG MessageSize,
    _Out_writes_bytes_opt_(ReplyBufferSize) UCHAR *ReplyBuffer,
    _In_ ULONG ReplyBufferSize,
    _Out_opt_ PULONG ReplySize);
NTSTATUS T2AudioGetDeviceList(
    _Out_writes_(MaxDevices) ULONG64 *DeviceList,
    _In_ ULONG MaxDevices,
    _Out_ PULONG DeviceCount);
NTSTATUS T2AudioSetRemoteAccess(_In_ BOOLEAN Enable);
NTSTATUS T2AudioFindSpeakerDeviceId(
    _In_ PT2AUDIO_DEVICE_CONTEXT Context,
    _Out_ PULONG64 DeviceId);

NTSTATUS T2AudioProbeBceDevices(_Inout_ PT2AUDIO_DEVICE_CONTEXT Context);
VOID T2AudioStopJackPoll(_Inout_ PT2AUDIO_DEVICE_CONTEXT Context);
VOID T2AudioLogBufferStruct(_In_ PT2AUDIO_DEVICE_CONTEXT Context);

// IOCTL AppleUSBVHCI uses to push bridgeOS messages to the audio device:
// CTL_CODE(FILE_DEVICE_UNKNOWN, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS).
#define IOCTL_APPLE_VHCI_AUDIO_MESSAGE 0x222020u

VOID T2AudioHandleVhciMessage(
    _Inout_opt_ PT2AUDIO_DEVICE_CONTEXT Context,
    _In_reads_bytes_opt_(MessageSize) const UCHAR *Message,
    _In_ ULONG MessageSize);

#ifdef __cplusplus
}
#endif

#endif
