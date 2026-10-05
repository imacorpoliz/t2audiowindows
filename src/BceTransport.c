#include "T2AudioMiniport.h"

// T2 BCE Protocol constants (from Linux KAIT2EN)
#define T2AUDIO_BCE_TAG "Audt"
#define T2AUDIO_MSG_TYPE_COMMAND 1
#define T2AUDIO_MSG_TYPE_RESPONSE 2

#define T2AUDIO_MSG_GET_DEVICE_LIST 101
#define T2AUDIO_MSG_GET_DEVICE_LIST_RESPONSE 102

// AppleUSBVHCI device name (from user-mode testing)
#define APPLE_USBVHCI_DEVICE_NAME L"\\Device\\AppleUSBVHCI"

// IOCTL code (from user-mode testing: 0x222018)
#define IOCTL_APPLE_BCE_SEND_MESSAGE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x2806, METHOD_BUFFERED, FILE_ANY_ACCESS)

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
        *ReplySize = (ULONG)iosb.Information;
    }

    return status;
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
    ULONG64 *deviceArray;
    ULONG64 count;
    ULONG i;
    NTSTATUS status;

    PAGED_CODE();

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

    if (replySize < sizeof(T2AUDIO_MSG_HEADER) + sizeof(T2AUDIO_MSG_BASE) + sizeof(ULONG64)) {
        KdPrint(("T2Audio: GET_DEVICE_LIST reply too small: %u\n", replySize));
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    header = (T2AUDIO_MSG_HEADER *)replyBuffer;
    base = (T2AUDIO_MSG_BASE *)(header + 1);

    if (base->Message != T2AUDIO_MSG_GET_DEVICE_LIST_RESPONSE) {
        KdPrint(("T2Audio: unexpected reply message: %u\n", base->Message));
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    // Parse device count and array
    count = *(ULONG64 *)(base + 1);
    deviceArray = (ULONG64 *)((UCHAR *)(base + 1) + sizeof(ULONG64));

    if (count > MaxDevices) {
        count = MaxDevices;
    }

    for (i = 0; i < count; i++) {
        DeviceList[i] = deviceArray[i];
    }

    *DeviceCount = (ULONG)count;
    KdPrint(("T2Audio: found %u BCE devices\n", (ULONG)count));

    return STATUS_SUCCESS;
}

NTSTATUS
T2AudioFindSpeakerDeviceId(
    _In_ PT2AUDIO_DEVICE_CONTEXT Context,
    _Out_ PULONG64 DeviceId)
{
    ULONG64 deviceList[32];
    ULONG deviceCount;
    NTSTATUS status;

    PAGED_CODE();

    *DeviceId = 0;

    status = T2AudioGetDeviceList(deviceList, ARRAYSIZE(deviceList), &deviceCount);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (deviceCount == 0) {
        KdPrint(("T2Audio: no BCE devices found\n"));
        return STATUS_DEVICE_NOT_READY;
    }

    // TODO: Linux KAIT2EN matches device by name via GET_PROPERTY(uid).
    // BufferStruct has "Speaker" name, but BCE protocol requires GET_PROPERTY
    // to query device names from BCE device IDs. Until proper name matching
    // is implemented via GET_PROPERTY, return error instead of guessing.
    KdPrint(("T2Audio: BCE device name matching not implemented\n"));
    return STATUS_NOT_IMPLEMENTED;
}
