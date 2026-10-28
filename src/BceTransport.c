#include "T2AudioMiniport.h"
#include "BceProtocolLogic.h"
#include "T2AudioSpeakerLogic.h"

// T2 BCE Protocol constants (from kaiT2en modules/t2bce_audio/protocol.h).
// The 4-byte tag is not a constant: T2AudioSendBceMessage stamps a request
// counter 1..127 into every message, as AppleAudio.sys does.
#define T2AUDIO_MSG_TYPE_COMMAND 1
#define T2AUDIO_MSG_TYPE_RESPONSE 2

// Remote-access handshake values
#define T2AUDIO_REMOTE_ACCESS_OFF 0
#define T2AUDIO_REMOTE_ACCESS_ON 2

// Property scope/selector values (kaiT2en modules/t2bce_audio/protocol.h).
#define T2AUDIO_PROP_SCOPE_GLOBAL 0x676c6f62u   // 'glob'
#define T2AUDIO_PROP_SCOPE_OUTPUT 0x6f757470u   // 'outp'
#define T2AUDIO_PROP_SCOPE_INPUT  0x696e7074u   // 'inpt'
#define T2AUDIO_PROP_JACK_PLUGGED 0x6a61636bu   // 'jack'
#define T2AUDIO_PROP_UID          0x75696420u   // 'uid '
#define T2AUDIO_PROP_LATENCY      0x6c746e63u   // 'ltnc'
#define T2AUDIO_PROP_SAFETY_OFFSET 0x73616674u  // 'saft'
#define T2AUDIO_PROP_PHYS_FORMAT  0x70667420u   // 'pft '

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
//
// AppleUSBVHCI's own 0x222018 handler (0x140002128) waits up to 5 s for the
// bridgeOS reply and then completes the IRP itself. Our bound is longer, so
// a slow reply is resolved by VHCI (completed IRP) instead of us abandoning
// the IRP and poisoning the transport while VHCI is still waiting.
#define T2AUDIO_BCE_SEND_TIMEOUT_MS  6000

// Largest message this driver sends (GET_PROPERTY = 49 bytes); the tagged copy
// lives on the stack.
#define T2AUDIO_BCE_MAX_MESSAGE_SIZE 128u

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

// Request tag counter (1..127), protected by g_BceLock.
static ULONG g_BceTagCounter = 0;

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
    UCHAR tagged[T2AUDIO_BCE_MAX_MESSAGE_SIZE];
    ULONG tag;

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
    if (Message == NULL || MessageSize < T2AUDIO_BCE_HEADER_SIZE ||
        MessageSize > sizeof(tagged)) {
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

    // Stamp the request tag into a copy of the message. METHOD_BUFFERED: the
    // I/O manager copies the input into the IRP's system buffer while building
    // it, so the stack copy does not need to outlive this call.
    RtlCopyMemory(tagged, Message, MessageSize);
    tag = T2AudioBceNextTag(&g_BceTagCounter);
    T2AudioBceWriteU32(tagged, tag);

    irp = IoBuildDeviceIoControlRequest(
        IOCTL_APPLE_BCE_SEND_MESSAGE,
        deviceObject,
        tagged,
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

    // Leave the outgoing IRP FileObject unset, as in the current experiment.
    // This has NOT been validated as a fix: the previous bugcheck involved a
    // forwarded IRP, whose construction and routing remain unproven.

    // Phase diagnostics: record exactly what leaves the driver and how far the
    // send gets. If a bugcheck occurs while a specific command is in flight, the
    // last line printed pins whether we reached IoCallDriver, returned from it,
    // or never saw a completion - i.e. a lower-driver fault vs. our own
    // post-processing.
    if (MessageSize >= T2AUDIO_BCE_HEADER_SIZE + T2AUDIO_BCE_BASE_SIZE) {
        KdPrint(("T2Audio: BCE send ioctl=0x%08X dev=%p in=%u tag=%u type=%u id=0x%I64X msg=%u\n",
                 (ULONG)IOCTL_APPLE_BCE_SEND_MESSAGE, deviceObject, MessageSize,
                 tag, (ULONG)Message[T2AUDIO_BCE_TYPE_OFFSET],
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
    } else if (NT_SUCCESS(status)) {
        // Completed synchronously with success; read final status from IOSB
        status = context->Iosb.Status;
    }
    // If IoCallDriver returned a synchronous failure, preserve that failure status.

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
        // AppleUSBVHCI hands back whatever woke its reply event - normally
        // the RESPONSE, but a NOTIFICATION_ALIVE arriving meanwhile wakes it
        // too. Only a reply carrying our tag answers this request.
        if (ReplyBufferSize != 0 &&
            !T2AudioBceReplyTagMatches(context->ReplyBuffer, information, tag)) {
            KdPrint(("T2Audio: BCE reply tag mismatch: sent %u, got %u (size=%u type=%u msg=%u)\n",
                     tag,
                     (information >= 4) ? T2AudioBceReadU32(context->ReplyBuffer) : 0u,
                     information,
                     (information > T2AUDIO_BCE_TYPE_OFFSET)
                         ? (ULONG)context->ReplyBuffer[T2AUDIO_BCE_TYPE_OFFSET] : 0u,
                     (information >= T2AUDIO_BCE_HEADER_SIZE + 4)
                         ? T2AudioBceReadU32(context->ReplyBuffer + T2AUDIO_BCE_HEADER_SIZE) : 0u));
            status = STATUS_DEVICE_PROTOCOL_ERROR;
        }
    }

    if (context->ReplyBuffer != NULL) {
        ExFreePoolWithTag(context->ReplyBuffer, T2AUDIO_BCE_POOL_TAG);
    }
    ExFreePoolWithTag(context, T2AUDIO_BCE_POOL_TAG);

    T2AudioBceLockRelease();
    return status;
}

// Send a GET_PROPERTY command for (ObjectId, Scope, Selector, element 0) on
// DeviceId and copy the raw property data into DataBuffer. The reply must echo
// the requested object and address.
static NTSTATUS
T2AudioGetProperty(
    _In_ ULONG64 DeviceId,
    _In_ ULONG64 ObjectId,
    _In_ ULONG Scope,
    _In_ ULONG Selector,
    _Out_writes_bytes_(DataBufferSize) PVOID DataBuffer,
    _In_ ULONG DataBufferSize,
    _Out_ PULONG DataLength)
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

    // header->Tag is stamped by T2AudioSendBceMessage.
    header->Type = T2AUDIO_MSG_TYPE_COMMAND;
    header->DeviceId = DeviceId;
    base->Message = T2AUDIO_MSG_GET_PROPERTY;
    base->Status = 0;

    // obj, element, scope, selector, qualifier_size (no qualifier).
    T2AudioBceWriteU64(payload + 0, ObjectId);
    T2AudioBceWriteU32(payload + 8, 0);
    T2AudioBceWriteU32(payload + 12, Scope);
    T2AudioBceWriteU32(payload + 16, Selector);
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

    // The reply must echo the property we asked for on the same object.
    if (obj != ObjectId || element != 0 ||
        scope != Scope || selector != Selector) {
        KdPrint(("T2Audio: GET_PROPERTY reply mismatch: obj=0x%I64X el=%u "
                 "scope=0x%08X sel=0x%08X\n",
                 obj, element, scope, selector));
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (dataSize > DataBufferSize) {
        // Longer than the caller accepts (e.g. an oversized UID): treat as a
        // non-matching object.
        KdPrint(("T2Audio: obj 0x%I64X property 0x%08X too long: %I64u\n",
                 ObjectId, Selector, dataSize));
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlCopyMemory(DataBuffer, replyBuffer + dataOffset, (SIZE_T)dataSize);
    *DataLength = (ULONG)dataSize;
    return STATUS_SUCCESS;
}

static NTSTATUS
T2AudioGetDeviceUid(
    _In_ ULONG64 DeviceId,
    _Out_writes_bytes_(UidBufferSize) CHAR *UidBuffer,
    _In_ ULONG UidBufferSize,
    _Out_ PULONG UidLength)
{
    return T2AudioGetProperty(DeviceId, DeviceId, T2AUDIO_PROP_SCOPE_GLOBAL,
                              T2AUDIO_PROP_UID, UidBuffer, UidBufferSize,
                              UidLength);
}

// Read a 4-byte property (latency / safety offset, in frames).
static NTSTATUS
T2AudioGetU32Property(
    _In_ ULONG64 DeviceId,
    _In_ ULONG64 ObjectId,
    _In_ ULONG Scope,
    _In_ ULONG Selector,
    _Out_ PULONG Value)
{
    UCHAR data[4];
    ULONG length = 0;
    NTSTATUS status;

    *Value = 0;
    status = T2AudioGetProperty(DeviceId, ObjectId, Scope, Selector,
                                data, sizeof(data), &length);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (length != sizeof(data)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    *Value = T2AudioBceReadU32(data);
    return STATUS_SUCCESS;
}

// GET_OUTPUT_STREAM_LIST (26 -> 27) or GET_INPUT_STREAM_LIST (24 -> 25). The
// reply has the same shape as the device list (count u64, then ids), so the
// same parser validates it.
static NTSTATUS
T2AudioGetStreamList(
    _In_ ULONG64 DeviceId,
    _In_ BOOLEAN Input,
    _Out_writes_(MaxStreams) ULONG64 *StreamList,
    _In_ ULONG MaxStreams,
    _Out_ PULONG StreamCount)
{
    UCHAR requestBuffer[T2AUDIO_BCE_HEADER_SIZE + T2AUDIO_BCE_BASE_SIZE];
    UCHAR replyBuffer[256];
    T2AUDIO_MSG_HEADER *header;
    T2AUDIO_MSG_BASE *base;
    ULONG replySize;
    ULONG outCount;
    NTSTATUS status;

    PAGED_CODE();

    *StreamCount = 0;
    RtlZeroMemory(requestBuffer, sizeof(requestBuffer));
    header = (T2AUDIO_MSG_HEADER *)requestBuffer;
    base = (T2AUDIO_MSG_BASE *)(header + 1);
    // header->Tag is stamped by T2AudioSendBceMessage.
    header->Type = T2AUDIO_MSG_TYPE_COMMAND;
    header->DeviceId = DeviceId;
    base->Message = Input ? T2AUDIO_MSG_GET_INPUT_STREAM_LIST : T2AUDIO_MSG_GET_OUTPUT_STREAM_LIST;
    base->Status = 0;

    status = T2AudioSendBceMessage(requestBuffer, (ULONG)sizeof(requestBuffer),
                                   replyBuffer, sizeof(replyBuffer), &replySize);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: GET_%s_STREAM_LIST send failed: 0x%08X\n",
                 Input ? "INPUT" : "OUTPUT", status));
        return status;
    }

    if (!T2AudioBceParseDeviceListResponse(replyBuffer, replySize,
                                           Input ? T2AUDIO_MSG_GET_INPUT_STREAM_LIST_RESPONSE
                                                 : T2AUDIO_MSG_GET_OUTPUT_STREAM_LIST_RESPONSE,
                                           StreamList, MaxStreams, &outCount)) {
        KdPrint(("T2Audio: GET_%s_STREAM_LIST reply invalid: size=%u\n",
                 Input ? "INPUT" : "OUTPUT", replySize));
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    *StreamCount = outCount;
    return STATUS_SUCCESS;
}

static NTSTATUS
T2AudioGetOutputStreamList(
    _In_ ULONG64 DeviceId,
    _Out_writes_(MaxStreams) ULONG64 *StreamList,
    _In_ ULONG MaxStreams,
    _Out_ PULONG StreamCount)
{
    return T2AudioGetStreamList(DeviceId, FALSE, StreamList, MaxStreams, StreamCount);
}

// Parameters\ExtendedProbe: log one direction of a device - its stream ids,
// each stream's physical format and latency. Read-only (GET_* only).
static VOID
T2AudioLogDeviceStreams(_In_ ULONG64 DeviceId, _In_ BOOLEAN Input)
{
    ULONG64 streams[8];
    ULONG streamCount = 0;
    ULONG s;
    NTSTATUS status;

    PAGED_CODE();

    status = T2AudioGetStreamList(DeviceId, Input, streams, ARRAYSIZE(streams), &streamCount);
    KdPrint(("T2Audio: BCE detail: device 0x%I64X %s streams: %u (status 0x%08X)\n",
             DeviceId, Input ? "input" : "output", streamCount, status));
    if (!NT_SUCCESS(status)) {
        return;
    }
    for (s = 0; s < streamCount; s++) {
        UCHAR formatData[T2AUDIO_BCE_PHYS_FORMAT_SIZE];
        ULONG formatLength = 0;
        T2AUDIO_BCE_PHYS_FORMAT format;
        ULONG latency = 0;
        NTSTATUS latencyStatus;

        status = T2AudioGetProperty(DeviceId, streams[s], T2AUDIO_PROP_SCOPE_GLOBAL,
                                    T2AUDIO_PROP_PHYS_FORMAT, formatData,
                                    sizeof(formatData), &formatLength);
        if (NT_SUCCESS(status) &&
            T2AudioBceParsePhysicalFormat(formatData, formatLength, &format)) {
            KdPrint(("T2Audio: BCE detail: device 0x%I64X %s stream 0x%I64X pft: rate=%u id=0x%08X flags=0x%X bpp=%u fpp=%u bpf=%u ch=%u bits=%u\n",
                     DeviceId, Input ? "input" : "output", streams[s],
                     format.SampleRate, format.FormatId, format.FormatFlags,
                     format.BytesPerPacket, format.FramesPerPacket, format.BytesPerFrame,
                     format.ChannelsPerFrame, format.BitsPerChannel));
        } else {
            KdPrint(("T2Audio: BCE detail: device 0x%I64X %s stream 0x%I64X pft unavailable (0x%08X, len=%u)\n",
                     DeviceId, Input ? "input" : "output", streams[s], status, formatLength));
        }
        latencyStatus = T2AudioGetU32Property(DeviceId, streams[s], T2AUDIO_PROP_SCOPE_GLOBAL,
                                              T2AUDIO_PROP_LATENCY, &latency);
        KdPrint(("T2Audio: BCE detail: device 0x%I64X %s stream 0x%I64X latency=%u (0x%08X)\n",
                 DeviceId, Input ? "input" : "output", streams[s], latency, latencyStatus));
    }
}

// Parameters\ExtendedProbe: everything a later headphone or microphone
// endpoint needs to know about one device (read-only).
static VOID
T2AudioLogDeviceDetail(_In_ ULONG64 DeviceId)
{
    static const ULONG scopes[2] = { T2AUDIO_PROP_SCOPE_OUTPUT, T2AUDIO_PROP_SCOPE_INPUT };
    static const ULONG selectors[3] = { T2AUDIO_PROP_LATENCY, T2AUDIO_PROP_SAFETY_OFFSET,
                                        T2AUDIO_PROP_JACK_PLUGGED };
    ULONG i, j;

    PAGED_CODE();

    T2AudioLogDeviceStreams(DeviceId, FALSE);
    T2AudioLogDeviceStreams(DeviceId, TRUE);
    for (i = 0; i < ARRAYSIZE(scopes); i++) {
        for (j = 0; j < ARRAYSIZE(selectors); j++) {
            ULONG value = 0;
            NTSTATUS status = T2AudioGetU32Property(DeviceId, DeviceId, scopes[i],
                                                    selectors[j], &value);

            KdPrint(("T2Audio: BCE detail: device 0x%I64X %s %s=%u (0x%08X)\n",
                     DeviceId, i == 0 ? "outp" : "inpt",
                     j == 0 ? "ltnc" : (j == 1 ? "saft" : "jack"), value, status));
        }
    }
}

// Parameters\JackEvents: subscribe to 'outp'/'jack' on the headphone device,
// as kaiT2en does at init (t2audio_init_dev) and AppleAudio does with msg 11
// (0x1400042C4). bridgeOS then sends PROPERTY_CHANGED (13) notifications,
// which AppleUSBVHCI delivers through IOCTL 0x222020.
static NTSTATUS
T2AudioRegisterJackListener(_Inout_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    UCHAR request[T2AUDIO_BCE_PROPERTY_LISTENER_SIZE];
    UCHAR reply[128];
    ULONG replySize = 0;
    ULONG64 device = Context->BceHeadphoneDeviceId;
    NTSTATUS status;

    PAGED_CODE();

    (VOID)T2AudioBceBuildPropertyListener(request, device, device,
                                          T2AUDIO_PROP_SCOPE_OUTPUT,
                                          T2AUDIO_PROP_JACK_PLUGGED);
    status = T2AudioSendBceMessage(request, (ULONG)sizeof(request),
                                   reply, sizeof(reply), &replySize);
    if (NT_SUCCESS(status) &&
        !T2AudioBceParsePropertyListenerResponse(reply, replySize, device,
                                                 T2AUDIO_PROP_SCOPE_OUTPUT,
                                                 T2AUDIO_PROP_JACK_PLUGGED)) {
        status = STATUS_INVALID_DEVICE_REQUEST;
    }
    Context->JackListenerRegistered = NT_SUCCESS(status) ? TRUE : FALSE;
    KdPrint(("T2Audio: jack listener on device 0x%I64X: %s (0x%08X, reply %u bytes)\n",
             device, NT_SUCCESS(status) ? "registered" : "FAILED", status, replySize));
    return status;
}

// PASSIVE work item queued by T2AudioHandleVhciMessage for a jack
// PROPERTY_CHANGED: re-read the jack (as kaiT2en and AppleAudio do; the
// notification carries no value) and publish it to the copy path.
static IO_WORKITEM_ROUTINE T2AudioJackWorkRoutine;

static VOID
T2AudioJackWorkRoutine(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID WorkContext)
{
    PIO_WORKITEM workItem = (PIO_WORKITEM)WorkContext;
    PT2AUDIO_DEVICE_CONTEXT context = T2AudioGetContext(DeviceObject);
    ULONG plugged = 0;
    LONG previous;
    NTSTATUS status;

    PAGED_CODE();

    // Clear first: a notification arriving during the read queues another
    // pass, so the last state always wins.
    InterlockedExchange(&context->JackWorkPending, 0);
    status = T2AudioGetU32Property(context->BceHeadphoneDeviceId,
                                   context->BceHeadphoneDeviceId,
                                   T2AUDIO_PROP_SCOPE_OUTPUT,
                                   T2AUDIO_PROP_JACK_PLUGGED, &plugged);
    if (NT_SUCCESS(status)) {
        previous = InterlockedExchange(&context->HeadphoneJackPlugged, plugged != 0 ? 1 : 0);
        KdPrint(("T2Audio: headphone jack %s (was %s)%s\n",
                 plugged ? "PLUGGED" : "unplugged", previous ? "plugged" : "unplugged",
                 (context->HeadphoneAutoMute && plugged) ? " - speakers muted" : ""));
    } else {
        KdPrint(("T2Audio: headphone jack re-read failed: 0x%08X\n", status));
    }
    if (workItem != NULL) {
        IoFreeWorkItem(workItem);
    }
}

// Query what the device itself says about the speaker output, as kaiT2en's
// t2audio_init_dev / t2audio_init_stream_info do: the output stream id, its
// physical format ('pft '), the stream and device output latency ('ltnc') and
// the device output safety offset ('saft'). The physical format decides how
// samples are laid out in the BAR1 ring; MMIO copy stays off unless it was
// read and is supported (Context->SpeakerFormatValid).
static VOID
T2AudioQuerySpeakerStream(_Inout_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    ULONG64 streams[4];
    ULONG streamCount = 0;
    UCHAR formatData[T2AUDIO_BCE_PHYS_FORMAT_SIZE];
    ULONG formatLength = 0;
    T2AUDIO_BCE_PHYS_FORMAT format;
    ULONG64 deviceId = Context->BceSpeakerDeviceId;
    ULONG deviceLatency = 0;
    ULONG streamLatency = 0;
    ULONG safetyOffset = 0;
    int shift;
    NTSTATUS status;

    PAGED_CODE();

    Context->SpeakerStreamId = 0;
    Context->SpeakerFormatValid = FALSE;
    Context->SpeakerSampleShift = 0;
    Context->SpeakerLatencyFrames = 0;
    Context->SpeakerSafetyOffsetFrames = 0;

    status = T2AudioGetOutputStreamList(deviceId, streams, ARRAYSIZE(streams),
                                        &streamCount);
    if (!NT_SUCCESS(status) || streamCount == 0) {
        KdPrint(("T2Audio: speaker 0x%I64X: no output stream (0x%08X, n=%u)\n",
                 deviceId, status, streamCount));
        return;
    }
    Context->SpeakerStreamId = streams[0];

    status = T2AudioGetProperty(deviceId, streams[0], T2AUDIO_PROP_SCOPE_GLOBAL,
                                T2AUDIO_PROP_PHYS_FORMAT, formatData,
                                sizeof(formatData), &formatLength);
    if (!NT_SUCCESS(status) ||
        !T2AudioBceParsePhysicalFormat(formatData, formatLength, &format)) {
        KdPrint(("T2Audio: speaker stream 0x%I64X: 'pft ' unavailable (0x%08X, len=%u)\n",
                 streams[0], status, formatLength));
        return;
    }

    KdPrint(("T2Audio: speaker stream 0x%I64X pft: rate=%u id=0x%08X flags=0x%X "
             "bpp=%u fpp=%u bpf=%u ch=%u bits=%u\n",
             streams[0], format.SampleRate, format.FormatId, format.FormatFlags,
             format.BytesPerPacket, format.FramesPerPacket, format.BytesPerFrame,
             format.ChannelsPerFrame, format.BitsPerChannel));

    // The WaveRT pin is fixed at 6 ch / 48 kHz / 32-bit containers.
    shift = T2AudioBceDeviceSampleShift(&format, 6, 48000);
    if (shift < 0) {
        KdPrint(("T2Audio: speaker stream format unsupported; MMIO copy disabled\n"));
        return;
    }
    Context->SpeakerSampleShift = (ULONG)shift;
    Context->SpeakerFormatValid = TRUE;

    // Latencies are informational (reported through GetHWLatency); a failed
    // query only leaves them at zero.
    (VOID)T2AudioGetU32Property(deviceId, streams[0], T2AUDIO_PROP_SCOPE_GLOBAL,
                                T2AUDIO_PROP_LATENCY, &streamLatency);
    (VOID)T2AudioGetU32Property(deviceId, deviceId, T2AUDIO_PROP_SCOPE_OUTPUT,
                                T2AUDIO_PROP_LATENCY, &deviceLatency);
    (VOID)T2AudioGetU32Property(deviceId, deviceId, T2AUDIO_PROP_SCOPE_OUTPUT,
                                T2AUDIO_PROP_SAFETY_OFFSET, &safetyOffset);
    Context->SpeakerLatencyFrames = streamLatency + deviceLatency;
    Context->SpeakerSafetyOffsetFrames = safetyOffset;

    KdPrint(("T2Audio: speaker sample shift=%u latency=%u+%u frames safety=%u frames\n",
             Context->SpeakerSampleShift, streamLatency, deviceLatency, safetyOffset));
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
    // header->Tag is stamped by T2AudioSendBceMessage.
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
// response (message 33) echoing device_id 0.
//
// Currently unused: on Windows AppleUSBVHCI sends SET_REMOTE_ACCESS itself
// (mode 1 at init/resume, mode 0 at power-down) and AppleAudio.sys never
// does, so the audio driver must not change it. Kept for diagnostics.
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

    // header->Tag is stamped by T2AudioSendBceMessage.
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
                                        T2AUDIO_MSG_SET_REMOTE_ACCESS_RESPONSE)) {
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

// Handle one message bridgeOS pushed to the host (IOCTL 0x222020, see
// T2AudioDeviceControl in Driver.c). AppleAudio.sys does the same in its own
// IRP_MJ_DEVICE_CONTROL handler: UPDATE_TIMESTAMP (4) stores the timestamp per
// device, PROPERTY_CHANGED (13) updates jack state; nothing is sent back. This
// runs in AppleUSBVHCI's context while it waits for the IRP, so it must not
// block or send BCE messages.
VOID
T2AudioHandleVhciMessage(
    _Inout_opt_ PT2AUDIO_DEVICE_CONTEXT Context,
    _In_reads_bytes_opt_(MessageSize) const UCHAR *Message,
    _In_ ULONG MessageSize)
{
    ULONG type = 0;
    ULONG msg = 0;
    ULONG64 deviceId = 0;
    LONG count;

    if (Context == NULL || Message == NULL ||
        !T2AudioBceParseIncoming(Message, MessageSize, &type, &msg, &deviceId)) {
        KdPrint(("T2Audio: VHCI message ignored (size=%u)\n", MessageSize));
        return;
    }

    count = InterlockedIncrement(&Context->VhciMessageCount);

    if (msg == T2AUDIO_MSG_UPDATE_TIMESTAMP) {
        ULONG64 timestamp, seed, sampleBits, frames = 0;
        LONG n;

        if (!T2AudioBceParseUpdateTimestamp(Message, MessageSize, &deviceId,
                                            &timestamp, &seed, &sampleBits) ||
            !T2AudioBceDecodeSampleTime(sampleBits, &frames)) {
            KdPrint(("T2Audio: VHCI UPDATE_TIMESTAMP invalid (size=%u)\n", MessageSize));
            return;
        }
        if (deviceId == Context->BceSpeakerDeviceId && deviceId != 0) {
            LARGE_INTEGER qpc, qpcFreq;
            ULONG64 anchorFrames;
            ULONG64 receptionNs;
            ULONG64 hostNs;
            KIRQL oldIrql;

            // hostTime is on the host clock AppleUSBVHCI publishes to the T2
            // (QPC in ns), so the delivery delay can be added back; an
            // implausible delta falls back to "current" (see
            // T2AudioClockFramesAtReception).
            qpc = KeQueryPerformanceCounter(&qpcFreq);
            receptionNs = T2AudioQpcToNs((ULONG64)qpc.QuadPart, (ULONG64)qpcFreq.QuadPart);
            // After ~25.6 h of uptime VHCI's hostTime has wrapped; undo it so
            // the delivery delay still counts (the first timestamp after
            // START_IO arrives ~13 ms late, which otherwise costs a second
            // re-aim and an audible gap). The raw value is still logged.
            hostNs = T2AudioClockUnwrapHostTime(timestamp, receptionNs,
                                                (ULONG64)qpcFreq.QuadPart);
            anchorFrames = T2AudioClockFramesAtReception(frames, hostNs,
                                                         receptionNs, 48000);

            KeAcquireSpinLock(&Context->ClockLock, &oldIrql);
            Context->LastT2Timestamp = timestamp;
            Context->LastT2SampleFrames = frames;
            Context->LastT2UpdateSeed = seed;
            Context->ClockAnchorFrames = anchorFrames;
            Context->ClockAnchorQpc = (ULONG64)qpc.QuadPart;
            Context->ClockAnchorNs = T2AudioClockAnchorNs(hostNs, receptionNs);
            Context->ClockGeneration++;
            KeReleaseSpinLock(&Context->ClockLock, oldIrql);
        }
        n = InterlockedIncrement(&Context->VhciTimestampCount);
        // These arrive continuously while I/O runs: log the first few, then
        // one in a thousand.
        if (n <= 8 || (n % 1000) == 0) {
            KdPrint(("T2Audio: VHCI UPDATE_TIMESTAMP #%d dev=0x%I64X t2=%I64u seed=%I64u sample=%I64u\n",
                     n, deviceId, timestamp, seed, frames));
        }
        return;
    }

    if (msg == T2AUDIO_MSG_PROPERTY_CHANGED) {
        ULONG64 changedDevice = 0, object = 0;
        ULONG element = 0, scope = 0, selector = 0;
        LONG events;

        if (!T2AudioBceParsePropertyChanged(Message, MessageSize, &changedDevice, &object,
                                            &element, &scope, &selector)) {
            KdPrint(("T2Audio: VHCI PROPERTY_CHANGED invalid (size=%u)\n", MessageSize));
            return;
        }
        events = InterlockedIncrement(&Context->JackEventCount);
        if (events <= 64) {
            KdPrint(("T2Audio: VHCI PROPERTY_CHANGED #%d dev=0x%I64X obj=0x%I64X scope=0x%08X sel=0x%08X el=%u\n",
                     events, changedDevice, object, scope, selector, element));
        }
        // Re-read the jack at PASSIVE: this handler runs while AppleUSBVHCI
        // waits for the IRP and must not send a BCE request itself.
        if (Context->JackEvents && Context->DeviceObject != NULL &&
            changedDevice == Context->BceHeadphoneDeviceId && changedDevice != 0 &&
            scope == T2AUDIO_PROP_SCOPE_OUTPUT && selector == T2AUDIO_PROP_JACK_PLUGGED &&
            InterlockedCompareExchange(&Context->JackWorkPending, 1, 0) == 0) {
            PIO_WORKITEM workItem = IoAllocateWorkItem(Context->DeviceObject);

            if (workItem != NULL) {
                IoQueueWorkItem(workItem, T2AudioJackWorkRoutine, DelayedWorkQueue, workItem);
            } else {
                InterlockedExchange(&Context->JackWorkPending, 0);
            }
        }
        return;
    }

    if (count <= 64) {
        KdPrint(("T2Audio: VHCI message #%d type=%u msg=%u dev=0x%I64X size=%u\n",
                 count, type, msg, deviceId, MessageSize));
    }
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
        if (Context->ExtendedProbe) {
            T2AudioLogDeviceDetail(deviceList[i]);
        }

        if (Context->BceSpeakerDeviceId == 0 &&
            T2AudioBceUidIsSpeaker(uid, uidLength)) {
            Context->BceSpeakerDeviceId = deviceList[i];
        }
        // Headphone jack, read-only first step (KAIT2EN reads the same
        // property: 'outp'/'jack' on device "Codec Output", u32 plugged).
        if (Context->BceHeadphoneDeviceId == 0 &&
            T2AudioBceUidEquals(uid, uidLength, "Codec Output")) {
            ULONG plugged = 0;
            NTSTATUS jackStatus;

            Context->BceHeadphoneDeviceId = deviceList[i];
            jackStatus = T2AudioGetU32Property(deviceList[i], deviceList[i],
                                               T2AUDIO_PROP_SCOPE_OUTPUT,
                                               T2AUDIO_PROP_JACK_PLUGGED, &plugged);
            InterlockedExchange(&Context->HeadphoneJackPlugged,
                                (NT_SUCCESS(jackStatus) && plugged != 0) ? 1 : 0);
            KdPrint(("T2Audio: BCE probe: headphone jack device 0x%I64X: %s (status 0x%08X)\n",
                     deviceList[i],
                     !NT_SUCCESS(jackStatus) ? "unknown" : (plugged ? "PLUGGED" : "unplugged"),
                     jackStatus));
        }
    }

    if (Context->BceSpeakerDeviceId != 0) {
        T2AudioQuerySpeakerStream(Context);
    }
    if (Context->JackEvents && Context->BceHeadphoneDeviceId != 0) {
        (VOID)T2AudioRegisterJackListener(Context);
    }

    Context->BceProbed = TRUE;
    KdPrint(("T2Audio: BCE probe done; speaker id 0x%I64X stream 0x%I64X format=%u\n",
             Context->BceSpeakerDeviceId, Context->SpeakerStreamId,
             Context->SpeakerFormatValid ? 1u : 0u));

    // Transport intentionally left open for START_IO/STOP_IO.
    return STATUS_SUCCESS;
}
