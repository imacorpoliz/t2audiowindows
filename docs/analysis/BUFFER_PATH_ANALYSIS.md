# Buffer Path Analysis - AppleUSBVHCI & AppleAudio Investigation Results

**Investigation Date**: 2026-10-04  
**Binary Versions**:
- AppleUSBVHCI.sys 6.1.7500.5 (SHA256: 0438292090fc5267ca6d28829fee7d16da8b73e7657bdb64250cf8df832955c1) ✓ VERIFIED
- AppleAudio.sys 6.1.8100.4 (SHA256: cd8b81fabc9be6127a082da22c4cc046a4d46e0225eb6baf6d27e0dc9862a150) ✓ VERIFIED

---

## Executive Summary

### What Was Found (CORRECTED ANALYSIS)

1. **AppleUSBVHCI.sys unknown IOCTLs (0x222004-0x222014)**: All handlers dispatch through a single runtime-initialized global object via virtual function calls. Purpose cannot be determined from static binary alone.

2. **AppleAudio.sys message path**: Uses IOCTL 0x222018 exclusively for T2 protocol messages (device enumeration, property queries, stream control). Confirmed at RVA 0x4A60.

3. **AppleAudio.sys does NOT call unknown IOCTLs**: Binary search found zero references to 0x222004, 0x222008, 0x22200C, 0x222010, 0x222014.

4. **AppleAudio.sys DOES use MmMapIoSpaceEx**: **2 calls found** at RVA 0x17204 and 0x1721C in PAGE section (function 0x16FF0). Maps T2 memory resources (BAR1 and BAR2) during device initialization.

5. **Mapped addresses stored in global object 0x14690**: Fields +0x28 (BAR1) and +0x30 (BAR2+0xC000) accessed from 347 code locations.

6. **Buffer metadata read mechanism identified**: Function 0x4700 reads T2 device structures from mapped memory (signature 0x19870423, offset tables, device metadata).

### What Was NOT Found (BLOCKING)

- **PCM audio sample write path**: mapped memory used for **reading metadata**, not writing audio samples
- **WaveSpeaker KS pin → T2 buffer connection**: no link found between PortCls audio callbacks and mapped BAR regions
- **Audio data transmission mechanism**: DMA setup, buffer write loops, or sample copying not identified

**Status**: T2 memory mapping mechanism **FOUND and TRACED**. PCM audio data path remains **UNKNOWN** — requires runtime debugging or additional analysis of global object 0x14690 consumers.

---

## Part A: AppleUSBVHCI.sys IOCTL Analysis

### IOCTL Handler Patterns (STATIC)

All five unknown IOCTLs follow identical dispatch pattern:

```c
// Pseudocode reconstruction from disassembly

NTSTATUS IoctlHandler(WDFREQUEST request, PVOID inputBuffer, SIZE_T inputSize) {
    // Step 1: Adjust input pointer (skip header bytes)
    PVOID adjustedInput = (BYTE*)inputBuffer + headerOffset;
    
    // Step 2: Acquire lock/wrapper
    PVOID wrapperResult = AcquireWrapperFunction(adjustedInput);
    
    // Step 3: Load global object (runtime-initialized)
    PVOID globalObject = *g_RuntimeObject;  // RVA 0x27488 (zero in static binary)
    
    // Step 4: Get vtable from object
    PVOID* vtable = *(PVOID**)globalObject;
    
    // Step 5: Call virtual function with parameters
    PVOID virtualFunc = vtable[vtableOffset];
    return ((FUNC_PTR)virtualFunc)(globalObject, parameters...);
}
```

### Individual IOCTL Details

#### IOCTL 0x222004 (input: 8 bytes)
```
Handler RVA: 0xDB7C
Input structure:
  +0x00: unknown (4 bytes, skipped)
  +0x04: parameter (4 bytes, passed to virtual function)
Vtable offset: +0x38 (index 7)
Operation: Tail call, single 4-byte parameter
```

#### IOCTL 0x222008 (input: 32 bytes)
```
Handler RVA: 0xDC5C
Input structure:
  +0x00..0x1B: context/header (28 bytes)
  +0x1C: parameter (4 bytes)
Vtable offset: +0x28 (index 5)
Operation: Complex call with multiple context fields (+0, +8, +16)
Note: Not a tail call - may return data
```

#### IOCTL 0x22200C (input: 8 bytes, output: 32 bytes)
```
Handler RVA: 0xDAF0
Input structure:
  +0x04: input parameter (4 bytes)
  +0x08: reference/pointer field
  +0x10: possible output buffer pointer
  +0x18: return value written here (32-bit)
Vtable offset: +0x30 (index 6)
Operation: Query operation - reads input, writes output to buffer
```

#### IOCTL 0x222010 (input: 16 bytes)
```
Handler RVA: 0xDC24
Input structure:
  +0x00: dword parameter (4 bytes)
  +0x0C: adjusted pointer field
Vtable offset: +0x10 (index 2)
Operation: Tail call with dword + pointer
```

#### IOCTL 0x222014 (input: 16 bytes)
```
Handler RVA: 0xDBEC
Input structure:
  +0x00: qword parameter (8 bytes)
  +0x0C: adjusted pointer field
Vtable offset: +0x18 (index 3)
Operation: Tail call with qword + pointer
```

### Global Object at RVA 0x27488 (STATIC)

**Location**: .data section, offset 0x25888  
**Static content**: All zeros (uninitialized)  
**Runtime behavior**: Created during driver initialization (DriverEntry or AddDevice)  
**Type**: Unknown - could be:
- WDFQUEUE object
- WDFDEVICE object  
- Custom C++ class managing BCE (Bridge Control Engine) state
- T2 communication context structure

**Critical limitation**: Without runtime analysis, cannot determine:
- Object type
- Vtable structure (may be derived class, not static vtable at RVA 0x23F78)
- Virtual function implementations
- Whether these IOCTLs relate to audio buffers

### Static Vtable at RVA 0x23F78

```
+0x00 → 0x0140E7C0  (index 0)
+0x08 → 0x0140E850  (index 1)  
+0x10 → 0x0140E1C0  (index 2) ← IOCTL 0x222010
+0x18 → 0x0140E180  (index 3) ← IOCTL 0x222014
+0x20 → 0x0140E090  (index 4)
+0x28 → 0x0140E310  (index 5) ← IOCTL 0x222008
+0x30 → 0x0140DE30  (index 6) ← IOCTL 0x22200C
+0x38 → 0x0140E150  (index 7) ← IOCTL 0x222004
```

Preliminary analysis of vtable functions (first 64 bytes each):
- **0xE7C0**: Constructor-like (initializes list heads, allocates sub-objects)
- **0xE850**: Destructor-like (releases WDF objects, zeros pointers)
- **0xE1C0**: Query/validation (checks object state, returns STATUS_DEVICE_NOT_READY if null)

**No evidence of**:
- `MmMapIoSpace` calls
- Large buffer allocation
- PCI BAR access
- DMA setup

### IAT Entry 0x1E140 (STATIC)

All handlers perform indirect calls through IAT@0x1E140. Import table analysis required to identify exact WDF function (likely `WdfRequestComplete` or queue management API).

---

## Part B: AppleAudio.sys T2 Communication Path

### Message Send Function at RVA 0x4A60 (STATIC)

**Signature**:
```c
NTSTATUS SendT2Message(
    void* reserved,
    void* t2RequestBuffer,    // T2 protocol message (21+ bytes)
    ULONG requestSize,
    void* t2ReplyBuffer      // Output buffer for reply
);
```

**Operation** (kernel-mode):
1. Opens `\DosDevices\AppleUSBVHCI` via `IoGetDeviceObjectPointer`
2. Builds IRP with `IoBuildDeviceIoControlRequest`:
   - IOCTL: **0x222018**
   - Input: T2 request message
   - Output: T2 reply buffer
3. Submits via `IoCallDriver`
4. Waits synchronously with `KeWaitForSingleObject`
5. Closes handle

**Key finding**: This is the **message control path only**. Does NOT transmit PCM audio data.

**Confirmed usage** (from hardware test logs):
- Command 101 (GET_DEVICE_LIST)
- Command 26 (GET_OUTPUT_STREAM_LIST)
- Command 7 (GET_PROPERTY)

**Expected but unconfirmed**:
- Command 0 (START_IO)
- Command 2 (STOP_IO)
- Command 32 (SET_REMOTE_ACCESS)

### Critical Finding: No Unknown IOCTL Calls (STATIC)

**Binary search results**:
- IOCTL 0x222004: **NOT FOUND**
- IOCTL 0x222008: **NOT FOUND**
- IOCTL 0x22200C: **NOT FOUND**
- IOCTL 0x222010: **NOT FOUND**
- IOCTL 0x222014: **NOT FOUND**

**Implication**: AppleAudio.sys does NOT use AppleUSBVHCI unknown IOCTLs for buffer registration, mapping, or audio data transfer.

### Memory Mapping: MmMapIoSpaceEx Usage FOUND (VERIFIED)

**Call sites identified** (independent verification confirms):
- **RVA 0x17204** (file offset 0x14804): `FF 15 4E 6E FF FF` → call qword ptr [IAT+0xE058]
- **RVA 0x1721C** (file offset 0x1481C): `FF 15 36 6E FF FF` → call qword ptr [IAT+0xE058]

**Function context** (0x16FF0–0x172FE):
1. Device initialization callback (EvtDevicePrepareHardware)
2. Iterates WDF resource list (CM_RESOURCE_LIST)
3. Filters memory resources (type 3 = CmResourceTypeMemory)
4. Maps **two physical regions**:
   - First resource: cache type 0x404 (MmCached | MmWriteCombined) → stored in `[rbx+0x68]`
   - Second resource: cache type 0x204 (MmNonCached | MmWriteCombined) → stored in `[rbx+0x70]`
5. Copies mapped pointers to **global object 0x14690**:
   - `[global+0x28]` = BAR1 base (first mapped region)
   - `[global+0x30]` = BAR2+0xC000 (second mapped region + offset)

**Physical address sources**: PCI BAR resources from PnP manager (CM_PARTIAL_RESOURCE_DESCRIPTOR.u.Memory.Start)

**Mapped regions identified**:
- **BAR1** (likely BAR0 in Linux): T2 buffer memory region (DMA buffers, BufferStruct metadata)
- **BAR2+0xC000** (likely BAR4+0xC000 in Linux): GPR registers (BufferStruct pointer, signature 0x19870423)

**Usage confirmed**: 347 code locations access `[global+0x28]` and `[global+0x30]` for T2 hardware access

**Detailed analysis**: See [FINAL_REANALYSIS_REPORT.md](../archive/planning/FINAL_REANALYSIS_REPORT.md) Section III and [Linux_Windows_Structure_Mapping.md](Linux_Windows_Structure_Mapping.md)

---

## Part C: Comparison with Linux KAIT2EN

### Linux Buffer Architecture (KNOWN)

```
┌─────────────────────────────────────────────────┐
│ PCI Device (VID:0x106B, PID:0x1803)            │
├─────────────────────────────────────────────────┤
│ BAR0: Audio buffer space (DMA buffers)         │
│ BAR4: Control registers                         │
│   +0xC000: GPR registers                        │
│            → Points to BufferStruct location    │
└─────────────────────────────────────────────────┘
         ↓ ioremap()
┌─────────────────────────────────────────────────┐
│ Kernel virtual memory                           │
│ - BAR0 mapped: DMA output buffers (T2-owned)   │
│ - BAR4 mapped: Read GPR to find BufferStruct   │
│ - BufferStruct: Contains buffer addresses/sizes │
└─────────────────────────────────────────────────┘
         ↓
┌─────────────────────────────────────────────────┐
│ Audio playback flow:                            │
│ 1. Allocate input buffers (DMA coherent)       │
│ 2. Notify T2 via SET_INPUT_STREAM_ADDRESS_RANGES│
│ 3. START_IO command                             │
│ 4. Write PCM samples to mapped buffers          │
│ 5. UPDATE_TIMESTAMP notifications from T2       │
└─────────────────────────────────────────────────┘
```

### Windows Path (UNKNOWN)

**Hypothesis 1**: Similar PCI BAR mapping
- AppleUSBVHCI.sys maps BAR0/BAR4 internally
- Unknown IOCTLs provide usermode/kernelmode access
- **Problem**: No evidence in AppleAudio.sys calling these IOCTLs

**Hypothesis 2**: Shared memory section
- AppleUSBVHCI creates system-wide mapped section
- AppleAudio.sys opens section by name
- Direct writes without explicit IOCTL
- **Problem**: No ZwOpenSection or MmMapViewOfSection found

**Hypothesis 3**: PortCls/KS framework abstraction
- WaveSpeaker pin uses standard KS buffer mechanism
- PortCls miniport passes buffers to AppleAudio
- AppleAudio internally handles buffer→T2 transfer
- **Problem**: Where is the actual DMA write or copy?

**Hypothesis 4**: Message-based transfer (unlikely)
- Audio data sent via IOCTL 0x222018 messages
- T2 protocol allows embedded data payloads
- **Problem**: 
  - 4KB message limit too small for audio buffers
  - Stereo playback works (proven) without buffer mapping user-visible
  - Would cause massive CPU/bus overhead

**Hypothesis 5**: Different T2 firmware mode on Windows
- Windows T2 firmware uses different buffer protocol
- No direct memory mapping required
- Some abstracted mechanism
- **Problem**: Pure speculation, no evidence

---

## Part D: Unknown vs. Confirmed

### HARDWARE-TESTED (from previous sessions)

✓ T2 reports 6-channel speaker capability (device 0x39, stream 0x3A)  
✓ Physical format: 48kHz, 6ch, 24-bit signed PCM, interleaved  
✓ IOCTL 0x222018 works for T2 protocol messages  
✓ Commands 101, 26, 7 confirmed functional  
✓ AppleAudio.sys WaveSpeaker locked to stereo (Win32 error 1169 for 6ch attempts)  
✓ Stereo playback works through AppleAudio.sys (standard Windows operation)

### STATIC ANALYSIS (this session, corrected with independent verification)

✓ SHA-256 hashes verified for both binaries  
✓ IOCTLs 0x222004-0x222014 dispatch through global object+vtable  
✓ AppleAudio.sys calls IOCTL 0x222018 at RVA 0x4A60  
✓ AppleAudio.sys does NOT call unknown IOCTLs  
✓ **MmMapIoSpaceEx called 2× in AppleAudio.sys** (RVA 0x17204, 0x1721C in PAGE section)  
✓ **Mapped T2 BAR1 and BAR2** stored in global object 0x14690 (+0x28, +0x30)  
✓ **347 code locations access mapped memory** through global object  
✓ **Function 0x4700 reads T2 metadata** from mapped buffers (magic 0x19870423)  
✓ Global object at RVA 0x27488 (AppleUSBVHCI) is zero in static binary (runtime-initialized)  
✓ All IOCTL handlers use same object and WDF completion path

### HYPOTHESIS (requires validation)

? Unknown IOCTLs are NOT audio buffer APIs  
? AppleAudio.sys uses MmMapIoSpace to access T2 buffers directly  
? Physical address comes from PCI device enumeration  
? Buffer location negotiated during driver initialization  
? PortCls framework handles buffer management internally  
? Stereo→6ch upmix may happen in T2 firmware (not host)

### UNKNOWN (BLOCKING)

✗ **PCM sample write path** - mapped memory used for metadata reads (function 0x4700), PCM write mechanism not found  
✗ **WaveSpeaker → T2 buffer link** - no connection between PortCls callbacks and BufferStruct stream buffers identified  
✗ **6-channel path** - can native access skip stereo limitation?  
✗ **Unknown IOCTL purpose** - queue management? device state? unrelated feature?  
✗ **Runtime object type** at 0x27488 - what creates it?

---

## Part E: Structure Definition Corrections

### Error in Previous Documentation (CONFIRMED)

NOTES.md T2Request structure at lines 112-119 **MISSING FIELD AT OFFSET 17**.

**Proof from T2NativeProbe.cs**:
- Line 28: `Put32(b,0,tag); b[4]=1; Put64(b,5,device); Put32(b,13,command);`
- Line 64-67: Property reply reads:
  - `Get64(response,21)` - object_id echo
  - `Get32(response,41)` - value_size (**absolute offset 41, not 37**)
  - `response[49...]` - value data (**absolute offset 49, not 41**)

**Corrected structures** (from NOTES.md lines 110-152 appear correct):

```c
// Request header: 21 bytes
struct T2Request {
    uint32 tag;            // offset 0
    uint8  type;           // offset 4 (always 1)
    uint64 device_id;      // offset 5
    uint32 command;        // offset 13
    uint32 unknown_field;  // offset 17 ← WAS MISSING in older docs
    uint8  payload[];      // offset 21
};

// Reply header: 21 bytes
struct T2Reply {
    uint32 tag;            // offset 0
    uint8  type;           // offset 4 (always 2)
    uint64 device_id;      // offset 5
    uint32 reply_command;  // offset 13
    uint32 status;         // offset 17
    uint8  payload[];      // offset 21
};

// Property request payload (starts at absolute offset 21)
struct PropertyRequest {
    uint64 object_id;      // payload+0  → absolute 21
    uint32 unknown_0;      // payload+8  → absolute 29
    uint32 scope;          // payload+12 → absolute 33 ('glob')
    uint32 selector;       // payload+16 → absolute 37 ('pft ')
    uint64 unknown_1;      // payload+20 → absolute 45
};  // Total: 28 bytes payload → absolute request size 49 bytes

// Property reply payload (starts at absolute offset 21)
struct PropertyReplyPayload {
    uint64 object_id;      // payload+0  → absolute 21
    uint32 unknown_0;      // payload+8  → absolute 29
    uint32 scope;          // payload+12 → absolute 33
    uint32 selector;       // payload+16 → absolute 37
    uint64 value_size;     // payload+20 → absolute 41 ← PROVEN by T2NativeProbe.cs:67
    uint8  value[];        // payload+28 → absolute 49 ← PROVEN by T2NativeProbe.cs:70
};
```

**Verification**: T2NativeProbe.cs lines 64-70:
```csharp
if(Get64(response,21)!=obj || Get32(response,29)!=0 ||
    Get32(response,33)!=0x676c6f62 || Get32(response,37)!=selector)
    throw new InvalidOperationException("Property response does not match request.");
ulong size=Get64(response,41);  // ← size at absolute 41
Need(response,49,(int)size);
byte[] value=new byte[(int)size]; Array.Copy(response,49,value,0,value.Length);  // ← data at 49
```

**Status**: NOTES.md lines 110-152 already contain corrected structures. No further fix needed.

---

## Part F: Next Steps & Unresolved Blocks

### To Resolve Buffer Path (Priority Order)

**1. ✓ COMPLETED: Trace MmMapIoSpaceEx calls in AppleAudio.sys**
- Call sites: RVA 0x17204, 0x1721C in function 0x16FF0
- Physical addresses: PCI BAR resources from CM_RESOURCE_LIST
- Mapped regions: BAR1 (buffers) and BAR2+0xC000 (GPR registers)
- Storage: Global object 0x14690 fields +0x28 (BAR1) and +0x30 (BAR2+0xC000)

**2. Trace BufferStruct parsing and stream buffer setup** (HIGH)
- Analyze 347 consumers of `[global+0x28]` and `[global+0x30]`
- Find functions reading BufferStruct.devices[].output_streams[].buffers[]
- Identify where stream buffer offsets (from BufferStruct) are added to BAR1 base
- Look for pattern: `mov rax, [global+0x28]; add rax, <buffer_offset>; memcpy/write loop`
- **Estimated effort**: 4-8 hours static analysis of global object consumers

**3. Runtime kernel debugging** (HIGH, requires hardware)
- Attach WinDbg to live system
- Set breakpoint on buffer consumers: `ba r8 AppleAudio!global_object+0x28`
- Capture functions reading BAR1 base during audio playback
- Monitor memory writes to BAR1 region: `ba w4 <BAR1_address>+<offset>`
- **Estimated effort**: 1-2 hours if successful driver symbol loading

**4. Search for WaveSpeaker pin callbacks in AppleAudio.sys** (MEDIUM)
- Find IMiniportWaveRT/IMiniportWaveCyclic vtable implementations
- Trace GetBuffer/NewStream callbacks
- Look for PCM buffer handling and writes to mapped memory
- Search for pattern: buffer pointer arithmetic with BAR1 base + BufferStruct offset
- **Estimated effort**: 3-5 hours static analysis

**5. Port Linux buffer access logic to Windows** (ALTERNATIVE APPROACH)
- Use proven KAIT2EN BufferStruct parsing logic (audio.c:574-642, 710-741)
- Implement minimal WaveRT driver with direct mapped memory access
- Test read-only operations first (enumerate devices, validate BufferStruct)
- **Estimated effort**: 40-60 hours for proof-of-concept driver (see Linux_Windows_Structure_Mapping.md Section 10)

**6. Analyze AppleUSBVHCI DriverEntry/AddDevice** (LOW)
- Find initialization of global object at 0x27488
- Identify object type from allocation size + vtable assignment
- Determine if PCI resources are mapped during init
- **Estimated effort**: 2-3 hours static analysis

**7. AppleUSBVHCI import table analysis** (LOW)
- Identify IAT@0x1E140 function name
- Check if driver exports any functions (for AppleAudio to call directly)
- **Estimated effort**: 30 minutes

### Constraints (from task requirements)

✓ Only static analysis (no StreamControlTest execution)  
✓ No START_IO/STOP_IO testing  
✓ No unknown IOCTL probing  
✓ No driver disable or system modification  
✓ No WinRing0 or physical memory tools  

**Implication**: Without runtime analysis or executing unknowns, cannot fully resolve buffer path from binaries alone.

### Minimum Next Experiment

**Recommended approach** (based on Linux-Windows structure mapping analysis):

**Option A: Continue static analysis of global object consumers** (safe, no hardware risk)
1. Analyze functions accessing `[global+0x28]` (BAR1 base pointer)
2. Find BufferStruct parser and stream buffer offset calculation
3. Identify PCM write loop or PortCls callback linkage
4. **Risk**: May not find mechanism if hidden in vtable dispatch or firmware abstraction

**Option B: Port Linux buffer logic** (requires development, controlled risk)
1. Create minimal WDF driver based on KAIT2EN audio.c structure definitions
2. Implement read-only BufferStruct parsing (GPR read, signature validation)
3. Test device enumeration without touching audio buffers
4. Gradually add PCM write based on proven Linux logic
5. **Risk**: Medium - starts with safe read operations, controlled progression

**Option C: Runtime debugging** (requires test hardware)
1. Attach WinDbg kernel debugger to MacBook Pro running Windows
2. Set read-access breakpoint on global object BAR1 pointer
3. Trigger audio playback, capture callstack of buffer access functions
4. **Risk**: Low if only monitoring (no code injection)

**Cannot recommend blind experimentation** (START_IO without buffer setup) until PCM write path identified.

**Current status**: T2 hardware access mechanism **FOUND** (MmMapIoSpaceEx in function 0x16FF0, global object 0x14690). BufferStruct layout **KNOWN** from Linux. PCM write path **REMAINS UNKNOWN** — likely in one of 347 global object consumers or abstracted by PortCls framework.

---

## Part G: Conclusion

### Proven Chain (Message Path)

```
AppleAudio.sys RVA 0x4A60
    ↓ IoBuildDeviceIoControlRequest
    ↓ IOCTL 0x222018
AppleUSBVHCI.sys RVA 0x2128
    ↓ T2 message encoding
T2 Chip (protocol messages only)
```

**Status**: ✓ COMPLETE for control messages

### Unknown Chain (Data Path)

```
WaveSpeaker Pin (stereo PCM)
    ↓ PortCls framework (portcls.sys)
AppleAudio.sys internal processing
    ↓ Function 0x16FF0: MmMapIoSpaceEx → BAR1/BAR2 mapped
    ↓ Global object 0x14690: stores BAR1@+0x28, BAR2+0xC000@+0x30
    ↓ Function 0x4700: reads BufferStruct metadata (magic 0x19870423)
    ↓ ??? (PCM write to BAR1 + buffer_offset - NOT FOUND)
    ↓ ??? (347 consumers of global object - requires further analysis)
T2 Chip (audio DMA buffers on BAR1)
    ↓ 
Six physical speakers (working in stereo mode)
```

**Status**: ⚠ PARTIAL - hardware access mechanism found, PCM write path missing

### Answer to Task Questions

**1. Was the path to the PCM buffer found?**
PARTLY. The T2 hardware access mechanism was found:
- ✓ MmMapIoSpaceEx calls in function 0x16FF0 (RVA 0x17204, 0x1721C)
- ✓ Mapped BAR1 (buffer memory) and BAR2+0xC000 (GPR registers)
- ✓ The global object 0x14690 holds the mapped addresses (+0x28, +0x30)
- ✓ 347 code locations use the mapped memory
- ✓ Function 0x4700 reads the BufferStruct metadata (signature 0x19870423)
- ✗ The mechanism that writes PCM samples into the buffer was NOT FOUND (probably in one of the 347 consumers; later found: the copy loop `0x14000A715`–`0x14000B2C0`, see `RE_appleaudio_protocol.md` §3)

The message path through IOCTL 0x222018 is fully confirmed.

**2. Is it reachable from user mode?**
NOT directly. AppleAudio.sys runs in kernel mode; MmMapIoSpaceEx creates a kernel-only mapping. 
User-mode access is possible only through:
- A kernel-mode driver with its own IOCTL interface, OR
- WaveRT shared memory mode (if AppleAudio used it to export the buffer)

**3. What else is unknown?**
- The PCM write mechanism: the function copying samples into BAR1 + buffer_offset
- The WaveSpeaker → BufferStruct link: how the PortCls callbacks are tied to the T2 stream buffers
- BufferStruct stream buffer parsing: the function reading the devices[].output_streams[].buffers[] offsets
- Whether the stereo limitation can be bypassed: does it need an entirely new kernel driver (it did: T2AudioPort)
- Experiment safety: START_IO without a correct buffer setup could cause a hardware fault

**4. Mapping to Linux KAIT2EN?**
✓ DONE. A detailed structure mapping table is in [Linux_Windows_Structure_Mapping.md](Linux_Windows_Structure_Mapping.md):
- The BAR layout matches (BAR0/BAR4 on Linux ↔ the first/second memory resource on Windows)
- The GPR structure matches (version, signature 0x19870423, BufferStruct offset)
- The BufferStruct device stride 0xBDEC matches exactly
- Linux uses zero-copy ALSA → mapped BAR0 for PCM writes
- The Windows PCM write mechanism was not found, but the hardware access is identical

### Last Proven Point in Chain

**AppleAudio.sys maps T2 hardware memory via MmMapIoSpaceEx (function 0x16FF0) and stores pointers in global object 0x14690:**
- `[global+0x28]` = BAR1 base (T2 buffer memory region)
- `[global+0x30]` = BAR2+0xC000 (GPR registers with BufferStruct pointer)

**Function 0x4700 reads T2 BufferStruct metadata** from mapped memory (signature 0x19870423, device names, buffer offsets).

Everything after metadata reading (PCM sample write to buffer) is unresolved.

### Next Unresolved Call

**One of 347 code locations accessing global object fields +0x28/+0x30** likely performs PCM buffer writes, but specific function not yet identified.

**Required**: Analyze consumers of `[global+0x28]` (BAR1 pointer) to find:
- BufferStruct.devices[].output_streams[].buffers[].address offset read
- Pointer arithmetic: `BAR1_base + buffer_offset`
- memcpy/write loop to computed address

This is the **next concrete blocking point** for further analysis.
