# AppleAudio.sys Playback PCM Path Analysis

> **Status note (2026-10-28):** an early report. Its conclusion that AppleAudio is zero-copy is contradicted by the later analysis (`RE_appleaudio_protocol.md` §3: AppleAudio copies and processes the samples on the CPU before writing them into the ring). The open questions at the end are answered: one interleaved 6-channel output stream, S24_LE in a 32-bit container, 48 kHz (`DRIVER_GUIDE.md` §2.4).

## Errors in earlier reports, corrected

1. **BufferStruct.devices starts at +0x10**
   - Function 0x46F8: `base = bs_base + index*0xBDEC` points 16 bytes before the device
   - Correct offsets relative to that base:
     - `+0x90` = `device.num_input_streams` (NOT active_flag)
     - `+0x94` = `input_streams[0].num_buffers`
     - `+0x98` = `input_streams[0].buffers[0].address`
     - `+0xA0` = `input_streams[0].buffers[0].size`

2. **The branch 0x4838–0x4875 adds a new device**
   - It is NOT the output branch, as was wrongly claimed

3. **Function 0x9D74 reads the output buffers from BufferStruct**
   - `0x5F0C = 0x10 + 0x5EF8 + 0x04`: BS header + output_streams + buffers[0].address
   - `0x5F14`: the matching buffers[0].size
   - Matches Linux `offsetof(device, output_streams) = 0x5EF8`

## Confirmed call chain for playback

### 1. Function 0x1C240: AllocateOutputBuffer (vtable slot +0xB8)

**Bounds:** 0x1C240–0x1C4BA (from .pdata)

**Parameters:**
- RCX (RSI) = stream object
- RDX (EBX) = frame alignment/channels
- R8D (EBP) = buffer size in bytes
- R9 (R13) = output pointer for the buffer handle

**Key instructions:**

```
0x1C414: call 0x9D74              ; get the output buffer from BufferStruct
         ; Parameters:
         ; RCX = R15 (stream-8)
         ; RDX = [stream->a28]+0x21 (device name)
         ; R8  = GPR base + 0xC000
         ; [RSP+0x20] = &output_offset
         ; [RSP+0x28] = &output_size

0x1C422: mov RBX, [RSP+0x38]      ; RBX = size from BufferStruct
0x1C42C: mov [RSI+0x98], EBX      ; stream->buffer_size = size
0x1C459: add RAX, [RSP+0x40]      ; RAX = GPR_base + offset
0x1C45E: mov [RSI+0xA0], RAX      ; stream->buffer_ptr = virtual address
```

**Confirmation:** the instructions match the ones given in the task exactly.

### 2. Function 0x9D74: GetOutputBufferFromBS

**Purpose:** extract the physical offset and size from BufferStruct for the output stream of a device.

**Code:**
```
0x9D74:  looks the device up by name in BufferStruct.devices[]
0x9DC8:  reads [device_base + 0x5F0C] → output_streams[0].buffers[0].address
0x9DD0:  reads [device_base + 0x5F14] → output_streams[0].buffers[0].size
```

**Offset 0x5F0C:** `0x10 (BS header) + 0x5EF8 (offsetof output_streams) + 0x04 (stream.buffers[0])`

**Linux confirmation:** `audio.h:42` defines `output_streams` with the same layout.

### 3. Function 0xB470: start/stop stream handler (vtable slot +0x80)

**Bounds:** 0xB470–0xB721

**Parameters:**
- RCX (RDI) = stream object
- RDX (RBP) = command: 1=start, 2=pause, 3=stop

**Critical part on stop (0xB5EC–0xB5FC):**
```
0xB5EC: mov R8D, [RDI+0x98]       ; buffer_size
0xB5F5: mov RCX, [RDI+0xA0]       ; buffer_ptr
0xB5FC: call 0xCD80               ; memset(buffer, 0, size)
```

**Conclusion:** the function CLEARS the playback buffer on stop. This confirms that `[RSI+0xA0]` is the real PCM buffer pointer.

### 4. Vtable layout of the stream object

**Vtable base address:** ~0xF048

**Key slots:**
- `+0x00` (slot 0): 0x068C0 — QueryInterface/AddRef
- `+0x80` (slot 16): 0x0B470 — start/stop handler
- `+0xB8` (slot 23): 0x1C240 — AllocateOutputBuffer

**Location of the vtable pointer:** `[stream+0x00]`

### 5. Function 0x184B8: volume/control handler

**NOT PART of the PCM playback data path.**

This function calls vtable slot +0xA0 of an object of **another type** (not a stream), passing volume parameters (R9) through CFG dispatch. It is T2 control, not PCM writing.

## The actual playback PCM path

### Where does the playback PCM come from?

**Linux driver (kait2en/modules/t2bce_audio/pcm.c):**

```c
// pcm.c:259-260
/* Publish userspace prefill before starting the remote consumer. */
wmb();
```

A memory barrier **before** calling `t2audio_cmd_start_io()`.

**Conclusion (Linux):**
1. **Userspace** (the ALSA application) fills the buffer through `mmap()` of shared memory
2. The Linux driver sends START_IO through the BCE transport
3. **The T2 chip reads the PCM directly** from shared memory by DMA
4. BufferStruct holds the physical addresses of these buffers
5. The early hypothesis was that Windows AppleAudio gets these addresses from BufferStruct and maps them into user mode

### Windows playback path (early hypothesis, superseded)

1. The Windows WaveRT application writes PCM into the cyclic buffer through `mmap()`
2. AppleAudio provides the buffer from `[stream+0xA0]`, obtained from the T2
3. The T2 reads the data directly by DMA (as on Linux)
4. AppleAudio only manages the life cycle (START/STOP)

**Claimed key point:** the driver does NOT copy PCM, zero-copy through shared memory. **Superseded:** AppleAudio's copy loop (`0x14000A715`–`0x14000B2C0`) converts and filters the samples before writing them into the ring, and a BAR cannot be mapped into user mode by PortCls anyway (`DRIVER_GUIDE.md` §6.1).

## Mapping to the Linux structures

### Exact matches

| Windows offset | Linux offsetof | Field | Confirmation |
|---|---|---|---|
| device+0x84 | 0x84 | input_streams | Function 0x46F8 |
| device+0x5EF8 | 0x5EF8 | output_streams | Function 0x9D74 |
| sizeof(device) | 0xBDEC | | Stride in 0x46F8 |
| sizeof(buffer) | 0x30 | | Index *8 in 0x480A |

### Mismatch

**stream.buffers offset:**
- Linux: `+0x04` (after `u8 num_buffers` + padding)
- Windows reads at `+0x98` inside the stream

**Explanation:** Windows uses a **stream-relative offset inside the device**, not the `t2audio_buffer_struct_stream` structure directly. The offset 0x98 = 0x84 (input_streams base) + 0x04 (buffers offset) + alignment + indexing.

## Format, channels, Speaker

**Buffer allocation code (0x1C240):**
- Parameter R8D (EBP): buffer size in bytes
- Parameter RDX (EBX): frame alignment (probably channels * bytes_per_sample)

**No channel splitting was found in the driver code.**

If the T2 provides one buffer for the Speaker device, all 6 channels are passed **interleaved** in one buffer. Separate streams would require:
- Several output_streams[] for one device
- Or several devices with the same name

**BufferStruct check:**
```c
// audio.h:41-42
struct t2audio_buffer_struct_stream output_streams[5];
```

The T2 supports up to 5 output streams per device. If Speaker uses one stream, the channels are interleaved. If several, a split is possible.

**The next concrete gap (since closed):** how many output_streams the T2 registers for the "Speaker" device, and the PCM format (16-bit/24-bit, 44.1/48 kHz, interleaved/planar).

## Commands for checking (Linux)

```bash
# Attach the MacBook and load kait2en
modprobe t2bce_audio

# Check BufferStruct
cat /sys/kernel/debug/t2audio/buffer_struct | hexdump -C

# Find the "Speaker" device and look at num_output_streams
# offset +0x81 from the start of the device

# Start playback
aplay -D hw:CARD=AppleT2,DEV=0 test.wav

# Check the real buffer addresses in dmesg
dmesg | grep t2audio
```

## Remaining questions (answered since)

1. How many output_streams for "Speaker"? (1 interleaved or 6 separate?) → **one stream `0x3A`, 6 interleaved channels.**
2. PCM format: S16_LE, S24_3LE, S32_LE? → **S24_LE in a 4-byte container (`'pft '`: 24 bits, 24 bytes per frame).**
3. Sample rate: 44100, 48000, 96000? → **48000.**
4. How does Windows learn the format? (a query to the T2 or hard-coded in the driver?) → **a `GET_PROPERTY` query (`'pft '`; AppleAudio reads `'pfta'`).**
