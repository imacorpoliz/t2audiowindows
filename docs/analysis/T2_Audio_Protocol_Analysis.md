# T2 Audio Protocol Analysis - Windows Implementation Reference

## 1. T2 Command Codes (protocol.h:42-67)

### Core Stream Control
```c
T2AUDIO_MSG_START_IO                = 0   // Start audio stream
T2AUDIO_MSG_START_IO_RESPONSE       = 1
T2AUDIO_MSG_STOP_IO                 = 2   // Stop audio stream
T2AUDIO_MSG_STOP_IO_RESPONSE        = 3
T2AUDIO_MSG_UPDATE_TIMESTAMP        = 4   // Position/timing update from T2
```

### Property Management
```c
T2AUDIO_MSG_GET_PROPERTY            = 7   // Query device/stream properties
T2AUDIO_MSG_GET_PROPERTY_RESPONSE   = 8
T2AUDIO_MSG_SET_PROPERTY            = 9   // Configure device/stream
T2AUDIO_MSG_SET_PROPERTY_RESPONSE   = 10
T2AUDIO_MSG_PROPERTY_LISTENER       = 11  // Register for property changes
T2AUDIO_MSG_PROPERTY_LISTENER_RESPONSE = 12
T2AUDIO_MSG_PROPERTY_CHANGED        = 13  // Async property change notification
```

### Stream Configuration
```c
T2AUDIO_MSG_SET_INPUT_STREAM_ADDRESS_RANGES = 18  // Provide DMA buffer addresses
T2AUDIO_MSG_SET_INPUT_STREAM_ADDRESS_RANGES_RESPONSE = 19
T2AUDIO_MSG_GET_INPUT_STREAM_LIST   = 24  // Enumerate input streams
T2AUDIO_MSG_GET_INPUT_STREAM_LIST_RESPONSE = 25
T2AUDIO_MSG_GET_OUTPUT_STREAM_LIST  = 26  // Enumerate output streams
T2AUDIO_MSG_GET_OUTPUT_STREAM_LIST_RESPONSE = 27
```

### Session Management
```c
T2AUDIO_MSG_SET_REMOTE_ACCESS       = 32  // Enable/disable audio access
T2AUDIO_MSG_SET_REMOTE_ACCESS_RESPONSE = 33
T2AUDIO_MSG_UPDATE_TIMESTAMP_RESPONSE = 34  // Ack timestamp notification

// Notifications (async from T2)
T2AUDIO_MSG_NOTIFICATION_ALIVE      = 100 // Keepalive/handshake
T2AUDIO_MSG_GET_DEVICE_LIST         = 101 // Enumerate audio devices
T2AUDIO_MSG_GET_DEVICE_LIST_RESPONSE = 102
T2AUDIO_MSG_NOTIFICATION_BOOT       = 104 // T2 reboot notification
```

### Remote Access Modes (protocol.h:71-73)
```c
T2AUDIO_REMOTE_ACCESS_OFF = 0
T2AUDIO_REMOTE_ACCESS_ON  = 2
```

---

## 2. Property Selectors (protocol.h:76-88)

### Property Scopes
```c
T2AUDIO_PROP_SCOPE_GLOBAL = 0x676c6f62  // 'glob' - device-wide
T2AUDIO_PROP_SCOPE_INPUT  = 0x696e7074  // 'inpt' - input stream
T2AUDIO_PROP_SCOPE_OUTPUT = 0x6f757470  // 'outp' - output stream
```

### Property Types
```c
T2AUDIO_PROP_UID          = 0x75696420  // 'uid ' - Device identifier string
T2AUDIO_PROP_BOOL_VALUE   = 0x6263766c  // 'bcvl' - Boolean value
T2AUDIO_PROP_JACK_PLUGGED = 0x6a61636b  // 'jack' - Headphone jack status
T2AUDIO_PROP_SEL_VOLUME   = 0x64656176  // 'deav' - Volume control
T2AUDIO_PROP_LATENCY      = 0x6c746e63  // 'ltnc' - Stream latency (u32)
T2AUDIO_PROP_PHYS_FORMAT  = 0x70667420  // 'pft ' - Audio format descriptor
```

### Property Address Structure (protocol.h:28-33)
```c
struct t2audio_prop_addr {
    u32 scope;      // GLOBAL/INPUT/OUTPUT
    u32 selector;   // UID/LATENCY/PHYS_FORMAT/etc
    u32 element;    // Index (usually 0)
};
```

---

## 3. Message Structure

### Message Header (protocol.h:18-26)
```c
struct t2audio_msg_header {
    char tag[4];              // Transaction tag (e.g., "S001")
    u8 type;                  // COMMAND/RESPONSE/NOTIFICATION
    t2audio_device_id_t device_id;  // Target device (u64)
} __packed;

struct t2audio_msg_base {
    u32 msg;      // Message type code (START_IO, GET_PROPERTY, etc)
    u32 status;   // Status code (0 = success)
} __packed;

// Message types
T2AUDIO_MSG_TYPE_COMMAND      = 1
T2AUDIO_MSG_TYPE_RESPONSE     = 2
T2AUDIO_MSG_TYPE_NOTIFICATION = 3
```

### Message Layout
```
+------------------+
| msg_header (13B) |  tag[4] + type + device_id
+------------------+
| msg_base (8B)    |  msg + status
+------------------+
| payload (varies) |
+------------------+
```

---

## 4. Buffer Management Architecture

### BufferStruct Overview (audio.h:27-51)
The T2 chip provides a shared memory structure describing DMA buffers:

```c
struct t2audio_buffer_struct {
    u32 version;      // Protocol version (expect >= 3)
    u32 signature;    // Magic: 0x19870423
    u32 flags;
    u8 num_devices;   // Number of audio devices
    struct t2audio_buffer_struct_device devices[20];
};
```

### Buffer Discovery (audio.c:580-596)
```c
// 1. Read GPR (General Purpose Register) region at reg_mem_cfg + 0xC000
u32 ver = ioread32(&reg_mem_gpr[0]);      // Must be >= 3
u32 sig = ioread32(&reg_mem_gpr[1]);      // Must be 0x19870423
u32 bs_base = ioread32(&reg_mem_gpr[2]);  // Offset into BAR0

// 2. Map BufferStruct
struct t2audio_buffer_struct *bs = (void*)(reg_mem_bs + bs_base);
```

### Device Buffer Structure (audio.h:37-44)
```c
struct t2audio_buffer_struct_device {
    char name[128];                 // "Speaker", "Digital Mic", etc
    u8 num_input_streams;
    u8 num_output_streams;
    struct t2audio_buffer_struct_stream input_streams[5];
    struct t2audio_buffer_struct_stream output_streams[5];
    char filler[128];
};
```

### Stream Buffer Structure (audio.h:27-36)
```c
struct t2audio_buffer_struct_stream {
    u8 num_buffers;
    struct t2audio_buffer_struct_buffer buffers[100];
    char filler[32];
};

struct t2audio_buffer_struct_buffer {
    size_t address;   // Physical address (offset from BAR0 base)
    size_t size;      // Buffer size in bytes
    size_t pad[4];
} __packed __aligned(4);
```

### Buffer Types (audio.h:61-71)
```c
enum t2audio_dma_buf_type {
    T2AUDIO_DMA_BUF_IOMEM,     // Output: T2-owned, map from BAR0
    T2AUDIO_DMA_BUF_COHERENT   // Input: Host-allocated DMA coherent
};

struct t2audio_dma_buf {
    dma_addr_t dma_addr;  // Physical address for DMA
    void *ptr;            // Virtual mapping
    size_t size;
    enum t2audio_dma_buf_type type;
};
```

### Output Stream Buffer Access (audio.c:710-741)
Output (playback) buffers are pre-allocated by T2:
```c
// Map from BAR0 using address from BufferStruct
strm->buffers[i].dma_addr = reg_mem_bs_dma + bs_strm->buffers[i].address;
strm->buffers[i].ptr = reg_mem_bs + bs_strm->buffers[i].address;
strm->buffers[i].size = bs_strm->buffers[i].size;
strm->buffers[i].type = T2AUDIO_DMA_BUF_IOMEM;
```

### Input Stream Buffer Allocation (audio.c:743-782)
Input (capture) buffers are allocated by host:
```c
// Allocate DMA coherent memory
size_t size = strm->desc.bytes_per_packet * 16640;  // Ring size
void *dma_ptr = dma_alloc_coherent(dev, size, &dma_addr, GFP_KERNEL);

// Write back to BufferStruct so T2 can access
bs_strm->buffers[0].address = dma_addr;
bs_strm->buffers[0].size = size;
bs_strm->num_buffers = 1;
wmb();  // Ensure T2 sees updated structure

// Notify T2 of new buffer addresses
t2audio_cmd_set_input_stream_address_ranges(dev_id);
```

---

## 5. Stream Control Flow

### Initialization Sequence (audio.c:383-414)
```
1. Send ALIVE notification (proto_ver=1, msg_ver=3)
2. Wait for ALIVE response (500ms timeout)
3. GET_DEVICE_LIST → list of device_id values
4. For each device:
   a. GET_PROPERTY(UID) → device name
   b. GET_PROPERTY(LATENCY, INPUT)
   c. GET_PROPERTY(LATENCY, OUTPUT)
   d. GET_INPUT_STREAM_LIST → list of stream_id values
   e. GET_OUTPUT_STREAM_LIST
   f. For each stream:
      - GET_PROPERTY(PHYS_FORMAT) → format descriptor
      - GET_PROPERTY(LATENCY)
5. Read BufferStruct from GPR
6. Match devices by name, map buffers
7. Allocate input stream DMA, call SET_INPUT_STREAM_ADDRESS_RANGES
8. SET_REMOTE_ACCESS(ON)
```

### Start Stream Flow (pcm.c:242-277)
```c
int t2audio_pcm_start_sync(substream) {
    // 1. Reset timing state
    stream->waiting_for_first_ts = true;
    stream->remote_timestamp = 0;
    stream->sample_frames = 0;
    stream->timestamp_accept_after = ktime_get();
    
    // 2. Ensure prefilled data is visible
    wmb();
    
    // 3. Mark stream as started (for timestamp handling)
    smp_store_release(&stream->started, 1);
    
    // 4. Send START_IO command (synchronous)
    status = t2audio_cmd_start_io(dev_id);
    if (status) {
        smp_store_release(&stream->started, 0);
        return status;
    }
    
    return 0;
}
```

### Stop Stream Flow (pcm.c:279-291)
```c
int t2audio_pcm_stop_sync(substream) {
    // 1. Mark stream as stopped (reject new timestamps)
    smp_store_release(&stream->started, 0);
    
    // 2. Send STOP_IO command (synchronous)
    status = t2audio_cmd_stop_io(dev_id);
    
    // 3. Reset timing
    stream->remote_timestamp = 0;
    stream->waiting_for_first_ts = true;
    
    return status;
}
```

---

## 6. Position/Timing Synchronization

### Timestamp Message (protocol.c:34-50)
```c
// T2 sends UPDATE_TIMESTAMP as command (must ack with response)
struct timestamp_msg {
    u64 timestamp;      // Host monotonic time (ns) when DMA position was sampled
    u64 update_seed;    // 1 = reset, 0 = continuation
    u64 sample_time;    // Frame count as IEEE-754 double (little-endian bits)
};
```

### Timestamp Decoding (timing.h:13-37)
```c
// Decode IEEE-754 double to u64 frame count (no FP math)
bool t2audio_decode_sample_time(u64 bits, u64 *frames) {
    unsigned int exponent = (bits >> 52) & 0x7ff;
    u64 fraction = bits & ((1ULL << 52) - 1);
    
    // Reject negative, non-integral, or too-large values
    if ((bits >> 63) || exponent < 1023 || exponent > 1076)
        return false;
    
    // Extract integer from mantissa+exponent
    fraction |= 1ULL << 52;
    unsigned int shift = 1075 - exponent;
    if (fraction & ((1ULL << shift) - 1))  // Non-integer
        return false;
    *frames = fraction >> shift;
    return true;
}
```

### Timestamp Processing (pcm.c:534-602)
```c
void t2audio_handle_stream_timestamp(substream, device_timestamp, update_seed, sample_frames) {
    // 1. Ignore if stream not started or timestamp too old
    if (!stream->started || device_timestamp < stream->timestamp_accept_after)
        return;
    
    // 2. Detect timeline reset/corruption
    if ((update_seed && sample_frames) ||  // Seed=1 must have frames=0
        (update_seed && stream->sample_frames) ||  // Mid-stream reset
        (sample_frames < stream->last_sample_frames))  // Backward jump
    {
        stream->timeline_fault = true;
        snd_pcm_period_elapsed();  // Trigger XRUN recovery
        return;
    }
    
    // 3. Correct for delayed callbacks (frames behind DMA clock)
    if (!stream->waiting_for_first_ts) {
        u64 elapsed_ns = device_timestamp - stream->last_device_timestamp;
        u64 elapsed_frames = (elapsed_ns * rate) / NSEC_PER_SEC;
        
        // If new anchor is behind projected position, carry forward
        u64 projected = stream->sample_frames + elapsed_frames;
        if (sample_frames < projected)
            sample_frames = projected;
    }
    
    // 4. Save anchor
    stream->sample_frames = sample_frames;
    stream->last_device_timestamp = device_timestamp;
    stream->remote_timestamp = ns_to_ktime(device_timestamp);
    stream->waiting_for_first_ts = false;
    
    // 5. Notify ALSA of period completion
    if (!no_period_wakeup)
        snd_pcm_period_elapsed();
}
```

### Position Reporting (pcm.c:383-417)
```c
snd_pcm_uframes_t t2audio_pcm_pointer(substream) {
    if (!stream->started || stream->waiting_for_first_ts)
        return 0;
    
    if (stream->timeline_fault)
        return SNDRV_PCM_POS_XRUN;
    
    // Interpolate position between timestamp anchors
    s64 elapsed_ns = ktime_get() - stream->remote_timestamp;
    u64 elapsed_frames = (elapsed_ns * rate) / NSEC_PER_SEC;
    
    // Apply latency and clamp to prevent backward motion
    u64 frames = stream->sample_frames + elapsed_frames;
    frames = (frames > latency) ? frames - latency : 0;
    frames = max(frames, stream->reported_frames);
    
    stream->reported_frames = frames;
    return frames % buffer_size;
}
```

---

## 7. Format Negotiation

### Audio Format Descriptor (description.h:6-16)
```c
struct t2audio_apple_description {
    u64 sample_rate_double;  // IEEE-754 double (e.g., 48000.0)
    u32 format_id;           // 'lpcm' = 0x6c70636d
    u32 format_flags;        // See below
    u32 bytes_per_packet;    // Frame size * channels
    u32 frames_per_packet;   // Usually 1
    u32 bytes_per_frame;     // Sample size * channels
    u32 channels_per_frame;  // Channel count (2, 6, etc)
    u32 bits_per_channel;    // 16, 24, 32
    u32 reserved;
};

// Format flags
T2AUDIO_FORMAT_FLAG_FLOAT           = 0x01
T2AUDIO_FORMAT_FLAG_BIG_ENDIAN      = 0x02
T2AUDIO_FORMAT_FLAG_SIGNED          = 0x04
T2AUDIO_FORMAT_FLAG_PACKED          = 0x08  // 24-bit in 3 bytes
T2AUDIO_FORMAT_FLAG_ALIGNED_HIGH    = 0x10  // 24-bit in 4 bytes, MSB-aligned
T2AUDIO_FORMAT_FLAG_NON_INTERLEAVED = 0x20
T2AUDIO_FORMAT_FLAG_NON_MIXABLE     = 0x40
```

### Format Query (audio.c:506-517)
```c
// Query format for each stream
t2audio_cmd_get_primitive_property(
    dev_id,
    stream_id,
    T2AUDIO_PROP(T2AUDIO_PROP_SCOPE_GLOBAL, T2AUDIO_PROP_PHYS_FORMAT, 0),
    NULL, 0,  // No qualifier
    &stream->desc, sizeof(struct t2audio_apple_description)
);
```

### 6-Channel Example
For MacBookPro16,1 speakers:
```c
sample_rate_double = 48000.0 (0x40E7700000000000)
format_id = 'lpcm'
format_flags = 0x04 (SIGNED)
bytes_per_packet = 12 (2 bytes * 6 channels)
frames_per_packet = 1
bytes_per_frame = 12
channels_per_frame = 6
bits_per_channel = 16
```

Maps to: S16_LE, 6 channels, 48kHz interleaved

---

## 8. Critical Constants

### Queue Configuration (protocol_bce.h:7-10)
```c
T2AUDIO_BCE_QUEUE_ELEMENT_SIZE  = 0x1000  // 4KB per message
T2AUDIO_BCE_QUEUE_ELEMENT_COUNT = 20      // Ring depth
T2AUDIO_BCE_QUEUE_TAG_COUNT     = 1000    // Max in-flight transactions
```

### BufferStruct Magic (audio.h:12)
```c
T2AUDIO_SIG = 0x19870423  // Signature for version/buffer validation
```

### Device Limits (audio.h:14-19)
```c
T2AUDIO_DEVICE_MAX_UID_LEN       = 128
T2AUDIO_DEVICE_MAX_INPUT_STREAMS = 1
T2AUDIO_DEVICE_MAX_OUTPUT_STREAMS = 1
T2AUDIO_DEVICE_MAX_BUFFER_COUNT  = 1
```

### Known Device Names (audio.c:979-986)
```c
"Speaker"          // Main speakers (6ch on 16-inch)
"Digital Mic"      // Internal microphone array
"Codec Output"     // Headphone jack output
"Codec Input"      // Headphone jack input
"Bridge Loopback"  // Internal loopback
```

### Timing Constants (audio.c:749)
```c
// Input buffer size calculation
size = bytes_per_packet * 16640;  // ~5.7 seconds at 48kHz
```

---

## 9. Command Transport (BCE Queues)

### Queue Initialization (protocol_bce.c:14-32)
```c
// Create completion queue (shared by both SQs)
cq = t2bce_core_create_cq(bce_client, 0x80);

// Outbound queue (host → T2 commands)
qout.sq = t2bce_core_create_sq(
    bce_client,
    cq,
    "com.apple.BridgeAudio.IntelToARM",
    el_count + 1,  // +1 for ring structure
    DMA_TO_DEVICE,
    completion_callback,
    userdata
);

// Inbound queue (T2 → host responses/notifications)
qin.sq = t2bce_core_create_sq(
    bce_client,
    cq,
    "com.apple.BridgeAudio.ARMToIntel",
    el_count + 1,
    DMA_FROM_DEVICE,
    completion_callback,
    userdata
);

// Pre-submit receive buffers
t2audio_bce_in_queue_submit_pending(qin, qin.el_count);
```

### Synchronous Command (protocol.c:279-283)
```c
int t2audio_cmd_start_io(device, device_id) {
    struct t2audio_msg reply = t2audio_reply_alloc();
    
    // Send command with 500ms timeout
    status = t2audio_send_cmd_sync(
        device, &sctx, &reply, 500,
        t2audio_msg_write_start_io, device_id
    );
    
    // Parse response
    status = t2audio_msg_read_start_io_response(&reply);
    
    t2audio_reply_free(&reply);
    return status;
}
```

### Transaction Tag System (protocol_bce.c:53-61)
```c
// Generate unique 4-byte tag for request/response matching
void t2audio_send_create_tag(bce, tag) {
    bce->tag_num = (bce->tag_num + 1) % 1000;
    tag[0] = 'S';
    tag[1] = '0' + bce->tag_num / 100;
    tag[2] = '0' + (bce->tag_num / 10) % 10;
    tag[3] = '0' + bce->tag_num % 10;
    // Results in: "S000", "S001", ..., "S999"
}
```

---

## 10. Windows Implementation Checklist

### Hardware Access
- [ ] Map PCI BAR0 (audio buffers) and BAR4 (config)
- [ ] Read GPR at BAR4+0xC000 to locate BufferStruct
- [ ] Parse BufferStruct for device/stream/buffer info
- [ ] Map output buffers from BAR0 as device memory
- [ ] Allocate DMA coherent memory for input buffers

### BCE Command Transport
- [ ] Implement BCE queue pair (Intel→ARM, ARM→Intel)
- [ ] Transaction tag generation/matching
- [ ] Synchronous command with timeout
- [ ] Asynchronous notification handling
- [ ] Command/response/notification dispatch

### Protocol Messages
- [ ] ALIVE handshake (send, wait for response)
- [ ] GET_DEVICE_LIST
- [ ] GET_PROPERTY (UID, LATENCY, PHYS_FORMAT)
- [ ] GET_INPUT/OUTPUT_STREAM_LIST
- [ ] SET_INPUT_STREAM_ADDRESS_RANGES
- [ ] SET_REMOTE_ACCESS (ON/OFF)
- [ ] START_IO / STOP_IO
- [ ] UPDATE_TIMESTAMP handling + response ack

### Audio Format
- [ ] Parse t2audio_apple_description
- [ ] Decode IEEE-754 sample_rate_double
- [ ] Map format_flags → WAVEFORMATEX
- [ ] Support 2/6 channel configurations
- [ ] Support S16_LE, S24_LE, S32_LE

### Timing/Synchronization
- [ ] Decode IEEE-754 sample_time to frame count
- [ ] Handle update_seed (reset vs continuation)
- [ ] Interpolate position between timestamps
- [ ] Apply latency compensation
- [ ] Detect timeline faults (backward jumps, mid-stream resets)
- [ ] Prevent reported position from going backward

### Stream State Machine
- [ ] Initialize: allocate buffers, register with T2
- [ ] Prepare: reset timing, clear buffers
- [ ] Start: START_IO, mark stream active, accept timestamps
- [ ] Running: process timestamps, report position
- [ ] Stop: STOP_IO, reject timestamps
- [ ] Cleanup: SET_REMOTE_ACCESS(OFF), free buffers

### Power Management
- [ ] Suspend: STOP all streams, SET_REMOTE_ACCESS(OFF)
- [ ] Resume: SET_REMOTE_ACCESS(ON), restart streams
- [ ] Handle NOTIFICATION_BOOT (T2 reset)

### Device Discovery
- [ ] Match BufferStruct device names to stream objects
- [ ] "Speaker" → WaveOut playback device
- [ ] "Digital Mic" → WaveIn capture device
- [ ] "Codec Output" → Headphone jack (with jack detection)
- [ ] "Codec Input" → Headphone mic input

---

## 11. DSP Processing Chain (graph.json)

MacBookPro16,1 uses a complex DSP pipeline in PipeWire/PulseAudio:

### Processing Stages
1. **Input** (2 channels from app)
2. **Autogain** (LSP stereo autogain, -12dB target)
3. **Virtual Bass** (psychoacoustic bass enhancement, 40-150Hz)
4. **Tone Control Stage 1** (6 cascaded biquads per channel)
5. **Crosstalk Cancellation** (XTC):
   - Mid/side decomposition
   - Mid delay (0.208ms)
   - Side FIR convolution
   - Reconstruction to L/R
6. **Tone Control Stage 2** (4 biquads + parametric per channel)
7. **Multiband Compression** (4 bands: <338Hz, 338-1590Hz, 1590-6597Hz, >6597Hz)
8. **Analytical Response Correction** (4 biquads per channel)
9. **Output** (6 channels to hardware)

**Note for Windows**: DSP is implemented in userspace (PipeWire/PulseAudio), not kernel driver. Windows driver only needs raw 6-channel PCM transport.

---

## 12. Example Initialization Trace

```
1. PCI device 1803 detected
2. Map BAR0 (buffer space) and BAR4 (config)
3. Read GPR[0]=3 (version), GPR[1]=0x19870423 (sig), GPR[2]=0x8000 (offset)
4. BufferStruct at BAR0+0x8000: 4 devices
5. Send ALIVE(proto_ver=1, msg_ver=3)
6. Receive ALIVE response
7. GET_DEVICE_LIST → [0x1000, 0x2000, 0x3000, 0x4000]
8. Device 0x1000:
   - GET_PROPERTY(UID) → "Speaker"
   - GET_OUTPUT_STREAM_LIST → [0x1001]
   - Stream 0x1001:
     - GET_PROPERTY(PHYS_FORMAT) → 48kHz, S16_LE, 6ch
     - GET_PROPERTY(LATENCY) → 128 frames
   - BufferStruct match: devices[0].name="Speaker"
     - output_streams[0].buffers[0]: addr=0x100000, size=0x20000 (128KB)
     - Map BAR0+0x100000 as playback ring buffer
9. Device 0x2000:
   - GET_PROPERTY(UID) → "Digital Mic"
   - GET_INPUT_STREAM_LIST → [0x2001]
   - Stream 0x2001:
     - GET_PROPERTY(PHYS_FORMAT) → 48kHz, S16_LE, 2ch
     - GET_PROPERTY(LATENCY) → 64 frames
   - BufferStruct: devices[1].name="Digital Mic", no pre-allocated buffer
   - Allocate DMA: 0x2000 * 16640 = ~131KB
   - Write to BufferStruct: devices[1].input_streams[0].buffers[0] = {dma_addr, size}
   - SET_INPUT_STREAM_ADDRESS_RANGES(0x2000)
10. SET_REMOTE_ACCESS(ON)
11. Driver ready, expose audio devices to OS
```

---

## 13. Key Differences from Standard Audio Hardware

1. **No MMIO position register**: Position comes from timestamped notifications, requires interpolation
2. **Asymmetric buffer allocation**: Output (T2-owned) vs Input (host-allocated)
3. **Two-phase initialization**: Enumerate devices, then discover buffers from separate structure
4. **Transaction-based control**: All commands require request/response over BCE queues
5. **Timeline reset detection**: Must handle mid-stream resets and backward timestamp jumps
6. **Mandatory acknowledgment**: UPDATE_TIMESTAMP must be ack'd with response
7. **Session management**: Must SET_REMOTE_ACCESS(ON) before any audio operation

---

## 14. Common Pitfalls

1. **Not acknowledging timestamps**: T2 will stop sending them after timeout
2. **Using stale timestamps**: Check `timestamp_accept_after` to filter pre-START_IO timestamps
3. **Reporting backward position**: Always clamp to `max(new_pos, last_reported)`
4. **Ignoring update_seed**: Seed=1 with nonzero frames is a protocol violation
5. **Wrong buffer type**: Output must be IOMEM (BAR0), input must be DMA coherent
6. **Forgetting SET_INPUT_STREAM_ADDRESS_RANGES**: T2 won't know where to write capture data
7. **Timeout too short**: Some commands (especially during init) can take >100ms
8. **Not handling NOTIFICATION_BOOT**: T2 can restart, must re-initialize session
9. **Assuming single buffer**: BufferStruct supports multiple (though driver only uses 1)
10. **Modifying BufferStruct without wmb()**: T2 might see inconsistent state

---

## 15. Register Layout

### BAR0 (Buffer Memory)
```
0x00000000: Audio DMA buffer space (size from PCI config)
  ├─ Output stream buffers (pre-allocated by T2, read from BufferStruct)
  └─ BufferStruct (location from GPR[2])
```

### BAR4 (Configuration)
```
0x0000: PCI config space
0xC000: GPR (General Purpose Registers)
  ├─ GPR[0]: BufferStruct version (u32)
  ├─ GPR[1]: Signature 0x19870423 (u32)
  └─ GPR[2]: BufferStruct offset in BAR0 (u32)
```

---

## 16. Recommended Reading Order for Implementation

1. **protocol.h**: Understand message types and property system
2. **description.h**: Audio format structure
3. **timing.h**: Position interpolation math
4. **audio.c: t2audio_init_cmd()**: Initialization sequence
5. **audio.c: t2audio_init_bs()**: Buffer discovery
6. **pcm.c: start/stop_sync()**: Stream control
7. **pcm.c: t2audio_handle_stream_timestamp()**: Timing anchor processing
8. **pcm.c: t2audio_pcm_pointer()**: Position reporting
9. **protocol_bce.c**: Command transport layer
10. **protocol.c**: Message encoding/decoding

---

## Summary

The T2 audio protocol is a message-based system where the host:
1. Enumerates devices/streams via GET_PROPERTY commands
2. Discovers DMA buffers from a shared memory structure (BufferStruct)
3. Allocates input buffers and notifies T2 of their addresses
4. Starts/stops streams with START_IO/STOP_IO commands
5. Receives asynchronous timestamp notifications to track DMA position
6. Interpolates position between timestamps using host monotonic clock

The key challenge is handling the asynchronous, notification-based timing model rather than polling hardware registers. Proper timeline reset detection and position clamping are critical for glitch-free playback.
