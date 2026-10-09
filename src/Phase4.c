#include "T2AudioMiniport.h"
#include "BceProtocolLogic.h"

#pragma pack(push, 1)
typedef struct _T2AUDIO_MESSAGE_HEADER {
    UCHAR Tag[4];
    UCHAR Type;
    ULONG64 DeviceId;
} T2AUDIO_MESSAGE_HEADER;

typedef struct _T2AUDIO_MESSAGE_BASE {
    ULONG Message;
    ULONG Status;
} T2AUDIO_MESSAGE_BASE;
#pragma pack(pop)

NTSTATUS
T2AudioCreateSpeakerMdl(
    _Inout_ PT2AUDIO_DEVICE_CONTEXT Context,
    _Out_ PMDL *Mdl,
    _Out_ PULONG OffsetFromFirstPage,
    _Out_ PULONG ActualSize)
{
    ULONG offset;
    ULONG size;
    ULONG pageCount;
    ULONG index;
    ULONG actual;
    BOOLEAN ioSpace;
    BOOLEAN pagesLocked;
    PMDL mdl;

    if (Context == NULL || Mdl == NULL || OffsetFromFirstPage == NULL ||
        ActualSize == NULL || !Context->HardwareReady ||
        Context->Bar1Mapped == NULL || Context->SpeakerBufferSize == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    // No speaker device id (BCE unavailable): never hand out a hardware buffer
    // MDL. The stream falls back to a system-memory buffer instead.
    if (Context->SpeakerDeviceId == 0) {
        *Mdl = NULL;
        *OffsetFromFirstPage = 0;
        *ActualSize = 0;
        KdPrint(("T2Audio: CreateSpeakerMdl blocked: no speaker device id\n"));
        return STATUS_NOT_SUPPORTED;
    }

    // Only one hardware buffer MDL may be outstanding at a time; a second
    // allocation would overwrite (and leak) the first.
    if (Context->SpeakerBufferMdl != NULL) {
        *Mdl = NULL;
        *OffsetFromFirstPage = 0;
        *ActualSize = 0;
        return STATUS_INVALID_DEVICE_STATE;
    }

    offset = (ULONG)(Context->SpeakerBufferOffset & (PAGE_SIZE - 1));
    size = (ULONG)Context->SpeakerBufferSize;
    pageCount = ADDRESS_AND_SIZE_TO_SPAN_PAGES(
        (PVOID)(ULONG_PTR)Context->SpeakerBufferOffset, size);

    // Diagnostic matrix selected by HKLM ...\Parameters\MdlVariant (DWORD):
    //   0 = baseline: MDL_IO_SPACE, ActualSize == raw size
    //   1 = no MDL_IO_SPACE
    //   2 = MDL_IO_SPACE | MDL_PAGES_LOCKED
    //   3 = no MDL_IO_SPACE | page-align ActualSize down (MDL count matches)
    //   4 = no MDL_IO_SPACE, ActualSize == raw size (cache set in stream)
    actual = size;
    ioSpace = TRUE;
    pagesLocked = FALSE;
    switch (Context->MdlVariant) {
    case 1:
    case 4:
        ioSpace = FALSE;
        break;
    case 2:
        pagesLocked = TRUE;
        break;
    case 3:
        ioSpace = FALSE;
        actual = (size / PAGE_SIZE) * PAGE_SIZE;
        if (actual == 0) {
            actual = size;
        }
        break;
    default:
        break;
    }

    pageCount = ADDRESS_AND_SIZE_TO_SPAN_PAGES(
        (PVOID)(ULONG_PTR)(Context->SpeakerBufferOffset - offset), actual);
    mdl = IoAllocateMdl((PVOID)(ULONG_PTR)(Context->SpeakerBufferOffset - offset),
                        pageCount * PAGE_SIZE,
                        FALSE, FALSE, NULL);
    if (mdl == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (ioSpace) {
        mdl->MdlFlags |= MDL_IO_SPACE;
    }
    if (pagesLocked) {
        mdl->MdlFlags |= MDL_PAGES_LOCKED;
    }
    for (index = 0; index < pageCount; ++index) {
        MmGetMdlPfnArray(mdl)[index] =
            (PFN_NUMBER)((Context->Bar1Physical.QuadPart +
                          Context->SpeakerBufferOffset - offset +
                          index * PAGE_SIZE) >> PAGE_SHIFT);
    }

    KdPrint(("T2Audio: CreateSpeakerMdl variant=%u ioSpace=%u locked=%u "
             "actual=%u pages=%u\n",
             Context->MdlVariant, ioSpace ? 1u : 0u, pagesLocked ? 1u : 0u,
             actual, pageCount));

    Context->SpeakerBufferMdl = mdl;
    *Mdl = mdl;
    *OffsetFromFirstPage = offset;
    *ActualSize = actual;
    return STATUS_SUCCESS;
}

VOID
T2AudioFreeSpeakerMdl(_Inout_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    if (Context != NULL && Context->SpeakerBufferMdl != NULL) {
        IoFreeMdl(Context->SpeakerBufferMdl);
        Context->SpeakerBufferMdl = NULL;
    }
}

NTSTATUS
T2AudioBuildIoCommand(
    _In_ ULONG Message,
    _In_ ULONG64 DeviceId,
    _Out_writes_bytes_(BufferLength) PUCHAR Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ PSIZE_T MessageLength)
{
    T2AUDIO_MESSAGE_HEADER *header;
    T2AUDIO_MESSAGE_BASE *base;

    if (Buffer == NULL || MessageLength == NULL ||
        BufferLength < sizeof(*header) + sizeof(*base)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlZeroMemory(Buffer, sizeof(*header) + sizeof(*base));
    header = (T2AUDIO_MESSAGE_HEADER *)Buffer;
    RtlCopyMemory(header->Tag, "Audt", 4);
    header->Type = 1;
    header->DeviceId = DeviceId;
    base = (T2AUDIO_MESSAGE_BASE *)(Buffer + sizeof(*header));
    base->Message = Message;
    *MessageLength = sizeof(*header) + sizeof(*base);
    return STATUS_SUCCESS;
}

NTSTATUS
T2AudioStartIo(_In_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    UCHAR message[sizeof(T2AUDIO_MESSAGE_HEADER) + sizeof(T2AUDIO_MESSAGE_BASE)];
    UCHAR reply[256];
    SIZE_T length;
    ULONG replySize;
    NTSTATUS status;

    if (Context == NULL || !Context->HardwareReady) {
        return STATUS_DEVICE_NOT_READY;
    }

    KdPrint(("T2Audio: START_IO entry irql=%u speaker=0x%I64X\n",
             (ULONG)KeGetCurrentIrql(), Context->SpeakerDeviceId));
    if (KeGetCurrentIrql() > PASSIVE_LEVEL) {
        KdPrint(("T2Audio: START_IO refused at IRQL %u (needs PASSIVE)\n",
                 (ULONG)KeGetCurrentIrql()));
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (Context->SpeakerDeviceId == 0) {
        KdPrint(("T2Audio: START_IO blocked: no device ID\n"));
        return STATUS_NOT_SUPPORTED;
    }

    // The T2 must be in host-controlled mode first. START_IO without the
    // SET_REMOTE_ACCESS handshake is what the reference driver never does; we
    // refuse rather than risk an undefined firmware/lower-driver state.
    if (!Context->BceRemoteAccess) {
        KdPrint(("T2Audio: START_IO blocked: remote access not acquired\n"));
        return STATUS_DEVICE_NOT_READY;
    }

    // KNOWN BSOD: on this build, sending START_IO has twice produced a 0x7E
    // in ks!DispatchDeviceIoControl through AppleUSBVHCI/ksthunk. Keep this
    // path in the published reproducer; the manual agent refuses -BceIo so it
    // cannot be triggered accidentally through the normal test workflow.
    status = T2AudioBuildIoCommand(0, Context->SpeakerDeviceId,
                                   message, sizeof(message), &length);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = T2AudioSendBceMessage(message, (ULONG)length,
                                   reply, sizeof(reply), &replySize);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: START_IO send failed: 0x%08X\n", status));
        return status;
    }

    if (!T2AudioBceParseCommandResponse(reply, replySize, 0,
                                        Context->SpeakerDeviceId)) {
        KdPrint(("T2Audio: START_IO response invalid (size=%u)\n", replySize));
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    KdPrint(("T2Audio: START_IO sent to device 0x%I64X\n", Context->SpeakerDeviceId));
    return STATUS_SUCCESS;
}

NTSTATUS
T2AudioStopIo(_In_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    UCHAR message[sizeof(T2AUDIO_MESSAGE_HEADER) + sizeof(T2AUDIO_MESSAGE_BASE)];
    UCHAR reply[256];
    SIZE_T length;
    ULONG replySize;
    NTSTATUS status;

    if (Context == NULL || !Context->HardwareReady) {
        return STATUS_DEVICE_NOT_READY;
    }

    KdPrint(("T2Audio: STOP_IO entry irql=%u speaker=0x%I64X\n",
             (ULONG)KeGetCurrentIrql(), Context->SpeakerDeviceId));
    if (KeGetCurrentIrql() > PASSIVE_LEVEL) {
        KdPrint(("T2Audio: STOP_IO refused at IRQL %u (needs PASSIVE)\n",
                 (ULONG)KeGetCurrentIrql()));
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (Context->SpeakerDeviceId == 0) {
        KdPrint(("T2Audio: STOP_IO blocked: no device ID\n"));
        return STATUS_NOT_SUPPORTED;
    }

    status = T2AudioBuildIoCommand(2, Context->SpeakerDeviceId,
                                   message, sizeof(message), &length);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = T2AudioSendBceMessage(message, (ULONG)length,
                                   reply, sizeof(reply), &replySize);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: STOP_IO send failed: 0x%08X\n", status));
        return status;
    }

    // Validate the whole reply: tag, response type, echoed device id, message id
    // and protocol status. A malformed or mismatched reply is a protocol error.
    if (!T2AudioBceParseCommandResponse(reply, replySize, 2,
                                        Context->SpeakerDeviceId)) {
        KdPrint(("T2Audio: STOP_IO response invalid (size=%u)\n", replySize));
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    KdPrint(("T2Audio: STOP_IO sent to device 0x%I64X\n", Context->SpeakerDeviceId));
    return STATUS_SUCCESS;
}
