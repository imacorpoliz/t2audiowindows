# Apple T2 Audio Driver for Windows (T2AudioPort)
## Comprehensive Technical Briefing & New Agent Handover Manual

> **Target Platform:** Apple MacBook Pro 16-inch (2019, `MacBookPro16,1`), Apple T2 Security Chip (`PCI\VEN_106B&DEV_1803`), Windows 11 Pro 64-bit (build 28000+).  
> **Driver Type:** Native Kernel-Mode PortCls / WaveRT Audio Miniport Driver (`T2AudioMiniport.sys`).  
> **Repository Root on Drive A:** `A:\opencode\t2audio\`  
> **Original Workspace:** `C:\Users\othysa\Desktop\mbp\T2AudioPort\` (public repo) & `C:\Users\othysa\Desktop\mbp\` (research & archives).

---

## 1. Executive Summary & Project Purpose

Under Windows Boot Camp on Intel Macs equipped with Apple's T2 Security Chip, the default `AppleAudio.sys` driver fails to provide proper audio, lacks the proprietary macOS DSP processing, produces flat/tinny sound, and often crashes during sleep/wake cycles or START_IO transitions.

**T2AudioPort** is a ground-up, clean-room reverse-engineered and re-implemented native Windows kernel driver (PortCls / WaveRT) that unlocks:
1. **True 6-Channel Independent Audio:** Directly addresses all 6 physical speakers on the MacBook Pro 16" (2 Left Woofers, 2 Right Woofers, 1 Left Tweeter, 1 Right Tweeter).
2. **Hardware-Matched DSP Pipeline:** Replicates macOS-grade acoustic reproduction using the KAIT2EN profile 16_1 biquad filters, crossover networks, and the Bankstown virtual sub-bass harmonic generator (James Calligeros).
3. **5.1 Virtual Surround Sound:** Multichannel surround upmix/downmix with acoustic crosstalk cancellation (XTC) across the 6 physical transducers.
4. **Bidirectional 3.5 mm Jack Auto-Switching:** Hardware polling of T2 property registers with instant Windows Audio Policy handoff via `KSEVENT_PINCAPS_JACKINFOCHANGE` and automatic speaker muting (`HeadphoneAutoMute`).
5. **Headphone Hi-Fi & Crossfeed:** Dedicated, separate headphone filter pair with Bauer stereophonic-to-binaural (`bs2b`) crossfeed algorithm.
6. **Built-in Beamforming Microphone Array:** Low-latency 3-mic recording via BCE stream protocol.
7. **Crash Guard & Fault Tolerance:** Automatic watchdog preventing boot loops if a kernel crash occurs.
8. **Real-Time Live Parameter Tuning:** Standard users can adjust Bass, Treble, Clarity, and Gain on the fly without stopping sound or restarting the audio engine.

---

## 2. Low-Level Hardware Architecture

### 2.1 PCI Device & MMIO BARs
- **Device Hardware ID:** `PCI\VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01`
- **PCI Class:** Audio Device (`040100`)
- **BAR Mappings:**
  - **BAR 0:** Bridge MMIO registers (Doorbell registers, ring pointers, interrupt status).
  - **BAR 1:** High-speed mailbox memory area.

### 2.2 Apple BCE (Buffer Copy Engine) Transport Protocol
Communication with bridgeOS (the macOS kernel running on the T2 chip) occurs over the BCE transport layer (`src/BceTransport.c`):
- The driver communicates via IOCTL requests sent to `AppleUSBVHCI.sys` or directly over internal BCE IPC channels.
- **Device IDs on BCE Bus:**
  - `0x40`: Speaker Playback Device (Host-to-T2).
  - `0x44`: Codec Input (Headset jack detection, TRRS mic probe).
  - `0x45`: Codec Output / Headphone Stream.
  - `0x43`: Built-in Microphone Array.

### 2.3 Property Reading & Jack Detection
Properties are queried using `T2AudioGetU32Property`:
- **Property Scope:** `0x6f757470` (`'outp'` - Output Scope)
- **Property Selector:** `0x6a61636b` (`'jack'` - Jack Plugged State)
- When 3.5mm headphones are plugged in, `'jack'` reads `1`. When unplugged, it reads `0`.
- The driver runs a low-overhead DPC timer every 500 ms (`JackPollTimer`), which queues an `IO_WORKITEM` (`T2AudioJackWorkRoutine`) to poll this property.

### 2.4 The Critical 0x7E Bugcheck Fix (AppleUSBVHCI Trap)
In stock Boot Camp, `AppleUSBVHCI.sys` pushes asynchronous bridgeOS messages (e.g., `UPDATE_TIMESTAMP` immediately after `START_IO`) to the first `KSCATEGORY_AUDIO` device matching `VEN_106B` as IOCTL `0x222020` without attaching a `FileObject`. 
- When passed to default PortCls, KS dereferences `FileObject->FsContext` at offset `+0x18`, immediately triggering `SYSTEM_THREAD_EXCEPTION_NOT_HANDLED (0x7E)` in `ks!DispatchDeviceIoControl+0x15`.
- **Fix:** In `src/Driver.c`, `T2AudioPnpHandler` intercepts IOCTL `0x222020` before PortCls, checks for null file contexts, completes the IRP with `STATUS_SUCCESS`, completely eliminating the bugcheck.

---

## 3. Audio Pipeline & DSP Engine

### 3.1 Transducer Layout (MacBook Pro 16,1)
The 2019 16" MacBook Pro features 6 high-fidelity speaker drivers:
- **Index 0:** Left Woofer Primary (Outer)
- **Index 1:** Left Woofer Secondary (Inner, force-cancelling pair)
- **Index 2:** Left Tweeter
- **Index 3:** Right Tweeter
- **Index 4:** Right Woofer Secondary (Inner, force-cancelling pair)
- **Index 5:** Right Woofer Primary (Outer)

### 3.2 Biquad Filter Cascades (`src/T2AudioDsp.h`)
Audio samples are processed using Direct Form II Transposed IIR filters:
\[
y[n] = b_0 x[n] + b_1 x[n-1] + b_2 x[n-2] - a_1 y[n-1] - a_2 y[n-2]
\]
- **Crossover:** Split stereo input at ~1200 Hz into woofer bands and tweeter bands.
- **MacBookPro16,1 Calibration:** 12-stage biquad cascade per channel tuned from acoustic measurements to flatten chassis resonances.

### 3.3 Virtual Bass Harmonic Synthesizer (Bankstown Algorithm)
- Small laptop speakers physically cannot reproduce fundamental frequencies below ~80 Hz without excessive cone excursion and distortion.
- The virtual bass algorithm tracks low-frequency energy in the 40–90 Hz band and generates synthetic 2nd (\(2f\)) and 3rd (\(3f\)) harmonic overtones using a non-linear saturation curve:
  \[
  y = \tanh(\alpha \cdot x) - \beta \cdot x^3
  \]
- Due to the "missing fundamental" psychoacoustic phenomenon, the human brain reconstructs the sub-bass pitch, delivering deep, punchy bass without rattling the woofers.

### 3.4 5.1 Virtual Surround Upmix (`T2DspRender5Point1ToSpeakers`)
When a 5.1 channel stream (FL, FR, Center, LFE, SL, SR) is delivered:
- **Center:** Distributed equally across Left and Right Tweeters with speech intelligibility boost.
- **LFE (Subwoofer):** Directed exclusively to the 4 woofers through the virtual bass generator.
- **Surround Left / Surround Right (SL / SR):** Processed through an acoustic crosstalk cancellation (XTC) filter matrix with an inverted 180° interaural phase delay, projecting surround cues far outside the physical laptop chassis.

### 3.5 Headphone Audio Path & Crossfeed (`bs2b`)
- Headphone playback completely bypasses the speaker DSP (no speaker EQ, no limiter) to maintain pristine audiophile transparency.
- Includes an optional Bauer Stereophonic-to-Binaural (`bs2b`) crossfeed filter (700 Hz cutoff, 4.5 dB cross-feed level) to simulate listening to studio monitor speakers in an acoustic room, eliminating the "inside-the-head" listening fatigue caused by hard-panned stereo recordings.

---

## 4. Endpoints & Jack Detection Flow

### 4.1 Filter Pairs in Kernel Space
`T2AudioMiniport` exposes distinct PortCls filter pairs:
1. **Speakers Pair:** `Topology` (`KSCATEGORY_TOPOLOGY`) + `Wave` (`KSCATEGORY_AUDIO`, `KSCATEGORY_RENDER`).
2. **Headphones Pair:** `TopologyHeadphone` + `WaveHeadphone`.
3. **Microphone Pair:** `TopologyMic` + `WaveMic`.

### 4.2 Auto-Switching Mechanics
```
[Physical 3.5mm Jack Plugged/Unplugged]
                 │
                 ▼
[T2 Hardware Updates Property 'outp'/'jack' (0 or 1)]
                 │
                 ▼
[Background Worker 'T2AudioJackWorkRoutine' Reads State]
                 │
                 ▼
[T2AudioNotifyJackChange -> Fires KSEVENT_PINCAPS_JACKINFOCHANGE]
                 │
                 ▼
[Windows AudioEndpointBuilder queries KSPROPERTY_JACK_DESCRIPTION]
                 │
                 ├─► IsConnected == TRUE  -> Routes Audio to Headphones
                 │                          HeadphoneAutoMute mutes Speakers
                 │
                 └─► IsConnected == FALSE -> Routes Audio to Speakers
                                            HeadphoneAutoMute unmutes Speakers
```

---

## 5. Registry Parameters & Live Tuning

All parameters reside in:
`HKLM:\SYSTEM\CurrentControlSet\Services\T2AudioMiniport\Parameters`

| Registry DWORD Name | Values | Default | Description |
| :--- | :--- | :--- | :--- |
| `SpeakerDsp` | `0`=Bypass, `1`=Basic, `2`=EQ only, `3`=Full macOS DSP, `4`=DSP + Limiter | `3` | Speaker processing mode |
| `DspGainDb` | `0` .. `12` | `6` | Master DSP make-up gain in dB |
| `TweeterTrimDb`| `-12` .. `12` | `0` | High-frequency tweeter trim in dB |
| `WooferLimitDb`| `0` .. `12` | `0` | Woofer peak limiter attenuation in dB |
| `DspAutoGain` | `0`=Off, `1`=On | `0` | Automatic LUFS loudness leveling |
| `BassDb` | `-12` .. `12` | `0` | 150 Hz low-shelf tone adjustment |
| `TrebleDb` | `-12` .. `12` | `0` | 6 kHz high-shelf tone adjustment |
| `ClarityDb` | `-12` .. `12` | `0` | 2.5 kHz presence/vocal boost |
| `CompStyle` | `0`=Fast Peak, `1`=RMS Soft-Knee | `0` | Compressor detection curve |
| `HeadphoneCrossfeed` | `0`=Off, `1`=On | `1` | bs2b crossfeed for 3.5mm headphones |
| `AntiRattle` | `0`=Off, `1`=On | `0` | Woofer rattle prevention on bass peaks |
| `EnableProduction` | `0`=Test Session, `1`=Production Persistent | `1` | Persistent mode flag |
| `CrashGuardStrikes`| `0` .. `3` | `0` | Watchdog ungraceful shutdown counter |

> **Live Reloading:** `T2AudioReloadLiveParameters(Context)` runs inside the 500 ms periodic work item. Updating these registry keys changes the sound **within 500 milliseconds** without restarting the audio device!

---

## 6. Directory Map in `A:\opencode\t2audio\`

```
A:\opencode\t2audio\
├── src/                          # Kernel Driver C Source Code
│   ├── Driver.c                  # DriverEntry, PnP Handler, Registry reader, SMBIOS detect
│   ├── Device.c                  # AddDevice, Resource mapping, BAR0/1 MMIO allocation
│   ├── BceTransport.c            # Apple BCE IPC protocol, Jack polling, Mailbox IOCTLs
│   ├── Topology.c                # IMiniportTopology, KS Pins, Jack description properties
│   ├── WaveRTMiniport.c          # IMiniportWaveRT, Stream creation, Audio format negotiation
│   ├── WaveRTStream.c            # IMiniportWaveRTStream, DMA cyclic buffer, DSP dispatch
│   ├── MicCapture.c              # 3-channel microphone array capture & BCE stream reading
│   ├── Power.c                   # D0/D3 Power transitions, S3/Modern Standby resume
│   ├── T2AudioDsp.h              # 6-channel DSP, biquad filters, Bankstown virtual bass, 5.1
│   ├── T2AudioSpeakerLogic.h     # Speaker channel routing tables & gain coefficients
│   ├── T2AudioCrashGuard.h       # Watchdog & strike counter definitions
│   ├── T2AudioMiniport.h         # Master header, prototypes, structure definitions
│   └── T2AudioMiniport.rc        # Version resource file (1.0.13.14 / 1.0.13.15)
│
├── tools/                        # Automation, Diagnostics & GUI Tools
│   ├── T2Audio-Setup.ps1         # Master CLI installer & sound tuner script
│   ├── Restart-T2AudioService.ps1# Graceful service recycler (Audiosrv -> PnP)
│   ├── Make-ReleasePackage.ps1   # Distribution zip packaging script
│   ├── Record-WasapiMic.ps1      # 3-second microphone test recorder
│   ├── Test-WasapiTone.ps1       # Stereo L/R channel diagnostic tone generator
│   └── dsp/                      # Python filter coefficient generators
│       ├── gen_kait2en_dsp.py    # Generates biquads from raw impulse response
│       └── gen_user_eq.py        # Generates user shelf/peak filter tables
│
├── packaging/                    # Driver Package & INF Files
│   ├── T2AudioMiniport.inf       # Windows Device Setup INF
│   ├── T2AudioMiniport.sys       # Compiled x64 release driver binary
│   └── T2AudioMiniport.cat       # Cryptographic catalog file
│
├── certificate/                  # Signing Certificates
│   └── T2AudioTestCert.cer       # Public root certificate for testsigning
│
├── tests/                        # User-Mode Verification & Logic Tests
│   ├── T2AudioDspTest.c          # Automated C test for DSP biquad math
│   ├── BceProtocolLogicTest.c    # Mock BCE mailbox packet validator
│   └── T2AudioSpeakerLogicTest.c # Channel routing validator
│
├── docs/                         # Documentation
│   ├── DRIVER_GUIDE.md           # Deep architectural guide
│   ├── INSTALL.md                # End-user installation guide
│   ├── ROADMAP.md                # Feature completion roadmap
│   └── CURRENT_STATE.md          # Release verification summary
│
└── research_and_archive/         # Complete Historical Dumps & Reverse Engineering Captures
    ├── AppleUSBVHCI-extracted/   # Extracted Apple Boot Camp driver binaries
    ├── archives/                 # ZIP backups of earlier iterations
    ├── upstream-kait2en/         # Upstream Linux T2 sound driver & reverse-engineered profiles
    ├── docs/                     # AI research logs, Gemini prompts, Claude Stage E reports
    └── runtime_logs/             # Hardware register captures and BusHound/Wireshark logs
```

---

## 7. How to Build, Test, and Deploy

### 7.1 Compiling the Kernel Driver
The driver is compiled with MSVC x64 + Windows Driver Kit (WDK):
```cmd
cd A:\opencode\t2audio\src
build.bat
```
Output binary lands in `A:\opencode\t2audio\packaging\T2AudioMiniport.sys`.

### 8.2 Architecture & Project Directives
1. **Strictly Minimal Release Footprint:**
   - The user strictly requested: **NO GUI / Tray App**.
   - The release package must contain strictly the signed driver (`packaging/`), setup script (`tools/T2Audio-Setup.ps1`), root cert (`certificate/`), and `SHA256SUMS.txt`. No third-party bloat, no GUI binaries.
2. **Simplified `README.md`:**
   - Make the public GitHub `README.md` user-friendly: a 3-step quickstart, clear installation instructions, and clean formatting.
To install and test on the current MacBook Pro:
```powershell
# 1. Enable testsigning (requires one reboot if not already enabled)
powershell -NoProfile -ExecutionPolicy Bypass -File A:\opencode\t2audio\tools\T2Audio-Setup.ps1 -Action Prepare

# 2. Install driver and set macOS sound profile
powershell -NoProfile -ExecutionPolicy Bypass -File A:\opencode\t2audio\tools\T2Audio-Setup.ps1 -Action Install -Preset macos

# 3. Check status & diagnostics
powershell -NoProfile -ExecutionPolicy Bypass -File A:\opencode\t2audio\tools\T2Audio-Setup.ps1 -Action Status

# 4. Run sound test (Left/Right stereo check)
powershell -NoProfile -ExecutionPolicy Bypass -File A:\opencode\t2audio\tools\T2Audio-Setup.ps1 -Action SoundTest
```

---

## 8. Current System State & Pending Tasks

### 8.1 Active System Status (Verified Live)
- **Active Driver:** `1.0.13.19` (`T2AudioMiniport.sys` - tested live on physical hardware).
- **PnP Status:** `OK`, 0 crashes (`CrashGuardStrikes: 0`).
- **Unified Single Audio Device:** Windows exposes strictly one audio playback device (`Speakers (Apple T2 Audio Device)`).
  - Unplugged: client audio renders to the 6 physical transducers via KAIT2EN DSP (stereo / 5.1 surround downmix); headphone ring is zeroed.
  - Plugged in: client audio renders to the 2-channel headphone ring with Bauer bs2b crossfeed DSP and -12 dB safety attenuation; speaker ring is zeroed.
  - Seamless glitch-free switching in 2 ms DPC tick without stream restart.
- **Microphone:** 3-mic array active on BCE stream.
- **Kernel Safety Hardening & Bug Fixes (v1.0.13.15 - v1.0.13.19):**
  - **Separate Native Endpoints with Independent Volume (v1.0.13.19):** Enabled `SeparateHeadphoneEndpoint = 1` by default. Windows now exposes separate `Speakers` and `Headphones` endpoints with individual hardware volume controls. Hardware jack detection automatically switches the default output on plug/unplug while restoring the specific volume level of each device.
  - **Headphone bs2b Crossfeed 64-bit Overflow Fix (v1.0.13.18):** Upgraded `LeftIirState` and `RightIirState` in `T2AudioHeadphoneDsp.h` from 32-bit `int` to 64-bit `long long` (`LONGLONG`). The 32-bit integer was overflowing on 24-bit PCM samples ($> -48$ dBFS), creating severe digital clipping and noise. `HeadphoneCrossfeed` is defaulted to `0` for pure direct passthrough, but functions cleanly in 64-bit when enabled.
  - **Headphone Audio Path in Unified Mode (v1.0.13.17):** Resolved root causes preventing 3.5mm headphone switching:
    - `BceTransport.c`: `T2AudioQueryHeadphoneStream` was gated on `HeadphoneOutput == 1`, leaving `HeadphoneFormatValid = FALSE`. Changed to query unconditionally whenever `BceHeadphoneDeviceId != 0`.
    - `Driver.c`: `context->HeadphoneDeviceId` initialization was gated on `HeadphoneFiltersRegistered`, which is `FALSE` in unified single-endpoint mode. Updated condition to `(context->HeadphoneFormatValid && context->BceHeadphoneDeviceId != 0)`.
    - `BceTransport.c`: `JackSeenPlugged` was only set on `plugged != 0`, causing initial state discrepancy. Fixed to set on `NTSUCCESS(status)`.
  - Resolved TOCTOU race in jack poll routine and teardown synchronization (prevents kernel UAF on device unload/stop).
  - Added cancellation and wait loop for `T2AudioResumeWorkRoutine` during PnP Stop/Remove (`T2AudioStopResumeWork`).
  - Synchronized audio format and buffer size reads under `StreamLock` in `T2AudioStreamGetPosition` and `T2AudioStreamPlayPositionBytes`.
  - Added `volatile` qualification to power, jack, and mic state flags.
  - Fixed `ExtraFormats` fallback restoring 32-bit minimum sample size.
  - Fixed installer scripts audit: dynamic PnP ID in `Restart-T2AudioService.ps1`, variable scope in `Install-T2AudioDriver.ps1`, certificate trust error return in `T2Audio-Setup.ps1`, and inclusion of `LICENSE` & `THIRD_PARTY_LICENSES.md` in release zip (`Make-ReleasePackage.ps1`).
  - Fixed tools & tests audit: dynamic PnP discovery in `Capture-T2AudioLog.ps1` & `Test-MdlVariants.ps1`, portable recording path in `Record-WasapiMic.ps1`, exact parameter matching and live auto-restart in `T2-Manager.ps1`, full 8/8 pass in `PostInstall-Validate.ps1`, and corrected test assertions in `T2AudioDspTest.c` & `T2AudioPipelineTest.c`.

### 8.2 Architectural Constraints & Philosophy
1. **Strictly Minimal Footprint (No GUI):**
   - The user explicitly decided against any GUI or tray application ("не делай гуи").
   - Everything is configured cleanly via `T2Audio-Setup.ps1` or standard Windows Sound Settings.
   - The release package must strictly contain: driver (`packaging/`), setup script (`tools/`), certificate (`certificate/`), licenses (`LICENSE`, `THIRD_PARTY_LICENSES.md`, `README.md`), and `SHA256SUMS.txt`.
2. **Simplified `README.md`:**
   - The public `README.md` must be easy to read and simple for anyone to install.
3. **Packaging & Release:**
   - Published GitHub Release: `v1.0.13.17` (`dist/T2AudioPort-1.0.13.17.zip`).
