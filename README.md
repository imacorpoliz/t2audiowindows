# T2AudioPort — Apple T2 Audio Driver for Windows (WaveRT)

[![Build & Test](https://github.com/imacorpoliz/t2audiowindows/actions/workflows/build.yml/badge.svg?branch=diagnostics)](https://github.com/imacorpoliz/t2audiowindows/actions/workflows/build.yml)
[![Status](https://img.shields.io/badge/Hardware%20Status-Sound%20%2B%20DSP%20working%20(MacBookPro16%2C1)-brightgreen)](#hardware-verification-status)
[![Driver](https://img.shields.io/badge/Architecture-WaveRT%20%2F%20PortCls-blue)](#audio-pipeline-architecture)

> **Current Status (October 2026): sound through the internal speakers works on physical hardware, with the in-driver speaker DSP.**  
> * **Proven on hardware:** BSOD 0x7E fixed (kernel-mode interception of `IOCTL 0x222020`); `START_IO`/`STOP_IO` and `UPDATE_TIMESTAMP` clock lock; sound from all 6 speakers (Stage E, 1.0.8.0); KAIT2EN DSP chain with virtual bass and multiband compressor (`SpeakerDsp=3`) approved by the owner on ~15 minutes of music (1.0.11.0); clean copy timing (max gap ~3 ms, no underruns).
> * **Endpoint:** Windows sees a standard **Stereo 48 kHz / 32-bit** endpoint. The driver spreads stereo over the 6 physical speakers through the KAIT2EN 16_1 crossover/EQ.
> * **Implemented, not yet hardware-tested:** persistent mode with crash guard (starts on every boot), installer with sound presets, lookahead limiter, optional autogain, headphone jack read.
> * **Not implemented yet:** headphone output, microphones, hardware volume, models other than MacBookPro16,1. The full plan: [`docs/ROADMAP.md`](docs/ROADMAP.md).

🛠️ **Install for daily use: [`docs/INSTALL.md`](docs/INSTALL.md)** — `tools/T2Audio-Setup.ps1`: menu installer (Russian UI) with sound presets, persistent mode with crash guard, one-click return to AppleAudio. Experimental, not yet hardware-tested.  
📘 **Detailed Driver Guide: [`docs/DRIVER_GUIDE.md`](docs/DRIVER_GUIDE.md)** — hardware register maps, BCE protocol, DSP, parameters and debugging.  
🗺️ **Roadmap to full functionality: [`docs/ROADMAP.md`](docs/ROADMAP.md)** — everything that is left, in order.  
🤖 **Builder prompt (Gemini): [`GEMINI_PROMPT.md`](GEMINI_PROMPT.md)** — paste-ready task for the next hardware session.  
📋 **Authoritative Status & Progress Log: [`docs/CURRENT_STATE.md`](docs/CURRENT_STATE.md)** — chronological hardware test logs and verified findings.

---

## What Remains for "macOS-Quality" Sound

The 16-inch MacBook Pro is renowned for class-leading laptop acoustics, achieved through an intricate combination of custom multi-speaker hardware and a sophisticated digital signal processing (DSP) pipeline in macOS. Bringing that exact acoustic quality to Windows involves clearly defined stages:

```
  [Stage D: PROVEN] ──► [Stage E: PROVEN] ──► [Stage F: PROVEN (level 3)] ──► [Stage G: IN PROGRESS]
   START_IO & Clock       First Sound           Acoustic EQ, virtual bass,      Auto-start on boot (done, untested),
   (Verified on HW)       (1.0.8.0)             compressor, limiter             headphones, mic, hardware volume
```

### 1. First Audible Playback (Stage E — first sound achieved 2026-10-28)
* **Result:** the owner heard sound from the internal speakers through this driver (build 1.0.8.0, `docs/logs/EXCERPT_20261028_143928.md`): flat, sometimes crackling, not loud.
* **Since then (source only):** start-of-stream gap fixed (wrapped `hostTime` after > 25.6 h uptime), per-run copy-timing counters, `-Listen` mode in `tools/T2Audio-TestAgent.ps1`, default attenuation −6 dB. Analysis: first entry of `docs/CURRENT_STATE.md`.
* **Then:** the in-driver speaker DSP (Stage F) was approved on hardware: `SpeakerDsp=2` brought back the bass (1.0.10.0), `SpeakerDsp=3` was approved as the default (1.0.11.0, owner: "все хорошо" — "all good").

### 2. Acoustic EQ & Deep Bass Tuning (macOS DSP Parity)
* **The Reality of Laptop Acoustics:** The MacBook Pro speaker enclosures are physically compact. macOS achieves its signature deep, punchy low-end via heavy DSP equalization: boosting frequencies below 100 Hz while notching out chassis resonances of the unibody aluminum casing. Without EQ, raw speakers sound thin and flat.
* **The Solution:** Apply the measured chassis frequency response curves (derived from the open-source [kaiT2en](https://github.com/kaiT2en/KaiT2en-Fedora) `dsp/profiles/16_1` Linux profile). On Windows, this can be loaded seamlessly via **Equalizer APO** directly on the stereo endpoint or integrated into a bundled Audio Processing Object (APO).
* **Status (2026-10-28):** done inside the driver instead (`src/T2AudioDsp.h`, integer DSP in the copy DPC, generated from the KAIT2EN graph by `tools/dsp/gen_kait2en_dsp.py`): Tone1/Tone2 EQ, per-driver crossover and EQ, virtual bass (bankstown). Level 3 approved on hardware.

### 3. Speaker Protection & Dynamic Range Control (DRC / Peak Limiter)
* **Why It Is Essential:** At volumes above ~30%, bass frequencies would push miniature woofer cones beyond their maximum physical excursion ($X_{\max}$), and high-frequency peaks could thermally overload delicate tweeters. macOS uses a lookahead peak limiter to prevent mechanical and thermal damage.
* **The Solution:** A lightweight lookahead limiter / compressor before writing into BAR0, preventing digital clipping and hardware distortion when Windows master volume is set to 100%.
* **Status (2026-10-28):** in the driver (`src/T2AudioDsp.h`): 0.5 ms lookahead limiter on every DSP level, multiband compressor (`SpeakerDsp=3`, owner-approved), woofer buzz compressor (`SpeakerDsp=4`), optional autogain (`DspAutoGain=1`). The lookahead limiter and autogain are not yet hardware-tested.

### 4. Spatial Soundstage (Crosstalk Cancellation / XTC)
* **macOS Signature Staging:** Apple's wide spatial soundstage (where audio appears to emanate from beyond the physical laptop display) is created using inter-aural crosstalk cancellation (XTC).
* **The Solution:** Implement a crossfeed/XTC matrix in the driver's stereo-to-speaker distribution pass.
* **Status:** done (KAIT2EN XTC: delayed mid + 96-tap FIR on the side signal) in every DSP level.

### 5. Daily Driver Production Integration
* **Persistent Auto-start:** done as `Parameters\EnableProduction` (set by the installer) with a crash guard; awaiting the hardware test.
* **Hardware Volume Control:** Route volume slider adjustments to the hardware amplifier via BCE command 28 (`controls`), preserving full 24-bit resolution at lower listening levels.
* **Headphone Jack Auto-detection:** Monitor `'jack'` events to automatically switch between internal speakers and the 3.5 mm output. Step 1 (jack state read at probe) is done; the output itself is planned in `docs/HEADPHONES.md`.
* **Studio Microphone Array:** Add support for the high-SNR 3-mic beamforming array (`0x21 Digital Mic` / `0x44 Codec Input`).

---

## Audio Pipeline Architecture

```
 Windows Audio Engine (WASAPI)
        │  32-bit PCM, stereo, 48 kHz (WAVEFORMATEXTENSIBLE)
        ▼
 WaveRT Cyclic Buffer (System RAM)
        │  Copy DPC: tracks consumed play cursor
        │  Speaker DSP (SpeakerDsp=1..4, default 3): [autogain] → virtual bass → Tone1 → XTC
        │    → Tone2 → multiband compressor → per-driver crossover + EQ → lookahead limiter
        │  (SpeakerDsp=0: plain path, woofers full range, tweeters 4th-order HPF @ 3 kHz)
        │    Left  ──► Woofers [0, 1] + Tweeter [2]
        │    Right ──► Woofers [3, 4] + Tweeter [5]
        │  Format Conversion: ──► S24_LE (arithmetic shift >> 8)
        │  Clock Synchronization: maintains target 1000-frame lead via T2 UPDATE_TIMESTAMP
        ▼
 BAR0 Speaker Ring (399,360 bytes = 16,640 frames @ 48 kHz, Write-Combining + sfence)
        │  bridgeOS continuously streams ring data via DMA
        ▼
 Apple T2 Audio Subsystem ──► 6 Physical MacBook Pro Speakers
```

### Key Subsystems:
1. **Memory & Buffer Discovery (`src/Device.c`):** Locates configuration memory via GPR signature `0x19870423`, parses `BufferStruct`, and binds to the `"Speaker"` DMA buffer at offset `0x12c000` (size `0x61800` = 399,360 bytes). Maps BAR0 as write-combining for high-throughput cache-line streaming.
2. **BCE Transport & Safe Interception (`src/BceTransport.c`):** Issues commands to `\Device\AppleUSBVHCI` via `IOCTL 0x222018`. Intercepts incoming bridgeOS messages (`IOCTL 0x222020`) in kernel mode, shielding `ks.sys` from NULL `FileObject` dereferences.
3. **Hardware Clock & Drift Control (`src/WaveRTStream.c`):** The T2 audio subsystem emits an `UPDATE_TIMESTAMP` notification precisely once per ring revolution (every 16,640 frames = 346.66 ms). The driver locks its write cursor 1,000 frames ahead of the read cursor, dynamically compensating for startup latencies.

---

## Hardware Verification Status

All tests are verified on physical hardware (**MacBookPro16,1**, `PCI\VEN_106B&DEV_1803`):

| Stage | Execution | Purpose | Result | Key Indicators |
|---|---|---|:---:|---|
| **Host Unit Tests** | Offline | Control protocol, buffer wrapping, crossover math, copy pipeline, DSP vs double reference, crash guard | **PASS** | 0 failures across 6 test suites (MSVC, GCC, Clang) |
| **Stage 1** | Hardware | System RAM mapping & PortCls registration | **PASS** | Stereo 48 kHz / 32-bit endpoint active, WASAPI probe OK, mute verified |
| **Stage 2** | Hardware | BAR0 memory mapping & GPR probing | **PASS** | BAR0 mapped WC, GPR `0x19870423` verified, buffer located at `0x12c000` (`0x61800` bytes) |
| **Stage 3** | Hardware | BCE device discovery & format parsing | **PASS** | 5 BCE devices enumerated; Stream `0x3A` S24_LE 6ch; Latency 0+11 frames, safety 71 |
| **Stage 4** | Hardware | Transport hold during active stream | **PASS** | Audio path enabled for speaker `0x39`, transport kept open throughout stream |
| **Stage 5** | Hardware | MMIO DMA ring copy & DPC timer | **PASS** | Ring zeroed, copy DPC active, `ticks=1006` recorded across position calls |
| **Stage D** | Hardware | **Hardware START_IO & Clock Locking** | **PASS** | **BSOD 0x7E fixed!** Clocks locked at 16,640 frames / 346.66 ms; clean `STOP_IO` |
| **Stage E** | Hardware | First sound | **PASS** | Sound from the internal speakers (1.0.8.0) |
| **Stage F** | Hardware | Speaker DSP | **PASS** | `SpeakerDsp=2` (1.0.10.0) and `=3` (1.0.11.0) approved by the owner; limiters idle; max copy gap ~3 ms |
| **Recovery** | Hardware | Safe driver rollback to official package | **PASS** | Device cleanly restored to `AppleAudio.sys` (`oem16.inf`, Started, OK) |

Detailed hardware capture logs: [`docs/logs/`](docs/logs/) (Stage D: `EXCERPT_20261028_043442.md`, first sound: `EXCERPT_20261028_143928.md`, DSP level 3: `EXCERPT_20261028_192200.md`).

---

## Building & Testing

### 1. Build the Driver
Requirements: Visual Studio 2022+ and Windows Driver Kit (WDK) 10:
```powershell
# Build driver binary (Release x64)
msbuild src\T2AudioMiniport.vcxproj /p:Configuration=Release /p:Platform=x64 /t:Rebuild

# Run host unit tests
cl /nologo /W4 /Fe:tests\BceProtocolLogicTest.exe tests\BceProtocolLogicTest.c && tests\BceProtocolLogicTest.exe
cl /nologo /W4 /Fe:tests\T2AudioBufferLogicTest.exe tests\T2AudioBufferLogicTest.c && tests\T2AudioBufferLogicTest.exe
cl /nologo /W4 /Fe:tests\T2AudioSpeakerLogicTest.exe tests\T2AudioSpeakerLogicTest.c && tests\T2AudioSpeakerLogicTest.exe
# ... and T2AudioPipelineTest, T2AudioDspTest, T2AudioCrashGuardTest the same way (gcc/clang: add -lm)
```

### 2. Package & Sign
```powershell
$kit = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0\x64"
cd packaging
& "$kit\signtool.exe" sign /fd SHA256 /sha1 <THUMBPRINT> /t http://timestamp.digicert.com T2AudioMiniport.sys
& "$kit\makecat.exe" t2audio.cdf
& "$kit\signtool.exe" sign /fd SHA256 /sha1 <THUMBPRINT> /t http://timestamp.digicert.com t2audiominiport.cat
cd ..
```

### 3. Run Diagnostic Tests
```powershell
# Safe RAM-only verification
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Stage 1 -ExpectedSysHash <SHA256>

# Hardware I/O test (ring zeroed, silent run)
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Mmio -BceIo -OwnerApprovedBceIo -ExpectedSysHash <SHA256>

# Immediate recovery to official Boot Camp driver
.\tools\T2Audio-TestAgent.ps1 -Mode Recover
```

---

## Attributions & References

* **[kaiT2en](https://github.com/kaiT2en/KaiT2en-Fedora)** (`modules/t2bce_audio`) — Reference Linux implementation, reverse engineering of the BCE transport protocol, BufferStruct format, and MacBookPro16,1 speaker layout.
* **[kekrby](https://github.com/kekrby/linux-t2)** — Pioneering research on Apple T2 audio subsystems in Linux.
* **[osaul4ik](https://github.com/osaul4ik/apple-t2-touchid-windows)** — CI workflow structure for WDK compilation on GitHub Actions.
* **Apple Inc.** — Original Apple T2 hardware architecture and Boot Camp drivers (`AppleAudio.sys`, `AppleUSBVHCI.sys`).
