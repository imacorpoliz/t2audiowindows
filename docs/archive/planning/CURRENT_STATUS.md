# MacBook Pro 16,1 Six-Channel Audio Research — Current Status

**Date**: 2026-10-04  
**System**: MacBookPro16,1 (2019), Apple T2, Windows 11

---

## ✅ What We Know (PROVEN)

### 1. Hardware Capability
- **6-channel speaker system** exists and is reported by T2 chip
- Stream ID: 0x3A (Speaker device 0x39)
- Format: 48 kHz, 6 channels, signed PCM 24-bit in 32-bit containers
- Confirmed via GET_PROPERTY command through AppleUSBVHCI.sys

### 2. Protocol Access
- **IOCTL 0x222018** works for T2 message exchange
- Successfully tested commands:
  - 101 (GET_DEVICE_LIST)
  - 26 (GET_OUTPUT_STREAM_LIST)  
  - 7 (GET_PROPERTY)
- Message format validated against KAIT2EN Linux driver

### 3. Public API Limitation
- AppleAudio.sys WaveSpeaker: **locked to stereo only**
- All 6-channel KS format attempts rejected (Win32 error 1169)
- Native 6ch access must bypass public KS interface

---

## ❌ Critical Blocker: Buffer Access Unknown

### The Problem
Cannot transmit audio data without knowing how to access DMA buffers.

### Linux Solution (KAIT2EN)
- Maps PCI BAR0 (audio buffer space)
- Reads GPR registers at BAR4+0xC000 to locate BufferStruct
- BufferStruct contains DMA buffer addresses and sizes
- Output buffers: Pre-allocated by T2, mapped from BAR0
- Input buffers: Host-allocated, notified to T2 via SET_INPUT_STREAM_ADDRESS_RANGES

### Windows: Unknown Path
AppleUSBVHCI.sys likely abstracts PCI access, but method unclear:

**Option A**: Unknown IOCTLs provide buffer mapping
```
0x222004 → RVA 0xDB7C (input: 8 bytes)
0x222008 → RVA 0xDC5C (input: 32 bytes)
0x22200C → RVA 0xDAF0 (input: 8, output: 32 bytes)
0x222010 → RVA 0xDC24 (input: 16 bytes)
0x222014 → RVA 0xDBEC (input: 16 bytes)
```
**Requires**: Static analysis to identify purpose

**Option B**: Direct PCI BAR access required  
**Requires**: Kernel driver or WinRing0-style helper

**Option C**: Different mechanism entirely  
**Requires**: Reverse-engineer AppleAudio.sys buffer path

---

## 🔨 Tools Created

### MBP-Stream-Control-Test (NEW)
**Purpose**: Test START_IO/STOP_IO command acceptance  
**Safety**: No buffer access, no audio data transmission  
**Status**: Code complete, not yet compiled/run (no .NET SDK)

**What it does**:
1. Enumerate devices via GET_DEVICE_LIST
2. Find Speaker device (0x39)
3. Send START_IO (command 0)
4. Immediately send STOP_IO (command 2)
5. Log all responses

**Expected outcomes**:
- START_IO succeeds → T2 allows control without buffer setup
- START_IO fails with error → Buffer allocation required first
- Either result provides protocol sequence information

### Previous Tools
- MBP-Audio-Probe: KS format validation (stereo only)
- MBP-T2-Native-Probe: Descriptor reading (6ch confirmed)

---

## 📋 Next Steps (Priority Order)

### 1. HIGH: Reverse-Engineer Unknown IOCTLs
**Goal**: Identify buffer access mechanism in AppleUSBVHCI.sys

**Steps**:
1. Disassemble handlers at RVA 0xDB7C, 0xDC5C, 0xDAF0, 0xDC24, 0xDBEC
2. Look for memory mapping calls (MmMapIoSpace, ZwMapViewOfSection)
3. Identify parameter structures and return values
4. Document safe calling conventions

**Tools**: IDA Pro, Ghidra, or x64dbg

### 2. MEDIUM: Run StreamControlTest
**Goal**: Verify START_IO/STOP_IO acceptance by T2

**Requirements**:
- .NET SDK 6.0+ or Visual Studio
- Administrator privileges
- Boot Camp Windows 11 on MBP 16,1

**Value**: Determines if buffer setup is prerequisite for stream control

### 3. LOW: Monitor AppleAudio.sys Runtime
**Goal**: Observe working stereo driver's buffer access pattern

**Methods**:
- WinDbg kernel debugging
- ETW trace logging
- IOCTL capture via API Monitor

**Value**: May reveal AppleUSBVHCI buffer API in use

---

## ⚠️ Safety Constraints

**Before ANY audio transmission**:
- ✅ Confirm buffer access method
- ✅ Implement position reporting (UPDATE_TIMESTAMP handling)
- ✅ Add volume attenuation (-30 dB minimum)
- ✅ Limit test duration (100-200ms maximum)
- ✅ Single-channel test tones only (to identify physical mapping)
- ✅ Emergency stop mechanism ready

**Do NOT**:
- ❌ Call unknown IOCTLs without analysis
- ❌ Write to unmapped memory addresses
- ❌ Send unattenuated audio signals
- ❌ Run extended tests without position tracking
- ❌ Assume channel mapping without verification

---

## 🎯 End Goal

**Phase 1**: Raw 6-channel PCM access via T2 native protocol  
**Phase 2**: Port KAIT2EN DSP processing (crossovers, EQ, limiting)  
**Phase 3**: Integration with Windows audio stack (APO or WASAPI)

**Current Phase**: 1 (blocked on buffer access)

---

## 📚 Key References

- `NOTES.md`: Comprehensive research log
- `T2_Audio_Protocol_Analysis.md`: Linux KAIT2EN protocol documentation
- `T2_Windows_Implementation_Guide.md`: Windows driver implementation patterns
- `MBP-T2-Native-Probe/INVESTIGATION.md`: Static analysis of AppleUSBVHCI.sys

---

## Questions?

**Why not use AppleAudio.sys?**  
It only exposes stereo. The 6-channel capability is hidden at the native T2 protocol level.

**Is this safe?**  
Current work (descriptor reading, command testing) is read-only and safe. Audio transmission will require careful validation and safety limits.

**When will it work?**  
Unknown. Buffer access is the critical blocker. Reverse-engineering effort required.

**Can this damage hardware?**  
Not at current stage (no audio data). Future audio tests must include attenuation and duration limits to prevent speaker damage.
