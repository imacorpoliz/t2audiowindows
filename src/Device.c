#include "T2AudioMiniport.h"

VOID
T2AudioUnmapResources(_Inout_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    PAGED_CODE();
    T2AudioFreeSpeakerMdl(Context);
    if (Context->Bar1Mapped != NULL) {
        MmUnmapIoSpace(Context->Bar1Mapped, Context->Bar1Size);
    }
    if (Context->Bar2Mapped != NULL) {
        MmUnmapIoSpace(Context->Bar2Mapped, Context->Bar2Size);
    }
    Context->Bar1Mapped = NULL;
    Context->Bar2Mapped = NULL;
    Context->Bar1Size = 0;
    Context->Bar2Size = 0;
    Context->BufferStruct = NULL;
    Context->HardwareReady = FALSE;
}

NTSTATUS
T2AudioMapResources(
    _Inout_ PT2AUDIO_DEVICE_CONTEXT Context,
    _In_ PRESOURCELIST ResourceList)
{
    PCM_PARTIAL_RESOURCE_DESCRIPTOR descriptor;
    PUCHAR gpr;
    ULONG version;
    ULONG signature;
    ULONG bufferOffset;
    NTSTATUS status;

    PAGED_CODE();
    if (ResourceList == NULL) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (ResourceList->lpVtbl->NumberOfEntriesOfType(
            (INTERFACE *)ResourceList, CmResourceTypeMemory) < 2) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    descriptor = ResourceList->lpVtbl->FindTranslatedEntry(
        (INTERFACE *)ResourceList, CmResourceTypeMemory, 0);
    if (descriptor == NULL || descriptor->Type != CmResourceTypeMemory) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    Context->Bar1Size = descriptor->u.Memory.Length;
    Context->Bar1Physical = descriptor->u.Memory.Start;
    Context->Bar1Mapped = MmMapIoSpaceEx(descriptor->u.Memory.Start,
                                         Context->Bar1Size,
                                         PAGE_READONLY | PAGE_WRITECOMBINE);
    if (Context->Bar1Mapped == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    descriptor = ResourceList->lpVtbl->FindTranslatedEntry(
        (INTERFACE *)ResourceList, CmResourceTypeMemory, 1);
    if (descriptor == NULL || descriptor->Type != CmResourceTypeMemory) {
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto Exit;
    }
    Context->Bar2Size = descriptor->u.Memory.Length;
    if (Context->Bar2Size <= T2AUDIO_GPR_OFFSET + sizeof(ULONG) * 3) {
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto Exit;
    }
    Context->Bar2Mapped = MmMapIoSpaceEx(descriptor->u.Memory.Start,
                                         Context->Bar2Size,
                                         PAGE_READONLY | PAGE_NOCACHE);
    if (Context->Bar2Mapped == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    gpr = (PUCHAR)Context->Bar2Mapped + T2AUDIO_GPR_OFFSET;
    version = READ_REGISTER_ULONG((PULONG)(gpr + 0));
    signature = READ_REGISTER_ULONG((PULONG)(gpr + 4));
    bufferOffset = READ_REGISTER_ULONG((PULONG)(gpr + 8));
    if (version < 2 || signature != T2AUDIO_SIG ||
        bufferOffset > Context->Bar1Size ||
        Context->Bar1Size - bufferOffset < sizeof(ULONG) * 4) {
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto Exit;
    }

    Context->BufferStruct = (T2AUDIO_BUFFER_STRUCT *)
        ((PUCHAR)Context->Bar1Mapped + bufferOffset);
    status = T2AudioFindSpeakerBuffer(Context->BufferStruct,
                                      Context->Bar1Size,
                                      &Context->SpeakerBufferOffset,
                                      &Context->SpeakerBufferSize);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    // The current BufferStruct contract has no device-id field. Commands stay
    // disabled until the BCE device enumeration is connected to this context.
    Context->SpeakerDeviceId = 0;
    Context->HardwareReady = TRUE;
    KdPrint(("T2Audio: hardware validated; buffer=0x%Ix size=0x%Ix\n",
             Context->SpeakerBufferOffset, Context->SpeakerBufferSize));
    return STATUS_SUCCESS;

Exit:
    T2AudioUnmapResources(Context);
    return status;
}

NTSTATUS
T2AudioFindSpeakerBuffer(
    _In_ const T2AUDIO_BUFFER_STRUCT *BufferStruct,
    _In_ SIZE_T Bar1Size,
    _Out_ SIZE_T *BufferOffset,
    _Out_ SIZE_T *BufferSize)
{
    ULONG index;

    if (BufferStruct == NULL || BufferOffset == NULL || BufferSize == NULL ||
        BufferStruct->Signature != T2AUDIO_SIG || BufferStruct->Version < 2 ||
        BufferStruct->NumDevices > T2AUDIO_MAX_DEVICES) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    for (index = 0; index < BufferStruct->NumDevices; ++index) {
        const T2AUDIO_DEVICE_METADATA *device = &BufferStruct->Devices[index];
        const T2AUDIO_STREAM_METADATA *stream;
        if (RtlCompareMemory(device->Name, "Speaker", 7) != 7 ||
            device->Name[7] != '\0' || device->NumOutputStreams == 0 ||
            device->NumOutputStreams > T2AUDIO_MAX_STREAMS) {
            continue;
        }
        stream = &device->OutputStreams[0];
        if (stream->NumBuffers == 0 || stream->NumBuffers > T2AUDIO_MAX_BUFFERS) {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        *BufferOffset = stream->Buffers[0].Address;
        *BufferSize = stream->Buffers[0].Size;
        if (*BufferOffset > Bar1Size || *BufferSize > Bar1Size - *BufferOffset) {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        return STATUS_SUCCESS;
    }
    return STATUS_OBJECT_NAME_NOT_FOUND;
}
