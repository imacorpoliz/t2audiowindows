# T2 Audio Protocol - Windows Driver Implementation Guide

## Quick Reference: Message Encoding/Decoding Patterns

### Message Write Pattern (protocol.c:147-159)

```c
void t2audio_msg_write_start_io(struct t2audio_msg *msg, u64 device_id) {
    size_t offset = sizeof(struct t2audio_msg_header);
    struct t2audio_msg_header *hdr = msg->data;
    
    // Header
    hdr->type = T2AUDIO_MSG_TYPE_COMMAND;
    hdr->device_id = device_id;
    // hdr->tag already set by send_prepare
    
    // Base
    *(u32*)(msg->data + offset) = T2AUDIO_MSG_START_IO;  // msg
    offset += 4;
    *(u32*)(msg->data + offset) = 0;  // status
    offset += 4;
    
    msg->size = offset;
}
```

### Message Read Pattern (protocol.c:34-50)

```c
int t2audio_msg_read_update_timestamp(struct t2audio_msg *msg,
    u64 *devid, u64 *timestamp, u64 *update_seed, u64 *sample_time) {
    
    size_t offset = sizeof(struct t2audio_msg_header) + 
                    sizeof(struct t2audio_msg_base);
    const u8 *data = msg->data;
    
    // Validate message type
    u32 msg_type = *(u32*)(data + sizeof(struct t2audio_msg_header));
    if (msg_type != T2AUDIO_MSG_UPDATE_TIMESTAMP)
        return -EINVAL;
    
    // Validate size
    if (msg->size < offset + 3 * sizeof(u64))
        return -EINVAL;
    
    // Extract fields (little-endian)
    *devid = *(u64*)(data + offsetof(struct t2audio_msg_header, device_id));
    *timestamp = *(u64*)(data + offset);
    *update_seed = *(u64*)(data + offset + 8);
    *sample_time = *(u64*)(data + offset + 16);
    
    return 0;
}
```

---

## Windows-Specific Data Structures

### Stream Context
```c
typedef struct _T2_STREAM_CONTEXT {
    // Identity
    UINT64 StreamId;
    UINT64 DeviceId;
    BOOLEAN IsInput;
    
    // Format
    T2AUDIO_APPLE_DESCRIPTION Format;
    UINT32 Latency;  // In frames
    
    // DMA Buffer
    PVOID VirtualAddress;
    PHYSICAL_ADDRESS PhysicalAddress;
    SIZE_T BufferSize;
    BOOLEAN IsDeviceMemory;  // TRUE for output (BAR0), FALSE for input (coherent)
    
    // Timing State
    BOOLEAN Started;
    BOOLEAN WaitingForFirstTimestamp;
    BOOLEAN TimelineFault;
    LARGE_INTEGER RemoteTimestamp;  // QPC when T2 sampled position
    LARGE_INTEGER TimestampAcceptAfter;  // Reject older timestamps
    UINT64 LastDeviceTimestamp;  // T2's timestamp (ns)
    UINT64 LastSampleFrames;     // Last reported frame count from T2
    UINT64 SampleFrames;         // Current anchor frame count
    UINT64 ReportedFrames;       // Last frame count reported to OS
    
    // WDF Objects
    WDFSPINLOCK TimingLock;
    WDFREQUEST PendingIoRequest;
    
} T2_STREAM_CONTEXT, *PT2_STREAM_CONTEXT;
```

### Device Context
```c
typedef struct _T2_DEVICE_CONTEXT {
    // Hardware Resources
    PVOID Bar0Virtual;           // Audio buffer space
    PHYSICAL_ADDRESS Bar0Physical;
    SIZE_T Bar0Length;
    PVOID Bar4Virtual;           // Config space
    SIZE_T Bar4Length;
    PUINT32 GprRegisters;        // BAR4 + 0xC000
    
    // BufferStruct
    PT2AUDIO_BUFFER_STRUCT BufferStruct;
    
    // BCE Transport
    T2_BCE_CONTEXT BceContext;
    
    // Streams
    T2_STREAM_CONTEXT OutputStreams[MAX_OUTPUT_STREAMS];
    UINT32 OutputStreamCount;
    T2_STREAM_CONTEXT InputStreams[MAX_INPUT_STREAMS];
    UINT32 InputStreamCount;
    
    // Device State
    BOOLEAN RemoteAccessEnabled;
    BOOLEAN Initialized;
    
    // Synchronization
    WDFSPINLOCK DeviceLock;
    WDFWORKITEM TimestampWorkItem;
    
} T2_DEVICE_CONTEXT, *PT2_DEVICE_CONTEXT;
```

### BCE Queue Context
```c
typedef struct _T2_BCE_QUEUE {
    PVOID Data;                  // DMA buffer for messages
    PHYSICAL_ADDRESS DmaAddress;
    SIZE_T ElementSize;          // 0x1000 (4KB)
    SIZE_T ElementCount;         // 20
    SIZE_T HeadIndex;
    SIZE_T TailIndex;
    // SQ/CQ handles (implementation-specific)
} T2_BCE_QUEUE, *PT2_BCE_QUEUE;

typedef struct _T2_BCE_CONTEXT {
    T2_BCE_QUEUE QueueOut;  // Host → T2
    T2_BCE_QUEUE QueueIn;   // T2 → Host
    
    UINT32 TagNumber;  // 0-999
    PT2_PENDING_COMMAND PendingCommands[1000];
    
    WDFSPINLOCK QueueLock;
} T2_BCE_CONTEXT, *PT2_BCE_CONTEXT;

typedef struct _T2_PENDING_COMMAND {
    PVOID ReplyBuffer;
    SIZE_T ReplyBufferSize;
    SIZE_T BytesReturned;
    KEVENT CompletionEvent;
    NTSTATUS Status;
} T2_PENDING_COMMAND, *PT2_PENDING_COMMAND;
```

---

## Critical Code Patterns

### 1. IEEE-754 Double to Integer Conversion

```c
// Convert sample rate double to integer (e.g., 48000.0 → 48000)
UINT64 T2Audio_DoubleToUInt64(UINT64 DoubleBits) {
    UINT8 sign = (UINT8)((DoubleBits >> 63) & 1);
    INT32 exponent = (INT32)((DoubleBits >> 52) & 0x7FF) - 1023;
    UINT64 fraction = DoubleBits & ((1ULL << 52) - 1);
    
    if (sign || exponent < 0)
        return 0;
    
    return (1ULL << exponent) + (fraction >> (52 - exponent));
}

// Decode sample_time (frame count as double)
BOOLEAN T2Audio_DecodeSampleTime(UINT64 DoubleBits, PUINT64 Frames) {
    UINT32 exponent = (DoubleBits >> 52) & 0x7FF;
    UINT64 fraction = DoubleBits & ((1ULL << 52) - 1);
    UINT32 shift;
    
    // Zero is valid
    if (DoubleBits == 0) {
        *Frames = 0;
        return TRUE;
    }
    
    // Reject negative, non-integral, or too-large values
    // Valid range: 0 to 2^53 (doubles represent integers exactly up to this)
    if ((DoubleBits >> 63) ||          // Sign bit set (negative)
        exponent < 1023 ||              // Too small (< 1.0)
        exponent > 1076 ||              // Too large (> 2^53)
        (exponent == 1076 && fraction)) // Exactly 2^53 + fraction
        return FALSE;
    
    // Add implicit leading 1 to mantissa
    fraction |= 1ULL << 52;
    
    // Special case: 2^53 exactly
    if (exponent == 1076) {
        *Frames = fraction << 1;
        return TRUE;
    }
    
    // Shift mantissa to extract integer
    shift = 1075 - exponent;
    
    // Verify it's an integer (no fractional bits)
    if (fraction & ((1ULL << shift) - 1))
        return FALSE;
    
    *Frames = fraction >> shift;
    return TRUE;
}
```

### 2. Position Interpolation

```c
UINT64 T2Audio_GetCurrentPosition(PT2_STREAM_CONTEXT Stream) {
    LARGE_INTEGER now;
    UINT64 elapsedNs, elapsedFrames, frames;
    
    if (!Stream->Started || Stream->WaitingForFirstTimestamp)
        return 0;
    
    if (Stream->TimelineFault)
        return (UINT64)-1;  // Signal XRUN
    
    // Calculate time elapsed since last anchor
    KeQueryPerformanceCounter(&now);
    elapsedNs = T2Audio_QpcDeltaToNanoseconds(Stream->RemoteTimestamp, now);
    
    // Convert to frames at stream rate
    elapsedFrames = (elapsedNs * Stream->Format.SampleRate) / 1000000000ULL;
    
    // Interpolate position
    frames = Stream->SampleFrames + elapsedFrames;
    
    // Apply latency compensation (playback only)
    if (!Stream->IsInput) {
        if (frames > Stream->Latency)
            frames -= Stream->Latency;
        else
            frames = 0;
    }
    
    // Never go backward
    if (frames < Stream->ReportedFrames)
        frames = Stream->ReportedFrames;
    
    Stream->ReportedFrames = frames;
    return frames % (Stream->BufferSize / Stream->Format.BytesPerFrame);
}
```

### 3. Timestamp Processing

```c
VOID T2Audio_HandleTimestamp(
    PT2_STREAM_CONTEXT Stream,
    UINT64 DeviceTimestamp,  // T2's host monotonic time (ns)
    UINT64 UpdateSeed,        // 1=reset, 0=continuation
    UINT64 SampleFrames)      // Frame count from T2
{
    LARGE_INTEGER now, deviceTime;
    UINT64 elapsedNs, elapsedFrames, projectedFrames;
    
    KeAcquireSpinLockAtDpcLevel(&Stream->TimingLock);
    
    // Ignore if stream not started
    if (!Stream->Started)
        goto unlock;
    
    // Reject timestamps from before START_IO
    deviceTime.QuadPart = T2Audio_NanosecondsToQpc(DeviceTimestamp);
    if (deviceTime.QuadPart < Stream->TimestampAcceptAfter.QuadPart)
        goto unlock;
    
    // Detect timeline corruption
    // 1. Seed=1 must have frames=0 (reset)
    // 2. Mid-stream reset (seed=1 after data has flowed)
    // 3. Backward jump
    if ((UpdateSeed && SampleFrames) ||
        (!Stream->WaitingForFirstTimestamp && UpdateSeed && 
         (Stream->ReportedFrames || Stream->SampleFrames)) ||
        (SampleFrames < Stream->LastSampleFrames)) {
        
        Stream->TimelineFault = TRUE;
        KdPrint(("T2Audio: Timeline fault detected: seed=%llu frames=%llu prev=%llu\n",
                 UpdateSeed, SampleFrames, Stream->LastSampleFrames));
        goto unlock;
    }
    
    // Correct for delayed callbacks
    // If timestamp is newer than last, but frame count hasn't progressed
    // as much as the time delta suggests, use time-based projection
    if (!Stream->WaitingForFirstTimestamp &&
        DeviceTimestamp > Stream->LastDeviceTimestamp) {
        
        elapsedNs = DeviceTimestamp - Stream->LastDeviceTimestamp;
        elapsedFrames = (elapsedNs * Stream->Format.SampleRate) / 1000000000ULL;
        projectedFrames = Stream->SampleFrames + elapsedFrames;
        
        // If T2's frame count is behind our projection, use projection
        if (SampleFrames < projectedFrames)
            SampleFrames = projectedFrames;
    }
    
    // Save anchor point
    Stream->LastSampleFrames = SampleFrames;
    Stream->SampleFrames = SampleFrames;
    Stream->LastDeviceTimestamp = DeviceTimestamp;
    Stream->RemoteTimestamp = deviceTime;
    Stream->WaitingForFirstTimestamp = FALSE;
    
    KdPrint(("T2Audio: Anchor: frames=%llu time=%llu\n", 
             SampleFrames, DeviceTimestamp));

unlock:
    KeReleaseSpinLockFromDpcLevel(&Stream->TimingLock);
}
```

### 4. Start Stream Sequence

```c
NTSTATUS T2Audio_StartStream(
    PT2_DEVICE_CONTEXT Device,
    PT2_STREAM_CONTEXT Stream)
{
    NTSTATUS status;
    LARGE_INTEGER now;
    
    KdPrint(("T2Audio: Starting stream %llx\n", Stream->StreamId));
    
    // Reset timing state
    Stream->WaitingForFirstTimestamp = TRUE;
    Stream->RemoteTimestamp.QuadPart = 0;
    Stream->LastDeviceTimestamp = 0;
    Stream->LastSampleFrames = 0;
    Stream->SampleFrames = 0;
    Stream->ReportedFrames = 0;
    Stream->TimelineFault = FALSE;
    
    // Accept timestamps received during and after START_IO
    KeQueryPerformanceCounter(&Stream->TimestampAcceptAfter);
    
    // Ensure any prefilled data is visible to device
    KeMemoryBarrier();
    
    // Mark stream as started (for timestamp handling)
    InterlockedExchange8(&Stream->Started, TRUE);
    
    // Send START_IO command (synchronous)
    status = T2Audio_SendCommand(
        Device,
        T2AUDIO_MSG_START_IO,
        Stream->DeviceId,
        NULL, 0,  // No payload
        NULL, 0,  // No response data expected
        5000);    // 5 second timeout
    
    if (!NT_SUCCESS(status)) {
        InterlockedExchange8(&Stream->Started, FALSE);
        KdPrint(("T2Audio: START_IO failed: %08x\n", status));
        return status;
    }
    
    KdPrint(("T2Audio: Stream started successfully\n"));
    return STATUS_SUCCESS;
}
```

### 5. Stop Stream Sequence

```c
NTSTATUS T2Audio_StopStream(
    PT2_DEVICE_CONTEXT Device,
    PT2_STREAM_CONTEXT Stream)
{
    NTSTATUS status;
    
    KdPrint(("T2Audio: Stopping stream %llx\n", Stream->StreamId));
    
    // Mark stream as stopped (reject new timestamps)
    InterlockedExchange8(&Stream->Started, FALSE);
    
    // Send STOP_IO command
    status = T2Audio_SendCommand(
        Device,
        T2AUDIO_MSG_STOP_IO,
        Stream->DeviceId,
        NULL, 0,
        NULL, 0,
        5000);
    
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: STOP_IO failed: %08x\n", status));
        // Continue anyway - stream is marked stopped
    }
    
    // Reset timing
    Stream->RemoteTimestamp.QuadPart = 0;
    Stream->WaitingForFirstTimestamp = TRUE;
    Stream->TimestampAcceptAfter.QuadPart = 0;
    
    return status;
}
```

### 6. BufferStruct Discovery

```c
NTSTATUS T2Audio_MapBufferStruct(PT2_DEVICE_CONTEXT Device) {
    UINT32 version, signature, offset;
    
    // Read GPR registers at BAR4 + 0xC000
    Device->GprRegisters = (PUINT32)((PUCHAR)Device->Bar4Virtual + 0xC000);
    
    version = READ_REGISTER_ULONG(&Device->GprRegisters[0]);
    signature = READ_REGISTER_ULONG(&Device->GprRegisters[1]);
    offset = READ_REGISTER_ULONG(&Device->GprRegisters[2]);
    
    KdPrint(("T2Audio: GPR version=%u sig=%08x offset=%08x\n",
             version, signature, offset));
    
    // Validate
    if (version < 3) {
        KdPrint(("T2Audio: Unsupported BufferStruct version %u\n", version));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    
    if (signature != T2AUDIO_SIG) {
        KdPrint(("T2Audio: Invalid GPR signature %08x\n", signature));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    
    if (offset >= Device->Bar0Length) {
        KdPrint(("T2Audio: BufferStruct offset %u exceeds BAR0 size\n", offset));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    
    // Map BufferStruct
    Device->BufferStruct = (PT2AUDIO_BUFFER_STRUCT)
                          ((PUCHAR)Device->Bar0Virtual + offset);
    
    // Validate BufferStruct signature
    signature = READ_REGISTER_ULONG(&Device->BufferStruct->Signature);
    if (signature != T2AUDIO_SIG) {
        KdPrint(("T2Audio: Invalid BufferStruct signature %08x\n", signature));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    
    KdPrint(("T2Audio: BufferStruct version=%u devices=%u\n",
             Device->BufferStruct->Version,
             Device->BufferStruct->NumDevices));
    
    return STATUS_SUCCESS;
}
```

### 7. Output Buffer Mapping (from BufferStruct)

```c
NTSTATUS T2Audio_MapOutputBuffer(
    PT2_DEVICE_CONTEXT Device,
    PT2_STREAM_CONTEXT Stream,
    PT2AUDIO_BUFFER_STRUCT_BUFFER BsBuffer)
{
    SIZE_T offset;
    
    // Buffer address is offset from BAR0 base
    offset = BsBuffer->Address;
    
    if (offset >= Device->Bar0Length ||
        offset + BsBuffer->Size > Device->Bar0Length) {
        KdPrint(("T2Audio: Buffer address %llx size %llx exceeds BAR0\n",
                 BsBuffer->Address, BsBuffer->Size));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    
    // Map from BAR0 (device memory)
    Stream->VirtualAddress = (PUCHAR)Device->Bar0Virtual + offset;
    Stream->PhysicalAddress.QuadPart = Device->Bar0Physical.QuadPart + offset;
    Stream->BufferSize = BsBuffer->Size;
    Stream->IsDeviceMemory = TRUE;
    
    KdPrint(("T2Audio: Mapped output buffer: PA=%llx VA=%p size=%llx\n",
             Stream->PhysicalAddress.QuadPart,
             Stream->VirtualAddress,
             Stream->BufferSize));
    
    return STATUS_SUCCESS;
}
```

### 8. Input Buffer Allocation (host-owned)

```c
NTSTATUS T2Audio_AllocateInputBuffer(
    PT2_DEVICE_CONTEXT Device,
    PT2_STREAM_CONTEXT Stream,
    UINT8 BufferStructDeviceIndex)
{
    NTSTATUS status;
    SIZE_T bufferSize;
    PVOID virtualAddress;
    PHYSICAL_ADDRESS physicalAddress;
    PT2AUDIO_BUFFER_STRUCT_DEVICE bsDev;
    PT2AUDIO_BUFFER_STRUCT_BUFFER bsBuf;
    
    // Calculate buffer size: period_size * 16640 periods
    bufferSize = Stream->Format.BytesPerPacket * 16640;
    
    KdPrint(("T2Audio: Allocating input buffer: size=%llx\n", bufferSize));
    
    // Allocate DMA coherent memory
    virtualAddress = MmAllocateContiguousMemorySpecifyCache(
        bufferSize,
        (PHYSICAL_ADDRESS){0},
        (PHYSICAL_ADDRESS){0xFFFFFFFFFFFFFFFF},
        (PHYSICAL_ADDRESS){0},
        MmCached);
    
    if (!virtualAddress) {
        KdPrint(("T2Audio: Failed to allocate input buffer\n"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    
    physicalAddress = MmGetPhysicalAddress(virtualAddress);
    
    // Zero the buffer
    RtlZeroMemory(virtualAddress, bufferSize);
    
    // Write buffer info to BufferStruct
    bsDev = &Device->BufferStruct->Devices[BufferStructDeviceIndex];
    bsBuf = &bsDev->InputStreams[0].Buffers[0];
    
    WRITE_REGISTER_BUFFER_ULONG64((PULONG64)&bsBuf->Address,
                                  &physicalAddress.QuadPart, 1);
    WRITE_REGISTER_BUFFER_ULONG64((PULONG64)&bsBuf->Size,
                                  (PULONG64)&bufferSize, 1);
    WRITE_REGISTER_UCHAR(&bsDev->InputStreams[0].NumBuffers, 1);
    
    // Memory barrier to ensure T2 sees updated BufferStruct
    KeMemoryBarrier();
    
    // Notify T2 of new buffer addresses
    status = T2Audio_SendCommand(
        Device,
        T2AUDIO_MSG_SET_INPUT_STREAM_ADDRESS_RANGES,
        Stream->DeviceId,
        NULL, 0,
        NULL, 0,
        5000);
    
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: SET_INPUT_STREAM_ADDRESS_RANGES failed: %08x\n", status));
        MmFreeContiguousMemory(virtualAddress);
        return status;
    }
    
    // Save buffer info
    Stream->VirtualAddress = virtualAddress;
    Stream->PhysicalAddress = physicalAddress;
    Stream->BufferSize = bufferSize;
    Stream->IsDeviceMemory = FALSE;
    
    KdPrint(("T2Audio: Input buffer allocated: PA=%llx VA=%p\n",
             physicalAddress.QuadPart, virtualAddress));
    
    return STATUS_SUCCESS;
}
```

---

## Message Encoding Examples

### GET_PROPERTY Request

```c
// Get device UID string
NTSTATUS T2Audio_GetDeviceUID(
    PT2_DEVICE_CONTEXT Device,
    UINT64 DeviceId,
    PCHAR UidBuffer,
    SIZE_T UidBufferSize)
{
    UINT8 requestBuf[512];
    UINT8 responseBuf[4096];
    SIZE_T offset = 0;
    UINT64 dataSize;
    PCHAR data;
    
    // Build request
    offset += sizeof(T2AUDIO_MSG_HEADER) + sizeof(T2AUDIO_MSG_BASE);
    *(UINT64*)(requestBuf + offset) = DeviceId;  // object_id
    offset += 8;
    *(UINT32*)(requestBuf + offset) = 0;  // element
    offset += 4;
    *(UINT32*)(requestBuf + offset) = 0x676c6f62;  // scope = 'glob'
    offset += 4;
    *(UINT32*)(requestBuf + offset) = 0x75696420;  // selector = 'uid '
    offset += 4;
    *(UINT64*)(requestBuf + offset) = 0;  // qualifier_size = 0
    offset += 8;
    
    // Send command
    status = T2Audio_SendCommand(
        Device,
        T2AUDIO_MSG_GET_PROPERTY,
        DeviceId,
        requestBuf, offset,
        responseBuf, sizeof(responseBuf),
        5000);
    
    if (!NT_SUCCESS(status))
        return status;
    
    // Parse response
    offset = sizeof(T2AUDIO_MSG_HEADER) + sizeof(T2AUDIO_MSG_BASE);
    offset += 8;  // object_id
    offset += 4;  // element
    offset += 4;  // scope
    offset += 4;  // selector
    dataSize = *(UINT64*)(responseBuf + offset);
    offset += 8;
    data = (PCHAR)(responseBuf + offset);
    
    if (dataSize >= UidBufferSize)
        return STATUS_BUFFER_TOO_SMALL;
    
    RtlCopyMemory(UidBuffer, data, (SIZE_T)dataSize);
    UidBuffer[dataSize] = '\0';
    
    return STATUS_SUCCESS;
}
```

---

## QPC ↔ Nanoseconds Conversion

```c
LARGE_INTEGER g_QpcFrequency;

VOID T2Audio_InitializeTimingConversion(VOID) {
    KeQueryPerformanceCounter(&g_QpcFrequency);
}

UINT64 T2Audio_QpcDeltaToNanoseconds(
    LARGE_INTEGER Start,
    LARGE_INTEGER End)
{
    UINT64 delta = (UINT64)(End.QuadPart - Start.QuadPart);
    return (delta * 1000000000ULL) / g_QpcFrequency.QuadPart;
}

LARGE_INTEGER T2Audio_NanosecondsToQpc(UINT64 Nanoseconds) {
    LARGE_INTEGER qpc;
    qpc.QuadPart = (LONGLONG)((Nanoseconds * g_QpcFrequency.QuadPart) / 1000000000ULL);
    return qpc;
}
```

---

## Power Management Handlers

```c
NTSTATUS T2Audio_EvtDeviceD0Entry(
    WDFDEVICE Device,
    WDF_POWER_DEVICE_STATE PreviousState)
{
    PT2_DEVICE_CONTEXT ctx = T2Audio_GetDeviceContext(Device);
    NTSTATUS status;
    
    KdPrint(("T2Audio: D0Entry from state %d\n", PreviousState));
    
    if (PreviousState == WdfPowerDeviceD3Final) {
        // Cold boot - full initialization already done
        return STATUS_SUCCESS;
    }
    
    // Resume from sleep/hibernate
    status = T2Audio_SendCommand(
        ctx,
        T2AUDIO_MSG_SET_REMOTE_ACCESS,
        0,  // Global command
        &(UINT64){T2AUDIO_REMOTE_ACCESS_ON}, sizeof(UINT64),
        NULL, 0,
        5000);
    
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: Failed to enable remote access on resume: %08x\n", status));
        return status;
    }
    
    ctx->RemoteAccessEnabled = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS T2Audio_EvtDeviceD0Exit(
    WDFDEVICE Device,
    WDF_POWER_DEVICE_STATE TargetState)
{
    PT2_DEVICE_CONTEXT ctx = T2Audio_GetDeviceContext(Device);
    UINT32 i;
    
    KdPrint(("T2Audio: D0Exit to state %d\n", TargetState));
    
    // Stop all active streams
    for (i = 0; i < ctx->OutputStreamCount; i++) {
        if (ctx->OutputStreams[i].Started)
            T2Audio_StopStream(ctx, &ctx->OutputStreams[i]);
    }
    for (i = 0; i < ctx->InputStreamCount; i++) {
        if (ctx->InputStreams[i].Started)
            T2Audio_StopStream(ctx, &ctx->InputStreams[i]);
    }
    
    // Disable remote access
    if (ctx->RemoteAccessEnabled) {
        T2Audio_SendCommand(
            ctx,
            T2AUDIO_MSG_SET_REMOTE_ACCESS,
            0,
            &(UINT64){T2AUDIO_REMOTE_ACCESS_OFF}, sizeof(UINT64),
            NULL, 0,
            5000);
        
        ctx->RemoteAccessEnabled = FALSE;
    }
    
    return STATUS_SUCCESS;
}
```

---

## Handling NOTIFICATION_BOOT (T2 Restart)

```c
VOID T2Audio_HandleNotificationBoot(PT2_DEVICE_CONTEXT Device) {
    NTSTATUS status;
    
    KdPrint(("T2Audio: T2 chip has rebooted - reinitializing\n"));
    
    // Resend ALIVE notification
    status = T2Audio_SendNotification(
        Device,
        T2AUDIO_MSG_NOTIFICATION_ALIVE,
        &(struct { UINT32 proto_ver; UINT32 msg_ver; }){1, 3},
        8);
    
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: Failed to resend ALIVE: %08x\n", status));
        return;
    }
    
    // Wait for ALIVE response
    // Re-enable remote access
    // Restart any active streams
    // ...
}
```

---

## Testing Checklist

### Basic Initialization
- [ ] PCI device detection (VID=106b, PID=1803)
- [ ] BAR0 and BAR4 mapping
- [ ] GPR reading and validation
- [ ] BufferStruct discovery and parsing
- [ ] BCE queue initialization
- [ ] ALIVE handshake
- [ ] Device enumeration

### Stream Setup
- [ ] Format query and parsing
- [ ] Latency query
- [ ] Output buffer mapping from BufferStruct
- [ ] Input buffer allocation and registration
- [ ] SET_REMOTE_ACCESS(ON)

### Playback
- [ ] START_IO command
- [ ] First timestamp reception
- [ ] Position interpolation accuracy
- [ ] Buffer wrapping
- [ ] STOP_IO command
- [ ] Glitch-free start/stop

### Capture
- [ ] Input buffer allocation
- [ ] SET_INPUT_STREAM_ADDRESS_RANGES
- [ ] Timestamp handling (with latency)
- [ ] Data validation

### Timing Edge Cases
- [ ] Delayed timestamp (projected position)
- [ ] Timeline reset detection (update_seed=1)
- [ ] Backward frame jump detection
- [ ] Position never goes backward
- [ ] XRUN recovery

### Power Management
- [ ] Suspend: streams stop, remote access disabled
- [ ] Resume: remote access enabled
- [ ] NOTIFICATION_BOOT handling
- [ ] Stream restart after resume

### Error Handling
- [ ] Command timeout
- [ ] Invalid response
- [ ] BufferStruct validation failure
- [ ] Stale timestamp rejection
- [ ] Timeline fault recovery

---

## Common Windows Driver Mistakes

1. **Not using READ_REGISTER_* macros**: Direct pointer dereferencing can be reordered
2. **Forgetting KeMemoryBarrier()**: T2 might see stale BufferStruct values
3. **Using KeQuerySystemTime**: Need KeQueryPerformanceCounter for high-res timing
4. **Mapping BAR0 as cached**: Must be uncached for device memory
5. **Not handling DPC-level interrupts**: Timestamp notifications come at DISPATCH_LEVEL
6. **Incorrect IRQL**: Most operations need PASSIVE_LEVEL, timing at DISPATCH_LEVEL
7. **Using floating-point**: Kernel mode forbids FPU - must use integer bit manipulation
8. **Race between START_IO and first timestamp**: Must mark stream started before command
9. **Not checking PreviousState in D0Entry**: Different behavior for cold boot vs resume
10. **Forgetting to ack UPDATE_TIMESTAMP**: T2 will stop sending after timeout

---

## Performance Considerations

### Latency Targets
- **Playback**: 10ms (480 frames @ 48kHz) - achievable with proper timestamp handling
- **Capture**: 20ms (960 frames @ 48kHz) - limited by T2 buffer management
- **Round-trip**: ~30-40ms typical

### Timestamp Frequency
- T2 sends timestamps every ~10-20ms
- Position interpolation fills gaps
- QPC must be fast enough for frequent polling

### Buffer Sizes
- Output: Typically 128KB (2.73 sec @ 48kHz 6ch S16)
- Input: ~131KB (2.73 sec @ 48kHz 2ch S16)
- Minimum period: 1024 frames (21ms @ 48kHz)

### CPU Overhead
- Command queue processing: negligible
- Position interpolation: <1% CPU
- BCE interrupt handling: <2% CPU under load

---

This guide provides the critical patterns and structures needed to implement a Windows kernel-mode driver for T2 audio. The key challenges are the asynchronous timing model and proper handling of the BCE command transport.
