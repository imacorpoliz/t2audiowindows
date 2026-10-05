#include "T2AudioMiniport.h"

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
    PMDL mdl;

    if (Context == NULL || Mdl == NULL || OffsetFromFirstPage == NULL ||
        ActualSize == NULL || !Context->HardwareReady ||
        Context->Bar1Mapped == NULL || Context->SpeakerBufferSize == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    // Diagnostic mode: BCE transport is disabled (SpeakerDeviceId == 0).
    // Never hand out a hardware buffer MDL in this mode.
    if (Context->SpeakerDeviceId == 0) {
        *Mdl = NULL;
        *OffsetFromFirstPage = 0;
        *ActualSize = 0;
        KdPrint(("T2Audio: CreateSpeakerMdl blocked: diagnostic mode\n"));
        return STATUS_NOT_SUPPORTED;
    }

    offset = (ULONG)(Context->SpeakerBufferOffset & (PAGE_SIZE - 1));
    size = (ULONG)Context->SpeakerBufferSize;
    pageCount = ADDRESS_AND_SIZE_TO_SPAN_PAGES(
        (PVOID)(ULONG_PTR)Context->SpeakerBufferOffset, size);
    mdl = IoAllocateMdl((PVOID)(ULONG_PTR)(Context->SpeakerBufferOffset - offset),
                        pageCount * PAGE_SIZE,
                        FALSE, FALSE, NULL);
    if (mdl == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    mdl->MdlFlags |= MDL_IO_SPACE;
    for (index = 0; index < pageCount; ++index) {
        MmGetMdlPfnArray(mdl)[index] =
            (PFN_NUMBER)((Context->Bar1Physical.QuadPart +
                          Context->SpeakerBufferOffset - offset +
                          index * PAGE_SIZE) >> PAGE_SHIFT);
    }

    Context->SpeakerBufferMdl = mdl;
    *Mdl = mdl;
    *OffsetFromFirstPage = offset;
    *ActualSize = size;
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
    T2AUDIO_MESSAGE_BASE *replyBase;

    if (Context == NULL || !Context->HardwareReady) {
        return STATUS_DEVICE_NOT_READY;
    }

    if (Context->SpeakerDeviceId == 0) {
        KdPrint(("T2Audio: START_IO blocked: no device ID\n"));
        return STATUS_NOT_SUPPORTED;
    }

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

    // Validate response size and structure
    if (replySize < sizeof(T2AUDIO_MESSAGE_HEADER) + sizeof(T2AUDIO_MESSAGE_BASE)) {
        KdPrint(("T2Audio: START_IO response too short: %u bytes\n", replySize));
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    // Check BCE response status
    replyBase = (T2AUDIO_MESSAGE_BASE *)(reply + sizeof(T2AUDIO_MESSAGE_HEADER));
    if (replyBase->Status != 0) {
        KdPrint(("T2Audio: START_IO failed with T2 status: 0x%08X\n", replyBase->Status));
        return STATUS_UNSUCCESSFUL;
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
    T2AUDIO_MESSAGE_BASE *replyBase;

    if (Context == NULL || !Context->HardwareReady) {
        return STATUS_DEVICE_NOT_READY;
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

    // Validate response size and structure
    if (replySize < sizeof(T2AUDIO_MESSAGE_HEADER) + sizeof(T2AUDIO_MESSAGE_BASE)) {
        KdPrint(("T2Audio: STOP_IO response too short: %u bytes\n", replySize));
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    // Check BCE response status
    replyBase = (T2AUDIO_MESSAGE_BASE *)(reply + sizeof(T2AUDIO_MESSAGE_HEADER));
    if (replyBase->Status != 0) {
        KdPrint(("T2Audio: STOP_IO failed with T2 status: 0x%08X\n", replyBase->Status));
        return STATUS_UNSUCCESSFUL;
    }

    KdPrint(("T2Audio: STOP_IO sent to device 0x%I64X\n", Context->SpeakerDeviceId));
    return STATUS_SUCCESS;
}
