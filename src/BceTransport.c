#include "T2AudioMiniport.h"
#include "BceProtocolLogic.h"

// T2 BCE Protocol constants (from kaiT2en modules/t2bce_audio/protocol.h)
#define T2AUDIO_BCE_TAG "Audt"
#define T2AUDIO_MSG_TYPE_COMMAND 1
#define T2AUDIO_MSG_TYPE_RESPONSE 2

#define T2AUDIO_MSG_GET_PROPERTY 7
#define T2AUDIO_MSG_GET_PROPERTY_RESPONSE 8
#define T2AUDIO_MSG_GET_DEVICE_LIST 101
#define T2AUDIO_MSG_GET_DEVICE_LIST_RESPONSE 102

// Property scope/selector values ('glob', 'uid ').
#define T2AUDIO_PROP_SCOPE_GLOBAL 0x676c6f62u
#define T2AUDIO_PROP_UID          0x75696420u

// GET_PROPERTY request payload after the base: obj(8) element(4) scope(4)
// selector(4) qualifier_size(8).
#define T2AUDIO_PROP_REQUEST_PAYLOAD_SIZE 28u

// Maximum UID length accepted, matching kaiT2en T2AUDIO_DEVICE_MAX_UID_LEN.
#define T2AUDIO_MAX_UID_LENGTH 128u

// AppleUSBVHCI device name (from user-mode testing)
#define APPLE_USBVHCI_DEVICE_NAME L"\\Device\\AppleUSBVHCI"

// IOCTL code (from user-mode testing: 0x222018). CTL_CODE(FILE_DEVICE_UNKNOWN,
// 0x0806, METHOD_BUFFERED, FILE_ANY_ACCESS) == 0x222018.
#define IOCTL_APPLE_BCE_SEND_MESSAGE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x0806, METHOD_BUFFERED, FILE_ANY_ACCESS)
C_ASSERT(IOCTL_APPLE_BCE_SEND_MESSAGE == 0x222018);

#pragma pack(push, 1)
typedef struct _T2AUDIO_MSG_HEADER {
    CHAR Tag[4];
    UCHAR Type;
    ULONG64 DeviceId;
} T2AUDIO_MSG_HEADER;

typedef struct _T2AUDIO_MSG_BASE {
    ULONG Message;
    ULONG Status;
} T2AUDIO_MSG_BASE;
#pragma pack(pop)

C_ASSERT(sizeof(T2AUDIO_MSG_HEADER) == T2AUDIO_BCE_HEADER_SIZE);
C_ASSERT(sizeof(T2AUDIO_MSG_BASE) == T2AUDIO_BCE_BASE_SIZE);

typedef struct _BCE_TRANSPORT_CONTEXT {
    PDEVICE_OBJECT DeviceObject;
    PFILE_OBJECT FileObject;
} BCE_TRANSPORT_CONTEXT, *PBCE_TRANSPORT_CONTEXT;

static BCE_TRANSPORT_CONTEXT g_BceTransport = { NULL, NULL };

NTSTATUS
T2AudioOpenBceTransport(VOID)
{
    UNICODE_STRING deviceName;
    NTSTATUS status;

    PAGED_CODE();

    if (g_BceTransport.DeviceObject != NULL) {
        return STATUS_SUCCESS;
    }

    RtlInitUnicodeString(&deviceName, APPLE_USBVHCI_DEVICE_NAME);
    status = IoGetDeviceObjectPointer(
        &deviceName,
        FILE_ALL_ACCESS,
        &g_BceTransport.FileObject,
        &g_BceTransport.DeviceObject);

    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: IoGetDeviceObjectPointer failed: 0x%08X\n", status));
        return status;
    }

    KdPrint(("T2Audio: BCE transport opened\n"));
    return STATUS_SUCCESS;
}

VOID
T2AudioCloseBceTransport(VOID)
{
    PAGED_CODE();

    if (g_BceTransport.FileObject != NULL) {
        ObDereferenceObject(g_BceTransport.FileObject);
        g_BceTransport.FileObject = NULL;
        g_BceTransport.DeviceObject = NULL;
        KdPrint(("T2Audio: BCE transport closed\n"));
    }
}

NTSTATUS
T2AudioSendBceMessage(
    _In_reads_bytes_(MessageSize) const UCHAR *Message,
    _In_ ULONG MessageSize,
    _Out_writes_bytes_opt_(ReplyBufferSize) UCHAR *ReplyBuffer,
    _In_ ULONG ReplyBufferSize,
    _Out_opt_ PULONG ReplySize)
{
    KEVENT event;
    IO_STATUS_BLOCK iosb;
    PIRP irp;
    NTSTATUS status;

    PAGED_CODE();

    if (g_BceTransport.DeviceObject == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    irp = IoBuildDeviceIoControlRequest(
        IOCTL_APPLE_BCE_SEND_MESSAGE,
        g_BceTransport.DeviceObject,
        (PVOID)Message,
        MessageSize,
        ReplyBuffer,
        ReplyBufferSize,
        FALSE,
        &event,
        &iosb);

    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoCallDriver(g_BceTransport.DeviceObject, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }

    if (NT_SUCCESS(status) && ReplySize != NULL) {
        // The lower driver must never report more bytes than we provided; a
        // larger Information would make callers read past the reply buffer.
        if (iosb.Information > ReplyBufferSize) {
            KdPrint(("T2Audio: BCE reply overran buffer: %Iu > %u\n",
                     iosb.Information, ReplyBufferSize));
            *ReplySize = ReplyBufferSize;
        } else {
            *ReplySize = (ULONG)iosb.Information;
        }
    }

    return status;
}

// Send a GET_PROPERTY command for the device UID and copy the raw (not
// NUL-terminated) UID bytes into UidBuffer.
static NTSTATUS
T2AudioGetDeviceUid(
    _In_ ULONG64 DeviceId,
    _Out_writes_bytes_(UidBufferSize) CHAR *UidBuffer,
    _In_ ULONG UidBufferSize,
    _Out_ PULONG UidLength)
{
    UCHAR requestBuffer[T2AUDIO_BCE_HEADER_SIZE + T2AUDIO_BCE_BASE_SIZE +
                        T2AUDIO_PROP_REQUEST_PAYLOAD_SIZE];
    UCHAR replyBuffer[512];
    T2AUDIO_MSG_HEADER *header;
    T2AUDIO_MSG_BASE *base;
    UCHAR *payload;
    ULONG replySize;
    ULONG64 obj;
    ULONG element;
    ULONG scope;
    ULONG selector;
    ULONG64 dataOffset;
    ULONG64 dataSize;
    NTSTATUS status;

    PAGED_CODE();

    RtlZeroMemory(requestBuffer, sizeof(requestBuffer));
    header = (T2AUDIO_MSG_HEADER *)requestBuffer;
    base = (T2AUDIO_MSG_BASE *)(header + 1);
    payload = (UCHAR *)(base + 1);

    RtlCopyMemory(header->Tag, T2AUDIO_BCE_TAG, 4);
    header->Type = T2AUDIO_MSG_TYPE_COMMAND;
    header->DeviceId = DeviceId;
    base->Message = T2AUDIO_MSG_GET_PROPERTY;
    base->Status = 0;

    // obj, element, scope, selector, qualifier_size (no qualifier).
    T2AudioBceWriteU64(payload + 0, DeviceId);
    T2AudioBceWriteU32(payload + 8, 0);
    T2AudioBceWriteU32(payload + 12, T2AUDIO_PROP_SCOPE_GLOBAL);
    T2AudioBceWriteU32(payload + 16, T2AUDIO_PROP_UID);
    T2AudioBceWriteU64(payload + 20, 0);

    status = T2AudioSendBceMessage(
        requestBuffer,
        (ULONG)sizeof(requestBuffer),
        replyBuffer,
        sizeof(replyBuffer),
        &replySize);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: GET_PROPERTY send failed: 0x%08X\n", status));
        return status;
    }

    if (!T2AudioBceParsePropertyResponse(replyBuffer, replySize,
                                         T2AUDIO_MSG_GET_PROPERTY_RESPONSE,
                                         &obj, &element, &scope, &selector,
                                         &dataOffset, &dataSize)) {
        KdPrint(("T2Audio: GET_PROPERTY reply invalid: size=%u\n", replySize));
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    // The reply must echo the property we asked for on the same device.
    if (obj != DeviceId || element != 0 ||
        scope != T2AUDIO_PROP_SCOPE_GLOBAL || selector != T2AUDIO_PROP_UID) {
        KdPrint(("T2Audio: GET_PROPERTY reply mismatch: obj=0x%I64X el=%u "
                 "scope=0x%08X sel=0x%08X\n",
                 obj, element, scope, selector));
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (dataSize > UidBufferSize) {
        // UID longer than we accept: treat as a non-matching device.
        KdPrint(("T2Audio: device 0x%I64X UID too long: %I64u\n",
                 DeviceId, dataSize));
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlCopyMemory(UidBuffer, replyBuffer + dataOffset, (SIZE_T)dataSize);
    *UidLength = (ULONG)dataSize;
    return STATUS_SUCCESS;
}

NTSTATUS
T2AudioGetDeviceList(
    _Out_writes_(MaxDevices) ULONG64 *DeviceList,
    _In_ ULONG MaxDevices,
    _Out_ PULONG DeviceCount)
{
    UCHAR requestBuffer[32];
    UCHAR replyBuffer[512];
    T2AUDIO_MSG_HEADER *header;
    T2AUDIO_MSG_BASE *base;
    ULONG replySize;
    ULONG outCount;
    NTSTATUS status;

    PAGED_CODE();

    if (DeviceList == NULL || DeviceCount == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(requestBuffer, sizeof(requestBuffer));
    header = (T2AUDIO_MSG_HEADER *)requestBuffer;
    base = (T2AUDIO_MSG_BASE *)(header + 1);

    // Build GET_DEVICE_LIST command
    RtlCopyMemory(header->Tag, T2AUDIO_BCE_TAG, 4);
    header->Type = T2AUDIO_MSG_TYPE_COMMAND;
    header->DeviceId = 0;
    base->Message = T2AUDIO_MSG_GET_DEVICE_LIST;
    base->Status = 0;

    status = T2AudioSendBceMessage(
        requestBuffer,
        sizeof(T2AUDIO_MSG_HEADER) + sizeof(T2AUDIO_MSG_BASE),
        replyBuffer,
        sizeof(replyBuffer),
        &replySize);

    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: GET_DEVICE_LIST send failed: 0x%08X\n", status));
        return status;
    }

    if (!T2AudioBceParseDeviceListResponse(replyBuffer, replySize,
                                           T2AUDIO_MSG_GET_DEVICE_LIST_RESPONSE,
                                           DeviceList, MaxDevices, &outCount)) {
        KdPrint(("T2Audio: GET_DEVICE_LIST reply invalid: size=%u\n", replySize));
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    *DeviceCount = outCount;
    KdPrint(("T2Audio: found %u BCE devices\n", outCount));

    return STATUS_SUCCESS;
}

NTSTATUS
T2AudioFindSpeakerDeviceId(
    _In_ PT2AUDIO_DEVICE_CONTEXT Context,
    _Out_ PULONG64 DeviceId)
{
    ULONG64 deviceList[32];
    ULONG deviceCount;
    ULONG i;
    NTSTATUS status;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(Context);

    if (DeviceId == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *DeviceId = 0;

    status = T2AudioGetDeviceList(deviceList, ARRAYSIZE(deviceList), &deviceCount);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (deviceCount == 0) {
        KdPrint(("T2Audio: no BCE devices found\n"));
        return STATUS_DEVICE_NOT_READY;
    }

    // Match the speaker by querying each device's UID, as kaiT2en does against
    // the BufferStruct device name ("Speaker"). Do not guess with deviceList[0].
    for (i = 0; i < deviceCount; i++) {
        CHAR uid[T2AUDIO_MAX_UID_LENGTH + 1];
        ULONG uidLength = 0;

        status = T2AudioGetDeviceUid(deviceList[i], uid,
                                     T2AUDIO_MAX_UID_LENGTH, &uidLength);
        if (!NT_SUCCESS(status)) {
            KdPrint(("T2Audio: UID query failed for device 0x%I64X: 0x%08X\n",
                     deviceList[i], status));
            continue;
        }

        if (T2AudioBceUidIsSpeaker(uid, uidLength)) {
            *DeviceId = deviceList[i];
            KdPrint(("T2Audio: speaker device id 0x%I64X\n", *DeviceId));
            return STATUS_SUCCESS;
        }
    }

    KdPrint(("T2Audio: no speaker BCE device found among %u\n", deviceCount));
    return STATUS_DEVICE_NOT_READY;
}

// Diagnostic-only discovery. Opens the BCE transport, enumerates devices and
// logs each UID, and records the speaker id in Context->BceSpeakerDeviceId.
// It deliberately does NOT touch Context->SpeakerDeviceId, so the audio path
// (MMIO buffer, START_IO) stays disabled. The transport is closed again so no
// long-lived reference is held while the audio path is unwired.
NTSTATUS
T2AudioProbeBceDevices(_Inout_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    ULONG64 deviceList[32];
    ULONG deviceCount;
    ULONG i;
    NTSTATUS status;

    PAGED_CODE();

    if (Context == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    Context->BceSpeakerDeviceId = 0;
    Context->BceProbed = FALSE;

    status = T2AudioOpenBceTransport();
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: BCE probe: transport unavailable: 0x%08X\n", status));
        return status;
    }

    status = T2AudioGetDeviceList(deviceList, ARRAYSIZE(deviceList), &deviceCount);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: BCE probe: device list failed: 0x%08X\n", status));
        T2AudioCloseBceTransport();
        return status;
    }

    KdPrint(("T2Audio: BCE probe: %u device(s)\n", deviceCount));

    for (i = 0; i < deviceCount; i++) {
        CHAR uid[T2AUDIO_MAX_UID_LENGTH + 1];
        ULONG uidLength = 0;

        status = T2AudioGetDeviceUid(deviceList[i], uid,
                                     T2AUDIO_MAX_UID_LENGTH, &uidLength);
        if (!NT_SUCCESS(status)) {
            KdPrint(("T2Audio: BCE probe: device 0x%I64X uid failed: 0x%08X\n",
                     deviceList[i], status));
            continue;
        }

        KdPrint(("T2Audio: BCE probe: device 0x%I64X uid=\"%.*s\" (%u bytes)\n",
                 deviceList[i], (int)uidLength, uid, uidLength));

        if (Context->BceSpeakerDeviceId == 0 &&
            T2AudioBceUidIsSpeaker(uid, uidLength)) {
            Context->BceSpeakerDeviceId = deviceList[i];
        }
    }

    Context->BceProbed = TRUE;
    KdPrint(("T2Audio: BCE probe done; speaker id 0x%I64X "
             "(diagnostic only, audio path stays disabled)\n",
             Context->BceSpeakerDeviceId));

    T2AudioCloseBceTransport();
    return STATUS_SUCCESS;
}
