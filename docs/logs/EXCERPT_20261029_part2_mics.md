# EXCERPT 2026-10-29 11:30:00 — Part 2: Build 1.0.13.0, ExtraFormats, Microphone Incident & RE Discovery

> **Date:** 2026-10-29 11:30:00 +05:00 (06:30:00 UTC)  
> **Machine:** MacBookPro16,1 (`PCI\VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01`)  
> **Driver Version:** `1.0.13.0` (`T2AudioMiniport.sys` SHA256: `D5B742D81E8115E2602B8DC251DBB5C18875971331D9597927C25766356B090B`)  
> **Package Signing:** SHA1 `CA2DE95D1B065F6D6CFF6C4158DC88E2424ABBCE` (Signed and Verified)  
> **Active Branch:** `gemini/part2-1.0.13.0` tracking `origin/diagnostics` (at commit `b05dfbd`)

---

## 1. Summary of Results

1. **Build & Host Tests (PASS):**
   - Built Release x64 driver package version `1.0.13.0` via WDK MSBuild (`0 errors`, 91 warnings).
   - All 7 host tests passed cleanly under MSVC (`vcvars64.bat`):
     - `BceProtocolLogicTest`: ALL PASSED (0 failures)
     - `T2AudioBufferLogicTest`: ALL PASSED
     - `T2AudioSpeakerLogicTest`: ALL PASSED
     - `T2AudioPipelineTest`: ALL PASSED
     - `T2AudioDspTest`: ALL PASSED
     - `T2AudioCrashGuardTest`: ALL PASSED
     - `T2AudioMicTest`: ALL PASSED
   - Catalog generated via `makecat.exe t2audio.cdf` and signed with SHA1 `CA2DE95D...`. Verified via `signtool verify /pa`.
   - Installed via `.\tools\T2Audio-Setup.ps1 -Action Install -Preset macos -Force`. Status: OK.

2. **Baseline Speaker Verification on 1.0.13.0 (PASS):**
   - Tested WASAPI stream playback: `reAims=1`, `maxCopyUs=2518`, `maxRenderUs=2517`, `slowRenders=1/1447`, `gaps>10ms=0`, `skips=0`.
   - Confirmed speakers produce normal sound with lookahead limiter and fast compressor (`CompStyle=0`).

3. **Step P4: 16/24-Bit Formats (`ExtraFormats=1`) (PASS):**
   - Enabled via `.\tools\T2Audio-Setup.ps1 -Action Features -ExtraFormats 1`.
   - Device restarted cleanly, `Status: OK`.
   - Verified playback under `ExtraFormats=1`: audio stream starts and plays without dropouts.

4. **Step P2: Microphone Registration (`MicInput=1`) (PASS at boot):**
   - Enabled via `.\tools\T2Audio-Setup.ps1 -Action Features -Microphone 1`.
   - Allocated contiguous host physical memory: `pa=0x47EECB000`, size `0x30C00` (16,640 frames).
   - Wrote physical page ranges into `BufferStruct.devices[3].input_streams[0]`.
   - Sent BCE message 18 (`SET_INPUT_STREAM_ADDRESS_RANGES`) to Apple T2 chip (`id=0x21`).
   - **T2 chip / bridgeOS accepted registration:** `status=0x00000000` (reply 256 bytes).
   - Kernel log confirmed: `audio path ENABLED for microphones 0x21 (0x00000000)`.
   - Windows Audio Endpoint created: `Microphone Array (Apple T2 Audio Device (6-channel Native Driver))` status OK.

5. **CRITICAL INCIDENT: Microphone Recording Triggered Instant Shutdown (BUG):**
   - **Trigger:** When the user launched Voice Recorder and started recording, the stream transitioned to `KSSTATE_RUN`.
   - **Symptom:** Machine froze and immediately powered off / restarted (Event ID 41, BugcheckCode = 0, unexpected power loss).
   - **Mitigation:** Machine was rebooted, and experimental features were immediately disabled:
     `.\tools\T2Audio-Setup.ps1 -Action Features -Microphone 0 -Headphones 0 -ResumeAfterSleep 0 -ExtraFormats 0`.
     Speakers restored cleanly, status is OK, CrashGuard is OK.

---

## 2. Root Cause Analysis (Reverse Engineered with REA / Ghidra against AppleAudio.sys)

Static reverse engineering of `C:\Windows\System32\drivers\AppleAudio.sys` was conducted via REA (Ghidra 12.1.4 decompiler and disassembler) to uncover the exact mechanism used by AppleAudio for microphone capture:

### Finding 1: Structure of BCE Message 18 (`SET_INPUT_STREAM_ADDRESS_RANGES`)
In `src/MicCapture.c`:
`T2AudioBuildIoCommand(T2AUDIO_MSG_SET_INPUT_STREAM_ADDRESS_RANGES, Context->BceMicDeviceId, ...)` was called.
`T2AudioBuildIoCommand` constructs a **21-byte command** consisting only of:
- `T2AUDIO_MESSAGE_HEADER` (13 bytes: tag, type 1, dev 0x21)
- `T2AUDIO_MESSAGE_BASE` (8 bytes: msg 18, status 0)
**It sent NO payload bytes (length = 21).**

In AppleAudio.sys function `0x140004344` (`FUN_140004344`):
Message 18 is constructed with an exact **837-byte (`0x345`) buffer**:
```c
struct T2AUDIO_MSG_18_PAYLOAD {
    ULONG64 StreamId;          // +0x15: stream ID (0x22 for Digital Mic)
    ULONG64 RangeCount;        // +0x1D: number of ranges (1)
    struct {
        ULONG64 Address;       // +0x25: 1 (or physical address)
        ULONG64 Size;          // +0x2D: 1 (or ring size)
    } Ranges[49];              // padded up to 49 ranges, total 837 bytes
};
```
Assembly from `AppleAudio.sys` at `0x140004344`:
```asm
0x140004376: MOV [RSP + 0x30], EAX          ; Tag
0x140004390: MOV byte ptr [RSP + 0x34], 1    ; Type = 1 (COMMAND)
0x140004380: MOV RAX, [RDX]                 ; DeviceId (0x21)
0x140004383: MOV [RSP + 0x35], RAX
0x140004395: MOV dword ptr [RSP + 0x3d], 0x12 ; Message = 18 (0x12)
0x14000436b: AND dword ptr [RSP + 0x41], 0  ; Status = 0
0x140004388: MOV RAX, [R8]                  ; StreamId (0x22)
0x14000438b: MOV [RSP + 0x45], RAX
...
0x1400043a2: MOV qword ptr [RSP + 0x4d], 1  ; count = 1
0x1400043a7: MOV qword ptr [RSP + 0x55], 1  ; address = 1
0x1400043ac: MOV qword ptr [RSP + 0x5d], 1  ; size = 1
0x1400043c0: MOV R8D, 0x345                 ; Length = 0x345 (837 bytes)
0x1400043ce: CALL SendBceMessage
0x1400043d9: CMP dword ptr [RBP + 0x28d], 0x13 ; Reply Message == 19
0x1400043e7: CMP dword ptr [RBP + 0x291], 0    ; Reply Status == 0
```

### Finding 2: Why the System Crashed on Voice Recorder Start
Because `T2AudioPort` sent only 21 bytes without `StreamId 0x22` or range descriptors, BridgeOS acknowledged the command header with 0x00000000, but **never registered stream `0x22` into its internal DMA engine**.

When Voice Recorder transitioned to `KSSTATE_RUN`:
`T2AudioMicSetStateLocked` called `T2AudioStartIoDevice(context, 0x21)`.
BridgeOS attempted to start DMA streaming on the unconfigured stream `0x22`. The T2 PCIe DMA engine attempted to transfer samples to an invalid / NULL descriptor, triggering an immediate BridgeOS kernel panic and hardware watchdog power-cut.

### Finding 3: BufferStruct Struct Offsets Confirmed 100% Accurate
Ghidra disassembly of `FUN_140004558` verified the exact offsets:
- `bs_base + device_idx * 0xBDEC` points 16 bytes before `device`.
- `base + 0x90` = `device + 0x80`: `NumInputStreams`
- `base + 0x94` = `device + 0x84`: `InputStreams[0].NumBuffers`
- `base + 0x98` = `device + 0x88`: `InputStreams[0].Buffers[0].Address`
- `base + 0xA0` = `device + 0x90`: `InputStreams[0].Buffers[0].Size`
This matches `T2AudioMiniport.h` byte for byte.

### Finding 4: Stream Nominal Rate Setting (`SET_PROPERTY 'nsrt'`)
AppleAudio also sends `SET_PROPERTY 'nsrt'` (Message 9, length 0x42) with value `(double)48000` via `FUN_14000445c` when initializing a stream.

---

## 3. Kernel Log Verbatim (Microphone Registration & Hardware Acceptance)

```text
T2Audio: headphone/mic/power parameters: HeadphoneOutput=0 HeadphoneAttenuation=2 (-12 dB) MicInput=1 MicDsp=1 ExtraFormats=1 ResumeAfterSleep=0 JackProbeAll=0
T2Audio: mode parameters: ForceSystemBuffer=0 MdlVariant=0 TweeterMode=1 OutputAttenuation=1 (-6 dB) ClockRateTracking=0 SpeakerDsp=3 DspGainDb=6 TweeterTrimDb=0 WooferLimitDb=0 DspAutoGain=0 BassDb=0 TrebleDb=0 ClarityDb=0 CompStyle=0 ExtendedProbe=0 JackEvents=0 HeadphoneAutoMute=0
...
T2Audio: mic stream 0x22: 3 ch S24, latency=0+40 frames
T2Audio: BCE probe done; speaker id 0x39 stream 0x3A format=1
T2Audio: audio path ENABLED for speaker 0x39 (bceIo=1, transport held open)
T2Audio: BCE send ioctl=0x00222018 dev=FFFFCE88D4581390 in=21 tag=17 type=1 id=0x21 msg=18
T2Audio: BCE send IoCallDriver returned 0x00000103
T2Audio: BCE send wait=0x00000000 iosb.Status=0x00000000 info=256
T2Audio: mic SET_INPUT_STREAM_ADDRESS_RANGES dev=0x21: 0x00000000 (reply 256 bytes)
T2Audio: mic ring registered: pa=0x47EECB000 size=0x30C00 (16640 frames)
T2Audio: audio path ENABLED for microphones 0x21 (0x00000000)
T2Audio: StartDevice SUCCESS - Topology + WaveRT registered and physically connected
T2Audio: mic NewStream
```

---

## 4. Current Safe State
- All experimental features (`Microphone`, `Headphones`, `ResumeAfterSleep`, `ExtraFormats`) set to 0.
- Master speakers active on `macos` preset, volume 100%, CrashGuard strikes = 0.
- `MicInput` remains disabled pending the Message 18 payload fix.
