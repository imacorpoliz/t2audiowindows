#include "T2AudioMiniport.h"
#include "T2AudioBufferLogic.h"

VOID
T2AudioUnmapResources(_Inout_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    PVOID bar1;
    SIZE_T bar1Size;
    PVOID bar2;
    SIZE_T bar2Size;

    PAGED_CODE();

    if (Context == NULL) {
        return;
    }

    // Stop all further device access before tearing anything down. HardwareReady
    // gates the copy DPC and the I/O paths, so clearing it first stops a tick
    // from starting a write into a mapping that is about to disappear. This
    // function is idempotent: a second call finds everything already cleared.
    Context->HardwareReady = FALSE;

    // Backstop against a copy DPC that is already executing past its
    // HardwareReady check: drain queued DPCs on all processors before the
    // mapping pointers below are cleared and unmapped. In the normal PnP flow no
    // stream (and therefore no copy DPC) exists while this runs - PortCls tears
    // streams down before the device is stopped - so this only guards the
    // invariant should that ordering ever be broken.
    KeFlushQueuedDpcs();

    T2AudioCloseBceTransport();
    T2AudioFreeSpeakerMdl(Context);

    // Snapshot then clear the mappings before unmapping, so a concurrent reader
    // that re-checks the context sees NULL rather than a freed pointer.
    bar1 = Context->Bar1Mapped;
    bar1Size = Context->Bar1Size;
    bar2 = Context->Bar2Mapped;
    bar2Size = Context->Bar2Size;

    Context->Bar1Mapped = NULL;
    Context->Bar2Mapped = NULL;
    Context->Bar1Size = 0;
    Context->Bar2Size = 0;
    Context->BufferStruct = NULL;
    Context->SpeakerBufferOffset = 0;
    Context->SpeakerBufferSize = 0;

    if (bar1 != NULL) {
        MmUnmapIoSpace(bar1, bar1Size);
    }
    if (bar2 != NULL) {
        MmUnmapIoSpace(bar2, bar2Size);
    }
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
    
    // The T2 audio engine reads PCM straight out of this region, so the copy
    // DPC must be able to write it. Mapping it PAGE_READONLY made the first
    // RtlCopyMemory in T2AudioStreamCopyTick fault with
    // 0xBE ATTEMPTED_WRITE_TO_READONLY_MEMORY. PAGE_NOCACHE keeps the writes
    // immediately visible to the engine (no write-combining buffers to flush).
    Context->Bar1Mapped = MmMapIoSpaceEx(descriptor->u.Memory.Start,
                                         Context->Bar1Size,
                                         PAGE_READWRITE | PAGE_NOCACHE);
    if (Context->Bar1Mapped == NULL) {
        KdPrint(("T2Audio: MapResources FAIL_E: MmMapIoSpaceEx failed for Resource[0]\n"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    KdPrint(("T2Audio: Resource[0] mapped at %p\n", Context->Bar1Mapped));

    // Log every memory resource with both its translated and raw (BAR) address.
    // Windows does not guarantee that the translated resource order matches PCI
    // BAR order (the Touch ID transport hit exactly this on the same T2), so the
    // raw addresses are recorded for cross-checking against !pci.
    for (i = 0; i < memoryResourceCount; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR translated;
        PCM_PARTIAL_RESOURCE_DESCRIPTOR raw;

        translated = ResourceList->FindTranslatedEntry(CmResourceTypeMemory, i);
        raw = ResourceList->FindUntranslatedEntry(CmResourceTypeMemory, i);
        if (translated != NULL && translated->Type == CmResourceTypeMemory) {
            KdPrint(("T2Audio: Resource[%u] translated: Phys=0x%I64X Len=0x%IX Flags=0x%04X\n",
                     i, translated->u.Memory.Start.QuadPart,
                     translated->u.Memory.Length, translated->Flags));
        }
        if (raw != NULL && raw->Type == CmResourceTypeMemory) {
            KdPrint(("T2Audio: Resource[%u] raw(BAR): Phys=0x%I64X Len=0x%IX Flags=0x%04X\n",
                     i, raw->u.Memory.Start.QuadPart,
                     raw->u.Memory.Length, raw->Flags));
        }
    }

    // Locate the config BAR by content rather than by index: the T2 audio config
    // window exposes its GPR block at T2AUDIO_GPR_OFFSET carrying the
    // driver/version signature. Probe every memory resource and use the first
    // one that reports the signature. The previous hard-coded Resource[2]/[1]
    // order silently failed if the enumeration order ever changed.
    descriptor = NULL;
    for (i = 0; i < memoryResourceCount; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR candidate;
        PVOID mapped;
        SIZE_T size;

        candidate = ResourceList->FindTranslatedEntry(CmResourceTypeMemory, i);
        if (candidate == NULL || candidate->Type != CmResourceTypeMemory) {
            continue;
        }
        size = candidate->u.Memory.Length;
        if (size <= T2AUDIO_GPR_OFFSET + sizeof(ULONG) * 3) {
            continue;
        }

        mapped = MmMapIoSpaceEx(candidate->u.Memory.Start, size,
                                PAGE_READONLY | PAGE_NOCACHE);
        if (mapped == NULL) {
            continue;
        }

        gpr = (PUCHAR)mapped + T2AUDIO_GPR_OFFSET;
        version = READ_REGISTER_ULONG((PULONG)(gpr + 0));
        signature = READ_REGISTER_ULONG((PULONG)(gpr + 4));
        bufferOffset = READ_REGISTER_ULONG((PULONG)(gpr + 8));
        MmUnmapIoSpace(mapped, size);

        KdPrint(("T2Audio: Resource[%u] GPR probe: version=0x%08X signature=0x%08X bufferOffset=0x%08X\n",
                 i, version, signature, bufferOffset));

        if (version >= 2 && signature == T2AUDIO_SIG) {
            KdPrint(("T2Audio: Resource[%u] is the config BAR (GPR signature match)\n", i));
            descriptor = candidate;
            break;
        }
    }

    if (descriptor == NULL) {
        KdPrint(("T2Audio: MapResources FAIL_F: no memory resource exposes the GPR signature\n"));
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto Exit;
    }

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

    // FAIL_M only proved the fixed header fits. Before FindSpeakerBuffer walks
    // the table, prove the entire header + NumDevices entries fit inside the
    // mapped BAR, so a corrupt device count cannot make the driver read past
    // the mapping (and fault). FindSpeakerBuffer repeats the count-vs-max check
    // but has no way to know how much of the BAR is actually mapped.
    if (!T2AudioDeviceTableFits(
            Context->BufferStruct->NumDevices,
            T2AUDIO_MAX_DEVICES,
            FIELD_OFFSET(T2AUDIO_BUFFER_STRUCT, Devices),
            sizeof(T2AUDIO_DEVICE_METADATA),
            (unsigned long long)(Context->Bar1Size - bufferOffset))) {
        KdPrint(("T2Audio: MapResources FAIL_M2: device table NumDevices=%u does not fit in 0x%IX bytes\n",
                 Context->BufferStruct->NumDevices,
                 Context->Bar1Size - bufferOffset));
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto Exit;
    }

    status = T2AudioFindSpeakerBuffer(Context->BufferStruct,
                                      Context->Bar1Size,
                                      &Context->SpeakerBufferOffset,
                                      &Context->SpeakerBufferSize);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: MapResources FAIL_N: FindSpeakerBuffer returned 0x%08X\n", status));
        goto Exit;
    }

    // The BufferStruct contract has no device-id field; the speaker device id
    // is discovered later via the BCE device list (T2AudioProbeBceDevices,
    // called from T2AudioStartDevice).
    Context->SpeakerDeviceId = 0;
    Context->BceSpeakerDeviceId = 0;
    Context->BceProbed = FALSE;
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
