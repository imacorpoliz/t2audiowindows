# Summary of the research into 6-channel audio on the MBP 16,1

**Date**: 2026-10-04  
**System**: MacBookPro16,1 (2019), Apple T2, Windows 11

> Archived: the earliest research notes. The buffer access blocker described here was solved later (the speaker ring is in the T2's BAR0, found through GPR/BufferStruct; see `docs/DRIVER_GUIDE.md`).

---

## What was done today

### 1. Analysis of the project's current state
- Studied all research files (NOTES.md, T2_Audio_Protocol_Analysis.md, T2_Windows_Implementation_Guide.md)
- Confirmed: through AppleUSBVHCI we got the Speaker stream ID 0x3A with the format 48 kHz, 6 channels, signed 24-bit PCM in a 32-bit container
- Determined: audio data transfer and independent channel control are unverified

### 2. Identifying the critical blocker
**Problem**: the way to access the audio buffers is unknown

**On Linux (KAIT2EN)**:
- PCI BAR0 is used to map the DMA buffers
- The GPR registers at BAR4+0xC000 point to BufferStruct
- BufferStruct holds the buffer addresses and sizes

**On Windows**:
- AppleUSBVHCI.sys hides PCI access behind its API
- The buffer access method is unknown
- 5 undocumented IOCTLs (0x222004–0x222014) need reverse engineering

### 3. The StreamControlTest tool was created
**Purpose**: check that START_IO/STOP_IO commands are accepted through IOCTL 0x222018

**Safety**:
- ✅ Only sends control commands
- ✅ Does NOT touch the buffers
- ✅ Does NOT send audio data
- ✅ Duration <100 ms

**Files**:
- `MBP-Stream-Control-Test/StreamControlTest.cs`: the main code
- `MBP-Stream-Control-Test/Program.cs`: entry point
- `MBP-Stream-Control-Test/README.txt`: documentation

**Status**: the code is ready but not compiled (no .NET SDK)

### 4. Documentation updated

**NOTES.md**:
- ✅ Critical unknowns detailed with priorities
- ✅ Added a "Buffer Access Investigation Plan" section with 4 approaches:
  1. Reverse engineering the unknown IOCTLs (recommended)
  2. Direct PCI BAR access
  3. Analysis of AppleAudio.sys
  4. Runtime monitoring
- ✅ Added a session summary with the priorities of the next steps

**CURRENT_STATUS.md** (new file):
- A short project overview in English
- What is proven, what is unknown
- Current blockers and next steps
- Safety limits

---

## Key conclusions

### ✅ Proven
1. **The 6-channel hardware exists**: the T2 reports the format 48 kHz, 6 channels
2. **The T2 protocol is reachable**: IOCTL 0x222018 works for the commands GET_DEVICE_LIST, GET_OUTPUT_STREAM_LIST, GET_PROPERTY
3. **The public API is locked**: AppleAudio.sys WaveSpeaker gives stereo only

### ❌ Blocker
**Access to the audio buffers is unknown**

Without it we cannot:
- Write PCM data into the buffer
- Start playback
- Check independent channel control

### 🔍 Next steps (priority)

**1. HIGH**: reverse engineer the unknown IOCTLs in AppleUSBVHCI.sys
   - Goal: find the buffer mapping functions
   - Addresses: RVA 0xDB7C, 0xDC5C, 0xDAF0, 0xDC24, 0xDBEC
   - Tools: IDA Pro, Ghidra, x64dbg

**2. MEDIUM**: run StreamControlTest
   - Needs: .NET SDK 6.0 or Visual Studio
   - Goal: check whether the T2 accepts START_IO/STOP_IO
   - Safe: no buffer access

**3. LOW**: monitor AppleAudio.sys at runtime
   - WinDbg to track the calls into AppleUSBVHCI
   - May reveal the IOCTLs in use

---

## Safety limits

**Before ANY audio transfer**:
- ✅ Confirm the buffer access method
- ✅ Implement UPDATE_TIMESTAMP handling (play position)
- ✅ Add volume attenuation (-30 dB at least)
- ✅ Limit the test duration (100-200 ms maximum)
- ✅ Single-channel test tones only (to find the physical mapping)

**Do NOT**:
- ❌ Call unknown IOCTLs without analysis
- ❌ Write to unknown memory addresses
- ❌ Send unattenuated audio signals
- ❌ Assume the channel order without checking

---

## Project structure

```
mbp/
├── NOTES.md                          — main research log
├── CURRENT_STATUS.md                 — short status (English)
├── T2_Audio_Protocol_Analysis.md     — T2 protocol documentation (from KAIT2EN)
├── T2_Windows_Implementation_Guide.md — Windows implementation patterns
├── MBP-Audio-Probe/                  — KS format validation (stereo)
├── MBP-Audio-Probe-v2/               — extended version
├── MBP-T2-Native-Probe/              — descriptor reading (6ch confirmed)
│   └── INVESTIGATION.md              — static analysis of AppleUSBVHCI.sys
└── MBP-Stream-Control-Test/          — START_IO/STOP_IO test (NEW)
    ├── StreamControlTest.cs          — main code
    ├── Program.cs                    — entry point
    ├── README.txt                    — documentation
    └── MBP-Stream-Control-Test.csproj — .NET 6.0 project
```

---

## Summary

**Progress**: a safe path for control commands is established; buffer access is identified as the blocking problem.

**Ready for the next stage**: reverse engineering AppleUSBVHCI.sys to find the buffer mapping functions.

**Risks**: without buffer access the project cannot move on to audio transfer. This is the critical blocker.

**Recommendation**: start by reverse engineering IOCTL 0x222004–0x222014 (Option A in NOTES.md).
