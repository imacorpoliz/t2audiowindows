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
    PVOID Bar2Mapped;
    SIZE_T Bar2Size;
    T2AUDIO_BUFFER_STRUCT *BufferStruct;
    SIZE_T SpeakerBufferOffset;
    SIZE_T SpeakerBufferSize;
    ULONG64 SpeakerDeviceId;
    PMDL SpeakerBufferMdl;
    BOOLEAN HardwareReady;
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
    _Out_ SIZE_T *BufferOffset,
    _Out_ SIZE_T *BufferSize);

NTSTATUS T2AudioValidateSixChannelFormat(
    _In_ PKSDATAFORMAT DataFormat);

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
NTSTATUS T2AudioFindSpeakerDeviceId(
    _In_ PT2AUDIO_DEVICE_CONTEXT Context,
    _Out_ PULONG64 DeviceId);

#ifdef __cplusplus
}
#endif

#endif
