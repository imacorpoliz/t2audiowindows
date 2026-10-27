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
    ULONG64 BceSpeakerDeviceId;
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
    ULONG TweeterMode;
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

VOID T2AudioLogFormat(
    _In_z_ PCSTR Tag,
    _In_opt_ PKSDATAFORMAT DataFormat);

NTSTATUS T2AudioCreateStream(
    _In_ PT2AUDIO_DEVICE_CONTEXT DeviceContext,
    _In_ PPORTWAVERTSTREAM PortStream,
    _In_ PKSDATAFORMAT DataFormat,
    _Out_ PMINIPORTWAVERTSTREAM *Stream);

NTSTATUS T2AudioCreateMiniport(
    _In_ PT2AUDIO_DEVICE_CONTEXT DeviceContext,
    _Out_ PMINIPORTWAVERT *Miniport);

NTSTATUS T2AudioCreateTopology(
    _In_ PT2AUDIO_DEVICE_CONTEXT DeviceContext,
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
