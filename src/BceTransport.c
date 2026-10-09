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

// Remote-access handshake. kaiT2en issues SET_REMOTE_ACCESS(ON) at device init
// (t2audio_cmd_set_remote_access, message 32, mode 2) so the T2 hands audio
// control to the host. The command is device-independent (device_id 0) and its
// payload is a single u64 mode; the reply is a plain command response (33).
#define T2AUDIO_MSG_SET_REMOTE_ACCESS 32
#define T2AUDIO_MSG_SET_REMOTE_ACCESS_RESPONSE 33
#define T2AUDIO_REMOTE_ACCESS_OFF 0
#define T2AUDIO_REMOTE_ACCESS_ON 2

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

// Bounded wait for a BCE request. The lower AppleUSBVHCI driver can, under some
// conditions, never complete the IRP; without a bound the caller (StartDevice)
// would block forever and stall PnP for the entire device. We time out and
// abandon the IRP in place (never cancelling it, which would race with the I/O
// manager freeing it).
//
// Scope: this bounds only the wait for completion. It cannot bound a lower
// driver that blocks *inside* its dispatch routine (i.e. inside IoCallDriver);
// that is a lower-driver defect we have no way to interrupt from here.
#define T2AUDIO_BCE_SEND_TIMEOUT_MS  2000

// Upper bound on how long any caller will wait to take the transport lock. A
// send holds the lock for up to T2AUDIO_BCE_SEND_TIMEOUT_MS, so this is set a
// little higher: if a caller cannot get the lock in this window the transport
// is stuck and the caller must fail fast rather than block PnP/teardown.
#define T2AUDIO_BCE_LOCK_TIMEOUT_MS  (T2AUDIO_BCE_SEND_TIMEOUT_MS + 1000)

#define T2AUDIO_BCE_POOL_TAG 'AbT2'

// Everything the I/O manager writes back on completion lives here, so that if we
// ever have to abandon the IRP we can leak it without leaving the completion
// path pointing at a dead stack frame.
typedef struct _T2AUDIO_BCE_SEND_CONTEXT {
    KEVENT Event;
    IO_STATUS_BLOCK Iosb;
    PUCHAR ReplyBuffer;
    ULONG ReplyBufferSize;
} T2AUDIO_BCE_SEND_CONTEXT, *PT2AUDIO_BCE_SEND_CONTEXT;

// Serializes every access to the transport. A send holds this lock across the
// whole IOCTL (including the wait for completion) so T2AudioCloseBceTransport
// can never dereference the file object while an IRP is still in flight against
// the device it keeps alive. Initialized once from DriverEntry.
//
// This is a KMUTEX, not a FAST_MUTEX. IoGetDeviceObjectPointer (called by Open)
// requires PASSIVE_LEVEL, but a FAST_MUTEX raises IRQL to APC_LEVEL, so holding
// one across the open was illegal. Every caller of this transport
// (StartDevice, SetState, teardown) runs at PASSIVE_LEVEL, so a KMUTEX is both
// correct and sufficient.
static KMUTEX g_BceLock;
static BOOLEAN g_BceLockReady = FALSE;

// Set when a send is abandoned in flight (the lower driver never completed it).
// Once poisoned the transport accepts no further sends and is never closed, so
// the abandoned IRP's target device stays alive for as long as the IRP exists.
static volatile LONG g_BceTransportPoisoned = 0;

// Take the transport lock, waiting at most T2AUDIO_BCE_LOCK_TIMEOUT_MS. Returns
// STATUS_SUCCESS on success or STATUS_TIMEOUT if the lock could not be taken.
static NTSTATUS
T2AudioBceLockAcquire(VOID)
{
    LARGE_INTEGER timeout;

    timeout.QuadPart = -((LONGLONG)T2AUDIO_BCE_LOCK_TIMEOUT_MS * 10000LL);
    return KeWaitForSingleObject(&g_BceLock, Executive, KernelMode, FALSE, &timeout);
}

static VOID
T2AudioBceLockRelease(VOID)
{
    KeReleaseMutex(&g_BceLock, FALSE);
}

VOID
T2AudioInitializeBceTransport(VOID)
{
    KeInitializeMutex(&g_BceLock, 0);
    g_BceLockReady = TRUE;
}

NTSTATUS
T2AudioOpenBceTransport(VOID)
{
    UNICODE_STRING deviceName;
    NTSTATUS status;

    PAGED_CODE();

    if (!g_BceLockReady) {
        return STATUS_DEVICE_NOT_READY;
    }

    // T2AudioBceLockAcquire returns the KeWaitForSingleObject result, whose
    // only failure value is STATUS_TIMEOUT (0x102). That is a positive NTSTATUS,
    // so NT_SUCCESS() would wrongly accept it; compare against STATUS_SUCCESS.
    status = T2AudioBceLockAcquire();
    if (status != STATUS_SUCCESS) {
        KdPrint(("T2Audio: BCE open could not take lock: 0x%08X\n", status));
        return STATUS_IO_TIMEOUT;
    }

    if (g_BceTransport.DeviceObject != NULL) {
        T2AudioBceLockRelease();
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
        T2AudioBceLockRelease();
        return status;
    }

    KdPrint(("T2Audio: BCE transport opened\n"));
    T2AudioBceLockRelease();
    return STATUS_SUCCESS;
}

VOID
T2AudioCloseBceTransport(VOID)
{
    PAGED_CODE();

    if (!g_BceLockReady) {
        return;
    }

    if (T2AudioBceLockAcquire() != STATUS_SUCCESS) {
        // A send is wedged holding the lock; do not block teardown on it. The
        // file-object reference stays, which is what keeps the abandoned IRP's
        // device alive anyway.
        KdPrint(("T2Audio: BCE close could not take lock; leaving transport\n"));
        return;
    }

    // If a send was abandoned in flight, do not drop the file-object reference:
    // it keeps the target device alive for the IRP that is still outstanding.
    if (InterlockedCompareExchange(&g_BceTransportPoisoned, 0, 0) != 0) {
        KdPrint(("T2Audio: BCE transport poisoned; not closing\n"));
        T2AudioBceLockRelease();
        return;
    }

    if (g_BceTransport.FileObject != NULL) {
        ObDereferenceObject(g_BceTransport.FileObject);
        g_BceTransport.FileObject = NULL;
        g_BceTransport.DeviceObject = NULL;
        KdPrint(("T2Audio: BCE transport closed\n"));
    }

    T2AudioBceLockRelease();
}

static LARGE_INTEGER
T2AudioBceRelativeTimeout(_In_ ULONG Milliseconds)
{
    LARGE_INTEGER timeout;

    timeout.QuadPart = -((LONGLONG)Milliseconds * 10000LL);
    return timeout;
}

NTSTATUS
T2AudioSendBceMessage(
    _In_reads_bytes_(MessageSize) const UCHAR *Message,
    _In_ ULONG MessageSize,
    _Out_writes_bytes_opt_(ReplyBufferSize) UCHAR *ReplyBuffer,
    _In_ ULONG ReplyBufferSize,
    _Out_opt_ PULONG ReplySize)
{
    PT2AUDIO_BCE_SEND_CONTEXT context;
    PDEVICE_OBJECT deviceObject;
    PIRP irp;
    NTSTATUS status;
    LARGE_INTEGER timeout;

    PAGED_CODE();

    // Every BCE request ends in a blocking Executive wait, which is only legal
    // at PASSIVE_LEVEL. Refuse anything higher rather than risk an illegal wait
    // from a DPC/dispatch context (the caller logs the IRQL).
    if (KeGetCurrentIrql() > PASSIVE_LEVEL) {
        KdPrint(("T2Audio: BCE send refused at IRQL %u (needs PASSIVE)\n",
                 (ULONG)KeGetCurrentIrql()));
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (!g_BceLockReady) {
        return STATUS_DEVICE_NOT_READY;
    }

    if (ReplyBufferSize != 0 && ReplyBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    // Hold the lock across the whole request so the transport cannot be closed
    // while this IRP is in flight. The file-object reference taken at open keeps
    // the target device alive for the duration.
    status = T2AudioBceLockAcquire();
    if (status != STATUS_SUCCESS) {
        KdPrint(("T2Audio: BCE send could not take lock: 0x%08X\n", status));
        return STATUS_IO_TIMEOUT;
    }

    if (InterlockedCompareExchange(&g_BceTransportPoisoned, 0, 0) != 0) {
        T2AudioBceLockRelease();
        return STATUS_IO_TIMEOUT;
    }

    deviceObject = g_BceTransport.DeviceObject;
    if (deviceObject == NULL || g_BceTransport.FileObject == NULL) {
        T2AudioBceLockRelease();
        return STATUS_DEVICE_NOT_READY;
    }

    // Allocate the completion state and reply landing buffer from the pool. If
    // the IRP has to be abandoned we leak this block rather than free it, so a
    // late completion can never touch a recycled stack frame.
    context = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*context), T2AUDIO_BCE_POOL_TAG);
    if (context == NULL) {
        T2AudioBceLockRelease();
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(context, sizeof(*context));
    context->ReplyBufferSize = ReplyBufferSize;

    if (ReplyBufferSize != 0) {
        context->ReplyBuffer = ExAllocatePoolWithTag(NonPagedPoolNx, ReplyBufferSize,
                                                     T2AUDIO_BCE_POOL_TAG);
        if (context->ReplyBuffer == NULL) {
            ExFreePoolWithTag(context, T2AUDIO_BCE_POOL_TAG);
            T2AudioBceLockRelease();
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    KeInitializeEvent(&context->Event, NotificationEvent, FALSE);

    irp = IoBuildDeviceIoControlRequest(
        IOCTL_APPLE_BCE_SEND_MESSAGE,
        deviceObject,
        (PVOID)Message,
        MessageSize,
        context->ReplyBuffer,
        ReplyBufferSize,
        FALSE,
        &context->Event,
        &context->Iosb);

    if (irp == NULL) {
        if (context->ReplyBuffer != NULL) {
            ExFreePoolWithTag(context->ReplyBuffer, T2AUDIO_BCE_POOL_TAG);
        }
        ExFreePoolWithTag(context, T2AUDIO_BCE_POOL_TAG);
        T2AudioBceLockRelease();
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Associate the opened transport file object with the outgoing request;
    // the transport lock retains its reference through completion. Note: this
    // does NOT fix AppleUSBVHCI's separate forwarded IRP that bugchecks in KS
    // when START_IO is sent. That command is blocked in T2AudioStartIo.
    IoGetNextIrpStackLocation(irp)->FileObject = g_BceTransport.FileObject;

    // Phase diagnostics: record exactly what leaves the driver and how far the
    // send gets. If a bugcheck occurs while a specific command is in flight, the
    // last line printed pins whether we reached IoCallDriver, returned from it,
    // or never saw a completion - i.e. a lower-driver fault vs. our own
    // post-processing.
    if (MessageSize >= T2AUDIO_BCE_HEADER_SIZE + T2AUDIO_BCE_BASE_SIZE) {
        KdPrint(("T2Audio: BCE send ioctl=0x%08X dev=%p in=%u tag=%.4s type=%u id=0x%I64X msg=%u\n",
                 (ULONG)IOCTL_APPLE_BCE_SEND_MESSAGE, deviceObject, MessageSize,
                 (const char *)Message, (ULONG)Message[T2AUDIO_BCE_TYPE_OFFSET],
                 T2AudioBceReadU64(Message + 5),
                 T2AudioBceReadU32(Message + T2AUDIO_BCE_HEADER_SIZE)));
    }

    status = IoCallDriver(deviceObject, irp);
    KdPrint(("T2Audio: BCE send IoCallDriver returned 0x%08X\n", status));

    if (status == STATUS_PENDING) {
        timeout = T2AudioBceRelativeTimeout(T2AUDIO_BCE_SEND_TIMEOUT_MS);
        status = KeWaitForSingleObject(&context->Event, Executive, KernelMode,
                                       FALSE, &timeout);
        KdPrint(("T2Audio: BCE send wait=0x%08X iosb.Status=0x%08X info=%Iu\n",
                 status, context->Iosb.Status, context->Iosb.Information));
        if (status == STATUS_TIMEOUT) {
            // The lower driver did not complete the IRP within the bound. Do NOT
            // call IoCancelIrp here: IoBuildDeviceIoControlRequest's IRP is freed
            // by the I/O manager the moment it completes, so cancelling after a
            // timeout races with that free and can dereference a recycled IRP.
            // Instead abandon the IRP in place and poison the transport: leak
            // context (and its reply buffer) so a late completion can never touch
            // recycled memory, and never close the transport so the file-object
            // reference keeps the target device alive for as long as the IRP
            // exists. Future sends fail fast. This bounds the PnP path to a
            // single leaked IRP instead of an unbounded wait.
            //
            // Ownership: this IRP is thread-bound (IoBuildDeviceIoControlRequest
            // queues it to the current thread), so the I/O manager owns it once
            // built. If our thread exits while it is still pending the I/O
            // manager cancels and frees it; if the lower driver eventually
            // completes it, the I/O manager frees it. Either way we never touch
            // it again and the leaked context is never referenced - so both
            // paths are safe, at the cost of one leaked context block.
            //
            // What this does NOT bound: if the lower driver blocks *inside*
            // IoCallDriver (never returning), this thread stays stuck in that
            // call. Nothing above can interrupt it; the bounded lock wait only
            // keeps the *other* callers (PnP/teardown) moving.
            InterlockedExchange(&g_BceTransportPoisoned, 1);
            KdPrint(("T2Audio: BCE send timed out; transport poisoned, "
                     "abandoning IRP\n"));
            T2AudioBceLockRelease();
            return STATUS_IO_TIMEOUT;
        }
        status = context->Iosb.Status;
    } else {
        status = context->Iosb.Status;
    }

    if (NT_SUCCESS(status)) {
        // The lower driver must never report more bytes than we provided; a
        // larger Information would make callers read past the reply buffer.
        ULONG information = (context->Iosb.Information > ReplyBufferSize)
                                ? ReplyBufferSize
                                : (ULONG)context->Iosb.Information;
        if (information != 0 && ReplyBuffer != NULL && context->ReplyBuffer != NULL) {
            RtlCopyMemory(ReplyBuffer, context->ReplyBuffer, information);
        }
        if (ReplySize != NULL) {
            *ReplySize = information;
        }
    }

    if (context->ReplyBuffer != NULL) {
        ExFreePoolWithTag(context->ReplyBuffer, T2AUDIO_BCE_POOL_TAG);
    }
    ExFreePoolWithTag(context, T2AUDIO_BCE_POOL_TAG);

    T2AudioBceLockRelease();
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

// Take (Enable=TRUE) or release (Enable=FALSE) host audio control over BCE.
// This mirrors kaiT2en's t2audio_cmd_set_remote_access: header device_id 0, base
// message 32, an 8-byte little-endian mode (0 = off, 2 = on), and a command
// response (message 33) echoing device_id 0. START_IO is only legal once the T2
// has accepted this, so the caller records success in Context->BceRemoteAccess
// and T2AudioStartIo refuses to run without it.
NTSTATUS
T2AudioSetRemoteAccess(_In_ BOOLEAN Enable)
{
    UCHAR message[T2AUDIO_BCE_HEADER_SIZE + T2AUDIO_BCE_BASE_SIZE +
                  T2AUDIO_BCE_U64_SIZE];
    UCHAR reply[256];
    T2AUDIO_MSG_HEADER *header;
    T2AUDIO_MSG_BASE *base;
    UCHAR *payload;
    ULONG replySize;
    NTSTATUS status;

    PAGED_CODE();

    RtlZeroMemory(message, sizeof(message));
    header = (T2AUDIO_MSG_HEADER *)message;
    base = (T2AUDIO_MSG_BASE *)(header + 1);
    payload = (UCHAR *)(base + 1);

    RtlCopyMemory(header->Tag, T2AUDIO_BCE_TAG, 4);
    header->Type = T2AUDIO_MSG_TYPE_COMMAND;
    header->DeviceId = 0;
    base->Message = T2AUDIO_MSG_SET_REMOTE_ACCESS;
    base->Status = 0;
    T2AudioBceWriteU64(payload, Enable ? T2AUDIO_REMOTE_ACCESS_ON
                                        : T2AUDIO_REMOTE_ACCESS_OFF);

    status = T2AudioSendBceMessage(message, (ULONG)sizeof(message),
                                   reply, sizeof(reply), &replySize);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: SET_REMOTE_ACCESS(%u) send failed: 0x%08X\n",
                 Enable ? 1u : 0u, status));
        return status;
    }

    if (!T2AudioBceParseCommandResponse(reply, replySize,
                                        T2AUDIO_MSG_SET_REMOTE_ACCESS_RESPONSE,
                                        0)) {
        KdPrint(("T2Audio: SET_REMOTE_ACCESS(%u) reply invalid (size=%u)\n",
                 Enable ? 1u : 0u, replySize));
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    KdPrint(("T2Audio: SET_REMOTE_ACCESS(%u) acknowledged\n",
             Enable ? 1u : 0u));
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

// Opens the BCE transport, enumerates devices and logs each UID, and records
// the speaker id in Context->BceSpeakerDeviceId. It does not touch
// Context->SpeakerDeviceId itself; the caller (T2AudioStartDevice) decides
// whether to wire the speaker into the audio path. On success the transport is
// left OPEN so START_IO/STOP_IO can use it; the caller closes it when no
// speaker was found, and T2AudioUnmapResources closes it on teardown.
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
    KdPrint(("T2Audio: BCE probe done; speaker id 0x%I64X\n",
             Context->BceSpeakerDeviceId));

    // Transport intentionally left open for START_IO/STOP_IO.
    return STATUS_SUCCESS;
}
