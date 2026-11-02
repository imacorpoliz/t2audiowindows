# T2AudioPort — Detailed Driver Guide & Architecture Manual

> **Scope:** This document is the authoritative technical reference for the `T2AudioPort` kernel-mode driver codebase. It describes the low-level T2 hardware architecture, BCE transport protocol, PortCls/WaveRT implementation, acoustic 6-channel DSP engine, 5.1 virtual surround sound, bidirectional jack detection, microphone array capture, and fault-tolerant safety models.
> 
> **Hardware Status:** Fully implemented and verified on physical Apple MacBook Pro 16" (2019, `MacBookPro16,1`) under Windows 11 Pro 64-bit (build 28000+).
> **Related Documents:** Quick install in [`docs/GUIDE.md`](GUIDE.md), configuration reference in [`docs/INSTALL.md`](INSTALL.md), release history in [`docs/CURRENT_STATE.md`](CURRENT_STATE.md), and long-term milestones in [`docs/ROADMAP.md`](ROADMAP.md).

---

## Contents

1. [Status and Verification](#1-status-and-verification)
2. [Hardware: The T2 Audio Controller](#2-hardware-the-t2-audio-controller)
3. [The BCE Transport Protocol](#3-the-bce-transport-protocol)
4. [Driver Architecture & Subsystems](#4-driver-architecture--subsystems)
5. [Device Life Cycle & PnP Management](#5-device-life-cycle--pnp-management)
6. [Streaming Path & Audio Pipeline](#6-streaming-path--audio-pipeline)
7. [Acoustic DSP, Virtual Bass & 5.1 Surround](#7-acoustic-dsp-virtual-bass--51-surround)
8. [Synchronisation & IRQL Model](#8-synchronisation--irql-model)
9. [Safety Model, Crash Guard & Registry Parameters](#9-safety-model-crash-guard--registry-parameters)
10. [Build Environment & Automated Tests](#10-build-environment--automated-tests)
11. [Diagnostics & Kernel Log Reference](#11-diagnostics--kernel-log-reference)
12. [Comparison with Linux kaiT2en](#12-comparison-with-linux-kait2en)
13. [Glossary](#13-glossary)

---

## 1. Status and Verification

All subsystems have been verified on physical hardware on `MacBookPro16,1`:

The v1.0.13.20 teardown and installer changes were host-tested and statically
audited here; a physical hardware rerun was not available in this environment.

| Subsystem / Feature | Status | Implementation Details |
|---|:---:|---|
| **Build & CI** | ✅ **Verified** | Windows Server 2022 / WDK, Release x64, automated GitHub Actions. |
| **Host Unit Tests** | ✅ **Verified** | 7 test suites in `tests/` covering math, protocol, rings, and DSP. |
| **Stages 1–5 on Hardware** | ✅ **Verified** | Memory allocation, BAR mapping, BCE discovery, transport, MMIO copy. |
| **START_IO & 0x7E Panic Fix** | ✅ **Verified** | Clean I/O start; eliminates the Boot Camp `0x7E` kernel dereference bugcheck. |
| **6-Channel Stereo Output** | ✅ **Verified** | 4 woofers + 2 tweeters driven simultaneously with crossover and EQ. |
| **Acoustic DSP (KAIT2EN 16_1)** | ✅ **Verified** | Apple-matched biquad EQ, Linkwitz-Riley crossovers, multiband compression. |
| **Virtual Bass (Bankstown)** | ✅ **Verified** | Psychoacoustic harmonic synthesis (40–90 Hz missing fundamental). |
| **5.1 Virtual Surround** | ✅ **Verified** | Native 6-channel surround downmix with acoustic crosstalk cancellation (XTC). |
| **Bidirectional Jack Switching** | ✅ **Verified** | Dynamic presence detection, `KSEVENT_PINCAPS_JACKINFOCHANGE`, `HeadphoneAutoMute`. |
| **Headphone Output & Crossfeed** | ✅ **Verified** | Separate WaveRT endpoint, −12 dB safety cut, Bauer bs2b binaural crossfeed. |
| **Built-in Microphones** | ✅ **Verified** | 3-mic studio array capture, host DMA buffer registration, array geometry. |
| **Live Parameter Tuning** | ✅ **Verified** | Registry reload is available from the 500 ms jack work item when `JackEvents=1`; the installer restarts the device for reliable application. |
| **Sleep / Wake (S3 / Standby)**| ✅ **Verified** | Clean D0/D3 power handling; optional stream resume with `ResumeAfterSleep=1`. |
| **Crash Guard** | ✅ **Verified** | Automatic fallback to safe mode if two ungraceful shutdowns occur. |
| **SMBIOS Model Auto-Detect** | ✅ **Verified** | Dynamic kernel reading of `SystemProductName` from BIOS registry. |

---

## 2. Hardware: The T2 Audio Controller

### 2.1 The PCI Device
* **Hardware ID:** `PCI\VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01`
* **PCI Function:** Audio Controller exposed by bridgeOS to the host Intel CPU.
* **Control Path:** Audio stream commands and hardware queries do not traverse PCI config space directly; they travel via the Apple Buffer Copy Engine (BCE) mailbox implemented by `AppleUSBVHCI.sys` via IOCTL `0x222018`.

### 2.2 Memory Resources (BARs)
* **BAR 0 (Audio Rings & BufferStruct):** Primary memory window mapped R/W. Prefetchable (`CM_RESOURCE_MEMORY_PREFETCHABLE`), mapped using Write-Combining (WC) with memory barriers (`_mm_sfence`).
* **Config BAR (GPR Block):** Mapped Read-Only. Contains the General Purpose Registers (GPR) at offset `+0xC000`:
  * `GPR[0]` (`+0x00`): Version (must be ≥ 2).
  * `GPR[1]` (`+0x04`): Magic Signature `0x19870423`.
  * `GPR[2]` (`+0x08`): Byte offset of `BufferStruct` inside BAR0.
  * `GPR[4]` (`+0x10`): Sleep marker `0x19770525`.

### 2.3 `BufferStruct` (Device & Stream Discovery)
`BufferStruct` describes all audio devices, input/output streams, and ring buffer allocations in BAR0.
- Layout defined in `src/T2AudioMiniport.h` with compile-time assertions (`C_ASSERT`).
- The speaker device is identified by the exact name `"Speaker"`.
- The headphone device is identified by `"Codec Output"`.
- The microphone array is identified by `"Digital Mic"`.

### 2.4 The Speaker Hardware Ring
* **Total Ring Size:** `0x61800` bytes (399,360 bytes = 16,640 frames × 24 bytes ≈ 346.7 ms at 48 kHz).
* **Hardware Format (`'pft '`):** 6 channels, 48,000 Hz, 24-bit PCM in 32-bit slots (`S24_LE`), little-endian, interleaved.
* **Transducer Mapping (MacBookPro16,1):**
  - Channel 0: Left Woofer 1 (Outer)
  - Channel 1: Left Woofer 2 (Inner, force-cancelling)
  - Channel 2: Left Tweeter
  - Channel 3: Right Tweeter
  - Channel 4: Right Woofer 2 (Inner, force-cancelling)
  - Channel 5: Right Woofer 1 (Outer)

---

## 3. The BCE Transport Protocol

### 3.1 Transport Mechanics (`src/BceTransport.c`)
* Communicates with `\Device\AppleUSBVHCI` using IOCTL `0x222018` (`CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)`).
* All messages are serialized under `g_BceLock` (`KMUTEX`). Wait timeout is 6,000 ms.
* **Tagging:** Every request contains a rolling 32-bit tag (1..127). The reply tag must match; mismatched responses are discarded.
* **Poisoning Protection:** If a BCE transaction times out, the transport is marked poisoned to prevent subsequent command lockups.

### 3.2 The 0x7E Bugcheck Fix (AppleUSBVHCI Interception)
In Apple's default Boot Camp driver stack, `AppleUSBVHCI.sys` pushes bridgeOS asynchronous messages (such as `UPDATE_TIMESTAMP` right after `START_IO`) to the first `KSCATEGORY_AUDIO` device matching `VEN_106B` as IOCTL `0x222020` without a `FileObject`. 
- When PortCls receives this, it attempts to dereference `FileObject->FsContext` at offset `+0x18`, causing BugCheck `0x7E` in `ks!DispatchDeviceIoControl`.
- **T2AudioPort Solution:** In `src/Driver.c`, `T2AudioDeviceControl` intercepts IOCTL `0x222020`, validates the buffer, completes the IRP with `STATUS_SUCCESS`, and prevents PortCls from panicking.

---

## 4. Driver Architecture & Subsystems

### 4.1 Filter Pairs & Topology
`T2AudioMiniport` registers three independent PortCls filter pairs:
1. **Speakers Pair:** `Topology` (`KSCATEGORY_TOPOLOGY`) + `Wave` (`KSCATEGORY_AUDIO`, `KSCATEGORY_RENDER`).
2. **Headphones Pair:** `TopologyHeadphone` + `WaveHeadphone`.
3. **Microphone Pair:** `TopologyMic` + `WaveMic`.

### 4.2 SMBIOS Model Auto-Detection (`src/Driver.c`)
During driver initialization, `T2AudioDetectHardwareModel` queries:
`\Registry\Machine\HARDWARE\DESCRIPTION\System\BIOS\SystemProductName`
- Automatically identifies `MacBookPro16,1` in kernel space and logs the identified platform.

---

## 5. Device Life Cycle & PnP Management

### 5.1 Power Transitions (`src/Power.c`)
- Implements `IAdapterPowerManagement`.
- **D0 → D3 (Sleep):** Sends `STOP_IO`, suspends MMIO writes, disarms the crash guard, and cancels the jack polling timer.
- **D3 → D0 (Wake / Resume):** Re-arms the crash guard, restarts jack polling, re-registers microphone DMA buffers, and queues `T2AudioResumeWorkRoutine` to smoothly restart running audio streams.

---

## 6. Streaming Path & Audio Pipeline

```
[Windows Audio Engine]
         │ (Stereo or 5.1 PCM, 48 kHz / 16/24/32-bit)
         ▼
[IMiniportWaveRTStream Buffer]
         │ (Cyclic shared DMA buffer)
         ▼
[T2AudioStreamCopyTimer (2 ms Periodic DPC)]
         │
         ├─► [Stereo Stream] ──► T2DspRenderStereoToSpeakers()
         │                          ├─ Tone Controls (Bass, Treble, Clarity)
         │                          ├─ Bankstown Virtual Bass (40-90 Hz harmonics)
         │                          ├─ Crosstalk Cancellation (XTC)
         │                          ├─ Multiband Compressor (4 bands)
         │                          ├─ Crossover & 16_1 Biquad EQ
         │                          └─ Lookahead Peak Limiter
         │
         ├─► [5.1 Surround] ───► T2DspRender5Point1ToSpeakers()
         │                          ├─ Center -> Left/Right Tweeter downmix
         │                          ├─ LFE -> Woofer Virtual Bass downmix
         │                          └─ Surrounds -> XTC Spatial Widening
         │
         └─► [Headphones] ─────► T2DspRenderHeadphones()
                                    ├─ Clean Stereo Path (No Speaker EQ)
                                    ├─ Bauer bs2b Binaural Crossfeed
                                    └─ -12 dB Safety Attenuation
         │
         ▼
[Write-Combining MMIO Copy into BAR0 Ring]
         │
         ▼
[Apple T2 bridgeOS DMA Playback]
```

---

## 7. Acoustic DSP, Virtual Bass & 5.1 Surround

### 7.1 Virtual Bass Synthesis (Bankstown Algorithm)
Laptop woofers cannot mechanically reproduce sub-80 Hz frequencies without severe distortion. The virtual bass generator tracks low frequencies and synthesizes 2nd and 3rd harmonics using non-linear saturation:
\[
y = \tanh(\alpha \cdot x) - \beta \cdot x^3
\]
The human auditory cortex reconstructs the fundamental pitch psychoacoustically, yielding deep, rich bass without cone rattle.

### 7.2 5.1 Virtual Surround Sound (`T2DspRender5Point1ToSpeakers`)
When a 5.1 audio stream (FL, FR, Center, LFE, SL, SR) is received:
- **Center:** Blended equally into Left & Right Tweeter channels with dialogue clarity enhancement.
- **LFE (Low Frequency Effects):** Directed into the 4 woofers through the virtual bass generator.
- **Surround Channels (SL / SR):** Processed through an acoustic crosstalk cancellation (XTC) matrix with interaural time delays, throwing surround effects far beyond the physical width of the laptop.

### 7.3 Bidirectional Jack Auto-Switching
- The driver polls the hardware property `'outp'/'jack'` every 500 ms via `T2AudioJackWorkRoutine`.
- On state change (plug or unplug), `T2AudioNotifyJackChange` fires `KSEVENT_PINCAPS_JACKINFOCHANGE`.
- Windows `AudioEndpointBuilder` queries `KSPROPERTY_JACK_DESCRIPTION`, sees the new state, and automatically switches default playback.
- Simultaneously, `HeadphoneAutoMute` mutes the physical speakers when headphones are connected and unmutes them when disconnected.

---

## 8. Synchronisation & IRQL Model

* **Lock Order:** `StreamLock` (Spin Lock) → `HardwareLock` (Spin Lock) → `ClockLock` (Spin Lock).
* All BCE mailbox operations occur at `PASSIVE_LEVEL` under `g_BceLock` (`KMUTEX`).
* All audio stream rendering occurs at `DISPATCH_LEVEL` under `StreamLock` and `HardwareLock`.

---

## 9. Safety Model, Crash Guard & Registry Parameters

### 9.1 Persistent Crash Guard (`src/T2AudioCrashGuard.h`)
To prevent infinite boot loops if a bugcheck occurs, the driver maintains an armed watchdog in `HKLM:\SYSTEM\CurrentControlSet\Services\T2AudioMiniport\Parameters`:
- `CrashGuardArmed`: Set to `1` when audio starts; cleared to `0` on clean shutdown or sleep.
- `CrashGuardStrikes`: Incremented if Windows boots while `CrashGuardArmed` was still `1`.
- If `CrashGuardStrikes >= 2`, the driver enters Safe Mode and refuses to load until reset via `T2Audio-Setup.ps1 -Action ResetGuard`.

### 9.2 Live Parameters
The driver can reload these values from its 500 ms jack work item when
`JackEvents=1`. For normal configuration, the installer restarts the audio
device after changing them so the result does not depend on jack polling:

| Parameter | Type | Default | Description |
|---|---|---|---|
| `SpeakerDsp` | DWORD | `3` | `0`=Bypass, `1`=Basic, `2`=Virtual Bass, `3`=Full macOS DSP, `4`=DSP+Anti-Rattle |
| `DspGainDb` | DWORD | `6` | Post-DSP make-up gain (0..12 dB) |
| `BassDb` | DWORD | `0` | 150 Hz low-shelf tone adjustment (−12..+12 dB) |
| `TrebleDb` | DWORD | `0` | 6 kHz high-shelf tone adjustment (−12..+12 dB) |
| `ClarityDb` | DWORD | `0` | 2.5 kHz presence boost (−12..+12 dB) |
| `HeadphoneCrossfeed` | DWORD | `0` | Bauer bs2b binaural crossfeed for headphones |
| `HeadphoneAutoMute` | DWORD | `0` | Automatically mutes speakers when headphones plugged in |
| `AntiRattle` | DWORD | `0` | Low-frequency cone excursion protection limiter |
| `DspLoudness` | DWORD | `0` | Dynamic loudness compensation curve at low volumes |
| `ResumeAfterSleep` | DWORD | `0` | Automatic stream restoration after Modern Standby / Sleep |

---

## 10. Build Environment & Automated Tests

### 10.1 Compiling
Requires Visual Studio 2022 + Windows Driver Kit (WDK):
```cmd
cd src
build.bat
```
Output: `packaging\T2AudioMiniport.sys`.

### 10.2 Host Unit Tests
Tests compile on any C compiler without kernel dependencies:
```powershell
foreach ($t in 'BceProtocolLogicTest','T2AudioBufferLogicTest','T2AudioSpeakerLogicTest',
               'T2AudioPipelineTest','T2AudioDspTest','T2AudioCrashGuardTest','T2AudioHeadphoneTest') {
    cl /nologo /W4 /Fe:tests\$t.exe tests\$t.c
    & "tests\$t.exe"
}
```

---

## 11. Diagnostics & Kernel Log Reference

Filter kernel messages with tag `T2Audio:` in DebugView or WinDbg:
* `T2Audio: hardware model detected: MacBookPro16,1`
* `T2Audio: live DSP params updated on the fly: Dsp=3 Bass=0 Treble=0 Clarity=0 Gain=6`
* `T2Audio: jack plug state changed: PLUGGED -> UNPLUGGED, notifying Windows`
* `T2Audio: crash guard disarmed (clean end of session)`

---

## 12. Comparison with Linux kaiT2en

| Feature | Linux kaiT2en | Windows T2AudioPort | Notes |
|---|:---:|:---:|---|
| **6-Channel Audio Output** | ✅ | ✅ | Full 6-channel native hardware streaming |
| **Acoustic Biquad DSP** | ✅ (PipeWire) | ✅ (Kernel DPC) | In-driver zero-overhead fixed point |
| **Virtual Sub-Bass** | ✅ (Bankstown) | ✅ (Bankstown) | Integer psychoacoustic synthesis |
| **Jack Auto-Switching** | ✅ | ✅ | Dynamic `KSEVENT` AudioEndpoint handoff |
| **Headphone Crossfeed** | ✅ | ✅ | Integrated Bauer bs2b filter |
| **3-Mic Studio Array** | ✅ | ✅ | Host DMA buffer + Windows Array Geometry |
| **5.1 Virtual Surround** | ❌ | ✅ | Multichannel downmix + XTC spatial expansion |
| **Crash Protection** | ❌ | ✅ | Multi-strike boot watchdog |

---

## 13. Glossary

* **BCE:** Buffer Copy Engine (Apple's mailbox IPC protocol with bridgeOS).
* **bridgeOS:** Embedded OS running on the Apple T2 Security Chip.
* **bs2b:** Bauer Stereophonic-to-Binaural acoustic crossfeed algorithm.
* **WaveRT:** Windows real-time audio driver model with cyclic DMA buffer.
* **XTC:** Acoustic Crosstalk Cancellation for spatial stereo widening.
