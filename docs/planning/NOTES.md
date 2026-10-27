# MacBook Pro 16,1 Six-Channel Audio Research

**Target**: MacBookPro16,1 (2019), Apple T2, Windows 11  
**Goal**: Native six-channel speaker access, then port KAIT2EN DSP processing  
**Status**: Descriptor reading proven; buffer access and independent channel control NOT demonstrated

---

## Verified Driver Hashes

```
AppleAudio.sys (6.1.8100.4)
SHA256: CD8B81FABC9BE6127A082DA22C4CC046A4D46E0225EB6BAF6D27E0DC9862A150

AppleUSBVHCI.sys (6.1.7500.5)
SHA256: 0438292090FC5267CA6D28829FEE7D16DA8B73E7657BDB64250CF8DF832955C1
```

---

## Evidence Classification

- **HARDWARE-TESTED**: Executed on target hardware, results captured in logs
- **STATIC**: Disassembly/binary analysis, not runtime-verified
- **HYPOTHESIS**: Logical inference from static analysis, requires validation
- **UNKNOWN**: Explicit gap requiring investigation

---

## Proven Facts (HARDWARE-TESTED)

### 1. AppleAudio KS Interface (MBP-Audio-report-20261004-085054.txt)
- **Public interface**: WaveSpeaker advertises stereo PCM16 48kHz only
- **Format validation**: Stereo (2ch, mask 0x3, 16-bit) ACCEPTED
- **Six-channel attempts**: All rejected (Win32 error 1169)
  - 6ch PCM16/32, float32
  - Masks: 0x0, 0x3f
- **Conclusion**: Public KS API locked to stereo

### 2. T2 Native Protocol Access (MBP-T2-native-report-20261004-090031.txt)
- **Path**: `\\.\AppleUSBVHCI`
- **IOCTL**: `0x222018` (audio message handler)
- **Working operations**:
  - Command 101 (GET_DEVICE_LIST) → 102 reply
  - Command 26 (GET_OUTPUT_STREAM_LIST) → 27 reply  
  - Command 7 (GET_PROPERTY) → 8 reply

### 3. T2 Audio Devices Enumerated
```
0x21  Digital Mic
0x25  Codec Output
0x39  Speaker         ← TARGET
0x43  Codec Input
0x47  Bridge Loopback
```

### 4. Speaker Device (0x39) Physical Format
- **Stream ID**: 0x3A (single output stream)
- **PHYSICAL_FORMAT** (property 0x70667420 'pft '):
  ```
  sample_rate        = 48000 Hz
  format_id          = 0x6C70636D ('lpcm')
  flags              = 0x4
  bytes_per_packet   = 24
  frames_per_packet  = 1
  bytes_per_frame    = 24
  channels_per_frame = 6          ← SIX CHANNELS
  bits_per_channel   = 24
  non_interleaved    = false (flag & 32 == 0)
  float              = false (flag & 1 == 0)
  ```
- **Data format**: Interleaved signed 24-bit PCM in 32-bit (4-byte) containers
- **Descriptor base64**: `AAAAAABw50BtY3BsBAAAABgAAAABAAAAGAAAAAYAAAAYAAAAAAAAAA==`

---

## Static Analysis: AppleUSBVHCI.sys (STATIC, 2026-10-04)

**Binary**: AppleUSBVHCI.sys 6.1.7500.5 (177056 bytes)  
**Architecture**: x64  
**Device names**: `\Device\AppleUSBVHCI`, `\DosDevices\AppleUSBVHCI`

### Confirmed IOCTL Handler Chain (STATIC)
- **RVA 0x17C50**: DeviceIoControl dispatch routine
- **IOCTL 0x222018** → calls RVA 0x2128 (audio message handler)
  - Requires input ≥ 0x15 bytes (21 bytes minimum)
  - Message size limit: 0x1000 bytes (4096)
  - Uses WDF wait lock for serialization
  - Reply buffer: copies from shared 8KiB allocation
  - **Returned byte count ≠ actual T2 message length** (capacity-based copy)

### Additional IOCTLs - Purpose UNKNOWN (STATIC)
```
0x222004 → RVA 0xDB7C (input: 8 bytes, vtable offset +0x38)
0x222008 → RVA 0xDC5C (input: 32 bytes, vtable offset +0x28)
0x22200C → RVA 0xDAF0 (input: 8, output: 32 bytes, vtable offset +0x30)
0x222010 → RVA 0xDC24 (input: 16 bytes, vtable offset +0x10)
0x222014 → RVA 0xDBEC (input: 16 bytes, vtable offset +0x18)
```

**Common pattern** (all five handlers):
1. Adjust input pointer (skip header bytes)
2. Call wrapper at RVA 0x8A54 (likely lock acquisition)
3. Load global object from RVA 0x27488 (runtime-initialized, **zero in static binary**)
4. Dispatch through vtable to virtual function
5. Tail call or return through IAT@0x1E140 (WDF function)

**Critical findings**:
- Global object at RVA 0x27488 (.data section) is **uninitialized** in static binary
- Object created at runtime (DriverEntry or device initialization)
- Cannot determine object type or vtable structure without runtime analysis
- **No evidence of buffer mapping** in handler code (no MmMapIoSpace, no large allocations)
- AppleAudio.sys **does NOT call** any of these IOCTLs (binary search: not found)

**WARNING**: Do NOT call these without runtime analysis. Purpose unknown, may be unrelated to audio.

### Virtual Function Table at RVA 0x23F78 (STATIC)
```
+0x00 → 0xE7C0  (constructor-like: list init, allocations)
+0x08 → 0xE850  (destructor-like: releases objects, zeros ptrs)
+0x10 → 0xE1C0  ← IOCTL 0x222010 (query/validation pattern)
+0x18 → 0xE180  ← IOCTL 0x222014
+0x20 → 0xE090
+0x28 → 0xE310  ← IOCTL 0x222008
+0x30 → 0xDE30  ← IOCTL 0x22200C
+0x38 → 0xE150  ← IOCTL 0x222004
```

**Note**: This may be base class vtable. Runtime object may use derived class with different vtable.

---

## AppleAudio.sys T2 Message Path (STATIC, 2026-10-04)

**Function at RVA 0x4A60**: Sends T2 protocol messages via IOCTL 0x222018

**Operation** (kernel-mode):
1. Opens `\DosDevices\AppleUSBVHCI` via `IoGetDeviceObjectPointer`
2. Builds IRP with `IoBuildDeviceIoControlRequest(0x222018, ...)`
3. Submits via `IoCallDriver`, waits synchronously
4. Closes handle after each message

**Usage**: Command/control path only (device enumeration, property queries, stream control)

**NOT used for**: PCM audio data transmission (no buffer writes visible)

**Critical finding**: AppleAudio.sys does NOT call unknown IOCTLs (0x222004-0x222014).

**Imports found**: `MmMapIoSpaceEx`, `MmMapLockedPagesSpecifyCache` present in IAT but usage location not determined.

---

## T2 Protocol Implementation (T2NativeProbe.cs) — HARDWARE-TESTED

### Message Structure (CORRECTED from T2NativeProbe.cs:24-70)
```c
// Request (21 bytes header + optional payload)
struct T2Request {
    uint32 tag;            // offset 0, echoed in reply
    uint8  type;           // offset 4, always 1 for request
    uint64 device_id;      // offset 5
    uint32 command;        // offset 13
    uint32 unknown_field;  // offset 17 — MISSING in previous docs
    uint8  payload[];      // offset 21
};

// Reply (21 bytes header + payload)
struct T2Reply {
    uint32 tag;            // offset 0, matches request
    uint8  type;           // offset 4, always 2 for reply
    uint64 device_id;      // offset 5
    uint32 reply_command;  // offset 13
    uint32 status;         // offset 17 (0 = success)
    uint8  payload[];      // offset 21
};
```

### Property Read (Command 7 → 8) — CORRECTED
```c
// Property request payload (28 bytes, starts at absolute offset 21)
struct PropertyRequest {
    uint64 object_id;      // payload offset 0 (stream or device)
    uint32 unknown_0;      // payload offset 8, always 0
    uint32 scope;          // payload offset 12, 0x676C6F62 ('glob')
    uint32 selector;       // payload offset 16, e.g. 0x70667420 ('pft ')
    uint64 unknown_1;      // payload offset 20, always 0
};

// Property reply payload (starts at absolute offset 21)
// PROOF: T2NativeProbe.cs:64-70 reads size at absolute 41, data at 49
struct PropertyReplyPayload {
    uint64 object_id;      // payload+0  → absolute 21, echo
    uint32 unknown_0;      // payload+8  → absolute 29
    uint32 scope;          // payload+12 → absolute 33, echo
    uint32 selector;       // payload+16 → absolute 37, echo
    uint64 value_size;     // payload+20 → absolute 41
    uint8  value[];        // payload+28 → absolute 49
};
```

### Probe Safety Constraints
- **Allowed commands**: 101 (device list), 26 (stream list), 7 (get property)
- **Tag range**: 120–126, wrapping
- **Validation**: Tag, type, reply command, status, bounds checked
- **No writes**: No SET_PROPERTY, START_IO, STOP_IO, format changes

---

## Critical Unknowns

### 1. Audio Buffer Access ⚠️ BLOCKING ISSUE (UNKNOWN)

**Linux method** (KAIT2EN, known):
- PCI BAR0: Audio buffer space (contains BufferStruct + DMA buffers)
- PCI BAR4 + 0xC000: GPR registers pointing to BufferStruct location
- Output buffers: Pre-allocated by T2, mapped from BAR0
- Input buffers: Host-allocated DMA coherent memory

**Windows path** (UNKNOWN after static analysis):
- AppleAudio.sys uses IOCTL 0x222018 for **messages only** (STATIC, confirmed RVA 0x4A60)
- AppleAudio.sys does **NOT** call unknown IOCTLs 0x222004-0x222014 (STATIC, binary search)
- AppleAudio.sys imports `MmMapIoSpaceEx` but usage location not determined (STATIC)
- Unknown IOCTLs dispatch through runtime object, purpose unclear (STATIC)
- **No evidence** of buffer mapping in IOCTL handlers (STATIC)

**Blocking**: Cannot transmit audio without buffer access method

**Next investigation required**:
- Trace MmMapIoSpaceEx calls in AppleAudio.sys (static analysis, 2-4 hours)
- Runtime kernel debugging to capture physical addresses (requires hardware, 1-2 hours)
- Analyze AppleUSBVHCI DriverEntry for PCI resource initialization (static, 2-3 hours)

### 2. Stream Control Commands (HYPOTHESIS + STATIC)

- START_IO (cmd 0) / STOP_IO (cmd 2): **KNOWN** from KAIT2EN protocol
- Can be sent via IOCTL 0x222018: **STATIC** (test tool created, not executed)
- Expected sequence (HYPOTHESIS from Linux driver):
  1. SET_REMOTE_ACCESS(ON) — command 32
  2. Map/allocate buffers (mechanism UNKNOWN)
  3. Notify T2 of buffer addresses (for input: cmd 18)
  4. START_IO (cmd 0)
  5. Handle UPDATE_TIMESTAMP notifications (cmd 4)
  6. STOP_IO (cmd 2)
- **UNKNOWN**: Whether START_IO succeeds without buffer setup
- **UNTESTED**: StreamControlTest.cs exists but not executed (per safety constraints)

### 3. Position Reporting (STATIC from protocol docs)

- T2 sends UPDATE_TIMESTAMP (cmd 4) notifications asynchronously
- Contains: device timestamp (ns), update_seed, sample_time (IEEE-754 double)
- Host must interpolate position between timestamps using QPC
- **UNKNOWN**: How Windows receives these notifications (callback? polling? separate IOCTL?)

### 4. Coexistence with AppleAudio.sys ⚠️ RISK (UNKNOWN)

- Exclusive ownership model unknown
- Risk of conflict if both drivers access stream simultaneously
- No tested safe handoff mechanism
- **Mitigation** (HYPOTHESIS): May need to disable AppleAudio.sys driver before testing

### 5. Channel Mapping & Processing (UNKNOWN)

- Physical speaker order in 6-channel stream (FL, FR, C, LFE, RL, RR?)
- Native DSP: does T2 apply crossovers, limiting, EQ?
- Or is raw access possible requiring host-side KAIT2EN processing?
- **Investigation**: Single-channel test tones required (Phase 3, after buffer access resolved)

### 6. Safety Limits ⚠️ CRITICAL (HYPOTHESIS)
   - Volume scaling, clipping protection in native path
   - Risk of hardware damage from incorrect channel mapping or excessive levels
   - **Mandatory before playback**: Attenuation (-30dB), duration limits (100-200ms)

---

## Linux KAIT2EN Architecture (Reference)

**Repository**: https://github.com/kaiT2en/KaiT2en-Fedora

### Key Components
- **t2bce_core/**: BCE (Bridge Control Engine) core protocol
- **t2bce_dma/**: DMA buffer management (PCI BAR mapping)
- **t2bce_audio/**: Audio device driver
  - `protocol.c`, `protocol.h`: T2 message encoding
  - `description.h`: Device/stream descriptors
  - `audio.c`: ALSA integration
  - `pcm.c`: PCM stream operations
  - `protocol_bce.c`: BCE transport layer

### DSP Processing
- **Location**: `dsp/profiles/16_1/graph.json`
- **Host-side processing**: KAIT2EN applies crossovers, EQ, limiting on CPU
- **License**: Must verify before porting

### Buffer Architecture (Linux)
- Output buffers mapped from **PCI BAR**
- Ring buffer with position tracking
- Synchronization via DMA completion notifications
- **Question**: Does Windows path use same PCI BAR or different mechanism?

---

## Next Steps

### Phase 1: Buffer Access Discovery (CURRENT)
**Goal**: Identify how to map/access audio buffers and write samples

**Status**: Stream control commands (START_IO/STOP_IO) can be sent via IOCTL 0x222018.
Buffer access mechanism remains unknown.

**Approach**:
1. **Static analysis**: Reverse-engineer AppleAudio.sys to find buffer allocation
2. **PCI BAR investigation**: Check if Windows exposes same PCI resources as Linux
3. **IOCTL exploration**: Carefully probe unknown IOCTLs (0x222004–0x222014)
4. **AppleUSBVHCI deeper dive**: Find buffer mapping functions beyond message transport

**Test tool created**: MBP-Stream-Control-Test/StreamControlTest.cs
- Sends START_IO (cmd 0) and STOP_IO (cmd 2) via IOCTL 0x222018
- Does NOT access buffers or transmit audio
- Safe to run for protocol verification

**Critical unknowns**:
- Buffer access: Linux uses PCI BAR0 mapping. Windows path unknown.
- Possible mechanisms:
  1. Unknown IOCTLs (0x222004–0x222014) provide buffer mapping
  2. Direct PCI BAR access via user-mode driver
  3. Shared memory section from AppleUSBVHCI
  4. Different protocol entirely (AppleAudio.sys does stereo upmix?)

**Required before audio test**: Confirmed buffer access method + position reporting

### Phase 2: Stream Control Commands
**Goal**: Identify START_IO, STOP_IO, format negotiation commands

**Sources**:
- KAIT2EN protocol.c command constants
- AppleUSBVHCI.sys message handlers
- Trial of safe control messages (NO DATA writes until confirmed safe)

### Phase 3: Controlled Channel Test
**Goal**: Verify independent six-channel operation

**Safety requirements**:
- Volume attenuation (start at -30 dB or lower)
- Short duration test signals (100–200ms max)
- Single-channel test tones to verify routing
- Emergency stop mechanism confirmed

**Test procedure**:
1. Sine sweep on channel 0 only → identify physical speaker
2. Repeat for channels 1–5
3. Document physical mapping
4. Verify no crosstalk between channels

### Phase 4: DSP Port
**Goal**: Implement KAIT2EN processing chain for MacBookPro16,1

**Components**:
- Parse `dsp/profiles/16_1/graph.json`
- Port crossover filters, EQ, limiters to C#/C++
- Integrate with Windows audio stack (APO? WASAPI loopback?)
- Verify licensing compliance

---

## Open Questions

1. Does AppleAudio.sys internally upconvert stereo to 6ch, or does T2 handle mixing?
2. Are additional IOCTLs (0x222004–0x222014) buffer-related or unrelated services?
3. Can we safely access PCI BARs from user-mode (via VHCI proxy) or need kernel driver?
4. Does T2 firmware enforce exclusive stream ownership?
5. What is the native speaker order: standard 5.1 or Apple-specific?

---

## Constraints & Safety Rules

- **No blind IOCTL calls**: Static analysis required before unknown operations
- **No driver patching**: Work with official signed drivers
- **No signature disable**: Test within standard Windows security model
- **No uncontrolled audio**: Volume limits + duration caps mandatory before playback
- **Coexistence awareness**: Assume AppleAudio.sys may conflict; plan for exclusive access
- **Incremental validation**: Each claim proven before building on it

---

## Buffer Access Investigation - Results (STATIC, 2026-10-04)

### Option A: Unknown IOCTLs Analysis ✓ COMPLETED

**Method**: Static disassembly of AppleUSBVHCI.sys handlers at RVA 0xDB7C, 0xDC5C, 0xDAF0, 0xDC24, 0xDBEC

**Findings**:
- All five handlers follow identical pattern:
  1. Adjust input pointer (skip header)
  2. Acquire wrapper/lock (call RVA 0x8A54)
  3. Load global object from RVA 0x27488 (.data section, **zero in static binary**)
  4. Dispatch through vtable to virtual function
  5. Return via WDF API (IAT@0x1E140)

- Global object is **runtime-initialized** (DriverEntry or device init)
- Cannot determine object type from static binary
- No evidence of `MmMapIoSpace`, `ZwMapViewOfSection`, or large buffer allocations
- Vtable at RVA 0x23F78 contains constructor/destructor-like functions

**Conclusion**: These IOCTLs are **NOT buffer mapping APIs** based on static analysis. Purpose unknown, possibly queue/device state management or unrelated feature.

### Option C: AppleAudio.sys Buffer Path Analysis ✓ COMPLETED

**Method**: Binary search for IOCTL calls, string references, import analysis

**Findings**:
- T2 message send function at RVA 0x4A60 uses IOCTL 0x222018 only
- **Zero occurrences** of IOCTLs 0x222004, 0x222008, 0x22200C, 0x222010, 0x222014 in binary
- Imports `MmMapIoSpaceEx` (IAT RVA 0x1E862) and `MmMapLockedPagesSpecifyCache`
- Usage location of memory mapping functions **not determined** (80+ spurious xrefs)
- Opens `\DosDevices\AppleUSBVHCI` for message path only

**Conclusion**: AppleAudio.sys does NOT use unknown IOCTLs. Buffer access mechanism remains unknown. Likely uses `MmMapIoSpaceEx` to map T2 hardware directly, but call site analysis incomplete.

### Status: Buffer Path NOT RESOLVED

**Last proven chain element**: AppleAudio.sys RVA 0x4A60 → IOCTL 0x222018 → AppleUSBVHCI message handler

**Missing link**: PCM data write mechanism from AppleAudio.sys to T2 hardware

**Next required investigation**:
1. Trace all `MmMapIoSpaceEx` call sites in AppleAudio.sys (2-4 hours static analysis)
2. OR runtime kernel debugging to capture actual memory operations (1-2 hours on hardware)

---

## Buffer Access Investigation Plan

### Option A: Reverse-Engineer Unknown IOCTLs (RECOMMENDED)
AppleUSBVHCI.sys exposes 5 additional IOCTLs (NOTES.md:84-91):
```
0x222004 → RVA 0xDB7C (input: 8 bytes)
0x222008 → RVA 0xDC5C (input: 32 bytes)
0x22200C → RVA 0xDAF0 (input: 8, output: 32 bytes)
0x222010 → RVA 0xDC24 (input: 16 bytes)
0x222014 → RVA 0xDBEC (input: 16 bytes)
```

**Next steps**:
1. Disassemble each handler (RVA 0xDB7C, 0xDC5C, etc.)
2. Identify function signatures and parameters
3. Look for memory mapping operations (MmMapIoSpace, ZwMapViewOfSection)
4. Look for buffer address returns or shared memory handles
5. Create minimal test for each IOCTL with safe dummy parameters

**Tools**: IDA Pro, Ghidra, or x64dbg for static analysis

### Option B: Direct PCI BAR Access (NOT PURSUED - violates constraints)

If AppleUSBVHCI doesn't expose buffers, may need direct PCI access:

**Requirements**:
- WinRing0 or similar kernel-mode helper ❌ violates safety constraints
- PCI device enumeration (VID=0x106B, PID=0x1803)
- Map BAR0 (audio buffers) and BAR4 (config)
- Read GPR at BAR4+0xC000 to locate BufferStruct

**Risks**: 
- Requires unsigned driver or test signing mode ❌ violates constraints
- May conflict with AppleUSBVHCI.sys exclusive PCI access
- More complex than using existing driver abstraction

**Status**: Option rejected due to safety constraints.

### Option D: Runtime Monitoring (NOT PURSUED - out of scope)

Monitor AppleAudio.sys at runtime to observe buffer operations:

**Tools**:
- WinDbg kernel debugging
- ETW (Event Tracing for Windows) if logging available
- Breakpoints on MmMapIoSpaceEx, memory writes

**Goal**: Capture physical addresses, buffer sizes, actual PCM write operations

**Status**: Out of scope for this iteration (static analysis only).

---

## Evidence Classification Summary
