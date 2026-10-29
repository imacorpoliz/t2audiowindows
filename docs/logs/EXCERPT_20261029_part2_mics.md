# EXCERPT 2026-10-29 11:15:00 — Part 2: Build 1.0.13.0, Host Tests, ExtraFormats & Microphone Buffer Registration

> **Date:** 2026-10-29 11:15:00 +05:00 (06:15:00 UTC)  
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

4. **Step P2: Microphone Groundwork & Registration (`MicInput=1`) (HISTORIC PASS):**
   - Enabled via `.\tools\T2Audio-Setup.ps1 -Action Features -Microphone 1`.
   - Allocated contiguous host physical memory: `pa=0x47EECB000`, size `0x30C00` (16,640 frames).
   - Wrote physical page ranges into `BufferStruct.devices[3].input_streams[0]`.
   - Sent BCE message 18 (`SET_INPUT_STREAM_ADDRESS_RANGES`) to Apple T2 chip (`id=0x21`).
   - **T2 chip / bridgeOS accepted registration:** `status=0x00000000` (reply 256 bytes).
   - Kernel log confirmed: `audio path ENABLED for microphones 0x21 (0x00000000)`.
   - Windows Audio Endpoint created: `Microphone Array (Apple T2 Audio Device (6-channel Native Driver))` status OK.
   - Teardown verified: `T2Audio: mic ring released`.

---

## 2. Kernel Log Verbatim (Microphone Registration & Hardware Acceptance)

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
...
T2Audio: mic ring released
```
