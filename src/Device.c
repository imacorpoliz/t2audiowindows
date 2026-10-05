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
    ULONG memoryResourceCount;
    ULONG i;
    NTSTATUS status;

    PAGED_CODE();
    
    KdPrint(("T2Audio: MapResources entry\n"));
    
    if (ResourceList == NULL) {
        KdPrint(("T2Audio: MapResources FAIL_A: ResourceList is NULL\n"));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    
    memoryResourceCount = ResourceList->NumberOfEntriesOfType(
            CmResourceTypeMemory);
    KdPrint(("T2Audio: Memory resource count: %u\n", memoryResourceCount));
    
    // Log all memory resources for BAR correspondence analysis
    for (i = 0; i < memoryResourceCount && i < 6; ++i) {
        descriptor = ResourceList->FindTranslatedEntry(
            CmResourceTypeMemory, i);
        if (descriptor != NULL && descriptor->Type == CmResourceTypeMemory) {
            KdPrint(("T2Audio: Resource[%u] Translated: Phys=0x%I64X Len=0x%IX Flags=0x%04X\n",
                     i, descriptor->u.Memory.Start.QuadPart, 
                     descriptor->u.Memory.Length, descriptor->Flags));
        }
    }
    
    if (memoryResourceCount < 2) {
        KdPrint(("T2Audio: MapResources FAIL_B: Insufficient memory resources (need 2, got %u)\n", 
                 memoryResourceCount));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    descriptor = ResourceList->FindTranslatedEntry(
        CmResourceTypeMemory, 0);
    if (descriptor == NULL) {
        KdPrint(("T2Audio: MapResources FAIL_C: Memory resource 0 descriptor is NULL\n"));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (descriptor->Type != CmResourceTypeMemory) {
        KdPrint(("T2Audio: MapResources FAIL_D: Resource 0 type is %u (expected %u)\n",
                 descriptor->Type, CmResourceTypeMemory));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    
    Context->Bar1Size = descriptor->u.Memory.Length;
    Context->Bar1Physical = descriptor->u.Memory.Start;
    KdPrint(("T2Audio: Using Resource[0] as buffer memory (kaiT2en BAR0)\n"));
    KdPrint(("T2Audio: Resource[0] Physical=0x%I64X Length=0x%IX\n",
             Context->Bar1Physical.QuadPart, Context->Bar1Size));
    
    Context->Bar1Mapped = MmMapIoSpaceEx(descriptor->u.Memory.Start,
                                         Context->Bar1Size,
                                         PAGE_READONLY | PAGE_WRITECOMBINE);
    if (Context->Bar1Mapped == NULL) {
        KdPrint(("T2Audio: MapResources FAIL_E: MmMapIoSpaceEx failed for Resource[0]\n"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    KdPrint(("T2Audio: Resource[0] mapped at %p\n", Context->Bar1Mapped));

    // TEST: Try Resource[2] as config memory (64 KB at 0xC1670000)
    // Hypothesis: Resource[2] might be actual config BAR, not Resource[1]
    // Resource[2] is physically between buffers and Resource[1]
    
    if (memoryResourceCount >= 3) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR testDescriptor;
        testDescriptor = ResourceList->FindTranslatedEntry(
            CmResourceTypeMemory, 2);
        
        if (testDescriptor != NULL && testDescriptor->Type == CmResourceTypeMemory) {
            SIZE_T testSize = testDescriptor->u.Memory.Length;
            KdPrint(("T2Audio: Testing Resource[2] as config memory\n"));
            KdPrint(("T2Audio: Resource[2] Physical=0x%I64X Length=0x%IX\n",
                     testDescriptor->u.Memory.Start.QuadPart, testSize));
            
            if (testSize > T2AUDIO_GPR_OFFSET + sizeof(ULONG) * 3) {
                PVOID testMapped = MmMapIoSpaceEx(testDescriptor->u.Memory.Start,
                                                   testSize,
                                                   PAGE_READONLY | PAGE_NOCACHE);
                if (testMapped != NULL) {
                    PUCHAR testGpr = (PUCHAR)testMapped + T2AUDIO_GPR_OFFSET;
                    ULONG testVersion = READ_REGISTER_ULONG((PULONG)(testGpr + 0));
                    ULONG testSignature = READ_REGISTER_ULONG((PULONG)(testGpr + 4));
                    ULONG testBufferOffset = READ_REGISTER_ULONG((PULONG)(testGpr + 8));
                    
                    KdPrint(("T2Audio: Resource[2] GPR test: version=0x%08X signature=0x%08X bufferOffset=0x%08X\n",
                             testVersion, testSignature, testBufferOffset));
                    
                    MmUnmapIoSpace(testMapped, testSize);
                    
                    if (testSignature == T2AUDIO_SIG) {
                        KdPrint(("T2Audio: FOUND valid signature in Resource[2]! Using Resource[2] as config.\n"));
                        // Switch to Resource[2] for actual mapping
                        descriptor = testDescriptor;
                        goto MapConfigResource;
                    }
                }
            }
        }
    }
    
    // If Resource[2] test failed, fall back to Resource[1]
    KdPrint(("T2Audio: Resource[2] test failed or unavailable, trying Resource[1]\n"));
    
    descriptor = ResourceList->FindTranslatedEntry(
        CmResourceTypeMemory, 1);
    if (descriptor == NULL) {
        KdPrint(("T2Audio: MapResources FAIL_F: Memory resource 1 descriptor is NULL\n"));
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto Exit;
    }
    if (descriptor->Type != CmResourceTypeMemory) {
        KdPrint(("T2Audio: MapResources FAIL_G: Resource 1 type is %u (expected %u)\n",
                 descriptor->Type, CmResourceTypeMemory));
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto Exit;
    }

MapConfigResource:
    Context->Bar2Size = descriptor->u.Memory.Length;
    KdPrint(("T2Audio: Using config memory: Physical=0x%I64X Length=0x%IX\n",
             descriptor->u.Memory.Start.QuadPart, Context->Bar2Size));
    
    if (Context->Bar2Size <= T2AUDIO_GPR_OFFSET + sizeof(ULONG) * 3) {
        KdPrint(("T2Audio: MapResources FAIL_H: BAR2 size 0x%IX too small (need 0x%IX)\n",
                 Context->Bar2Size, T2AUDIO_GPR_OFFSET + sizeof(ULONG) * 3));
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto Exit;
    }
    
    Context->Bar2Mapped = MmMapIoSpaceEx(descriptor->u.Memory.Start,
                                         Context->Bar2Size,
                                         PAGE_READONLY | PAGE_NOCACHE);
    if (Context->Bar2Mapped == NULL) {
        KdPrint(("T2Audio: MapResources FAIL_I: MmMapIoSpaceEx failed for BAR2\n"));
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }
    KdPrint(("T2Audio: BAR2 mapped at %p\n", Context->Bar2Mapped));

    gpr = (PUCHAR)Context->Bar2Mapped + T2AUDIO_GPR_OFFSET;
    version = READ_REGISTER_ULONG((PULONG)(gpr + 0));
    signature = READ_REGISTER_ULONG((PULONG)(gpr + 4));
    bufferOffset = READ_REGISTER_ULONG((PULONG)(gpr + 8));
    
    KdPrint(("T2Audio: GPR read: version=0x%08X signature=0x%08X bufferOffset=0x%08X\n",
             version, signature, bufferOffset));
    
    if (version < 2) {
        KdPrint(("T2Audio: MapResources FAIL_J: GPR version %u < 2\n", version));
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto Exit;
    }
    if (signature != T2AUDIO_SIG) {
        KdPrint(("T2Audio: MapResources FAIL_K: GPR signature 0x%08X != 0x%08X\n",
                 signature, T2AUDIO_SIG));
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto Exit;
    }
    if (bufferOffset > Context->Bar1Size) {
        KdPrint(("T2Audio: MapResources FAIL_L: bufferOffset 0x%X > Bar1Size 0x%IX\n",
                 bufferOffset, Context->Bar1Size));
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto Exit;
    }
    if (Context->Bar1Size - bufferOffset < sizeof(ULONG) * 4) {
        KdPrint(("T2Audio: MapResources FAIL_M: Remaining space 0x%IX < minimum 0x%IX\n",
                 Context->Bar1Size - bufferOffset, sizeof(ULONG) * 4));
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto Exit;
    }

    Context->BufferStruct = (T2AUDIO_BUFFER_STRUCT *)
        ((PUCHAR)Context->Bar1Mapped + bufferOffset);
    KdPrint(("T2Audio: BufferStruct at offset 0x%X\n", bufferOffset));
    
    status = T2AudioFindSpeakerBuffer(Context->BufferStruct,
                                      Context->Bar1Size,
                                      &Context->SpeakerBufferOffset,
                                      &Context->SpeakerBufferSize);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: MapResources FAIL_N: FindSpeakerBuffer returned 0x%08X\n", status));
        goto Exit;
    }

    // The current BufferStruct contract has no device-id field. Commands stay
    // disabled until the BCE device enumeration is connected to this context.
    Context->SpeakerDeviceId = 0;
    Context->HardwareReady = TRUE;
    KdPrint(("T2Audio: MapResources SUCCESS: buffer=0x%Ix size=0x%Ix\n",
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

    KdPrint(("T2Audio: FindSpeakerBuffer entry\n"));
    
    if (BufferStruct == NULL) {
        KdPrint(("T2Audio: FindSpeaker FAIL_A: BufferStruct is NULL\n"));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (BufferOffset == NULL || BufferSize == NULL) {
        KdPrint(("T2Audio: FindSpeaker FAIL_B: Output pointers NULL\n"));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    
    KdPrint(("T2Audio: BufferStruct: Signature=0x%08X Version=%u NumDevices=%u\n",
             BufferStruct->Signature, BufferStruct->Version, BufferStruct->NumDevices));
    
    if (BufferStruct->Signature != T2AUDIO_SIG) {
        KdPrint(("T2Audio: FindSpeaker FAIL_C: Signature mismatch (expected 0x%08X)\n",
                 T2AUDIO_SIG));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (BufferStruct->Version < 2) {
        KdPrint(("T2Audio: FindSpeaker FAIL_D: Version %u < 2\n",
                 BufferStruct->Version));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (BufferStruct->NumDevices > T2AUDIO_MAX_DEVICES) {
        KdPrint(("T2Audio: FindSpeaker FAIL_E: NumDevices %u > max %u\n",
                 BufferStruct->NumDevices, T2AUDIO_MAX_DEVICES));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    for (index = 0; index < BufferStruct->NumDevices; ++index) {
        const T2AUDIO_DEVICE_METADATA *device = &BufferStruct->Devices[index];
        const T2AUDIO_STREAM_METADATA *stream;
        SIZE_T nameMatch;
        
        nameMatch = RtlCompareMemory(device->Name, "Speaker", 7);
        KdPrint(("T2Audio: Device[%u]: Name='%.16s' (match=%Iu) NumOut=%u\n",
                 index, device->Name, nameMatch, device->NumOutputStreams));
        
        if (nameMatch != 7 || device->Name[7] != '\0') {
            continue;
        }
        if (device->NumOutputStreams == 0) {
            KdPrint(("T2Audio: FindSpeaker FAIL_F: Speaker found but NumOutputStreams=0\n"));
            continue;
        }
        if (device->NumOutputStreams > T2AUDIO_MAX_STREAMS) {
            KdPrint(("T2Audio: FindSpeaker FAIL_G: NumOutputStreams %u > max %u\n",
                     device->NumOutputStreams, T2AUDIO_MAX_STREAMS));
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        
        stream = &device->OutputStreams[0];
        KdPrint(("T2Audio: Speaker stream[0]: NumBuffers=%u\n", stream->NumBuffers));
        
        if (stream->NumBuffers == 0) {
            KdPrint(("T2Audio: FindSpeaker FAIL_H: Speaker stream has no buffers\n"));
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        if (stream->NumBuffers > T2AUDIO_MAX_BUFFERS) {
            KdPrint(("T2Audio: FindSpeaker FAIL_I: NumBuffers %u > max %u\n",
                     stream->NumBuffers, T2AUDIO_MAX_BUFFERS));
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        
        *BufferOffset = stream->Buffers[0].Address;
        *BufferSize = stream->Buffers[0].Size;
        KdPrint(("T2Audio: Speaker buffer[0]: Address=0x%Ix Size=0x%Ix\n",
                 *BufferOffset, *BufferSize));
        
        if (*BufferOffset > Bar1Size) {
            KdPrint(("T2Audio: FindSpeaker FAIL_J: BufferOffset 0x%Ix > Bar1Size 0x%Ix\n",
                     *BufferOffset, Bar1Size));
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        if (*BufferSize > Bar1Size - *BufferOffset) {
            KdPrint(("T2Audio: FindSpeaker FAIL_K: BufferSize 0x%Ix exceeds remaining space 0x%Ix\n",
                     *BufferSize, Bar1Size - *BufferOffset));
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        
        KdPrint(("T2Audio: FindSpeaker SUCCESS: buffer located\n"));
        return STATUS_SUCCESS;
    }
    
    KdPrint(("T2Audio: FindSpeaker FAIL_L: Speaker device not found among %u devices\n",
             BufferStruct->NumDevices));
    return STATUS_OBJECT_NAME_NOT_FOUND;
}
