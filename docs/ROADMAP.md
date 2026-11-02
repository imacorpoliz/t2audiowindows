# T2AudioPort — Project Roadmap & Milestone Verification

**Current Release:** v1.0.13.17  
**Target Hardware:** Apple MacBook Pro 16" (2019, `MacBookPro16,1`), Apple T2 Security Chip (`PCI\VEN_106B&DEV_1803`), Windows 11 Pro 64-bit.

---

## 0. Current Hardware Verification Summary

| Subsystem / Feature | Implemented | Hardware Status | Release | Notes |
|---|:---:|:---:|:---:|---|
| **Driver Initialization & BAR Mapping** | ✅ | ✅ Verified | 1.0.7 | BAR0 rings, config GPR (`+0xC000`), `BufferStruct` parsing |
| **BCE Transport & IPC Mailbox** | ✅ | ✅ Verified | 1.0.7 | AppleUSBVHCI communication, message tags, clock anchor |
| **START_IO & 0x7E Panic Fix** | ✅ | ✅ Verified | 1.0.7 | IOCTL `0x222020` interception, clean START/STOP transitions |
| **6-Channel Stereo Speaker Output** | ✅ | ✅ Verified | 1.0.8 | Simultaneous playback across 4 woofers and 2 tweeters |
| **KAIT2EN Profile 16_1 DSP** | ✅ | ✅ Verified | 1.0.11 | Apple EQ biquads, crossover, lookahead peak limiter |
| **Virtual Bass Synthesis (Bankstown)** | ✅ | ✅ Verified | 1.0.11 | Missing-fundamental harmonic generation (40–90 Hz) |
| **Dynamic Loudness & Anti-Rattle** | ✅ | ✅ Verified | 1.0.13.12 | Fletcher-Munson low-volume boost + cone rattle limiter |
| **Live DSP Parameter Tuning** | ✅ | ✅ Verified | 1.0.13.12 | 500 ms live reload of tone/presets without stream restart |
| **Persistent Boot Mode & Crash Guard** | ✅ | ✅ Verified | 1.0.12 | Automatic Safe Mode after 2 unclean shutdowns |
| **Sleep / Wake State Transitions** | ✅ | ✅ Verified | 1.0.13 | Clean power D0/D3 handling, audio resumes on lid open |
| **Unified Headphone / Speaker Routing** | ✅ | ✅ Verified | 1.0.13.17 | Dynamic seamless switching between 6 speakers and 3.5 mm headphones with bs2b crossfeed and −12 dB attenuation |
| **Bauer bs2b Binaural Crossfeed** | ✅ | ✅ Verified | 1.0.13.11 | Headphone crossfeed in 64-bit Q16 fixed point |
| **Bidirectional Jack Auto-Switching** | ✅ | ✅ Verified | 1.0.13.13 | Hardware register poll + `KSEVENT_PINCAPS_JACKINFOCHANGE` |
| **Automatic Speaker Muting** | ✅ | ✅ Verified | 1.0.13.13 | `HeadphoneAutoMute` mutes speakers while plugged in |
| **Built-in 3-Microphone Array** | ✅ | ✅ Verified | 1.0.13.4 | Digital Mic capture stream, host DMA buffer registration |
| **Windows Mic Array Geometry** | ✅ | ✅ Verified | 1.0.13.11 | `KSPROPERTY_AUDIO_MIC_ARRAY_GEOMETRY` reported to Windows |
| **5.1 Virtual Surround Sound** | ✅ | ✅ Verified | 1.0.13.14 | Multichannel downmix + XTC spatial acoustic expansion |
| **SMBIOS Hardware Model Detection** | ✅ | ✅ Verified | 1.0.13.14 | Automatic kernel detection of `MacBookPro16,1` |
| **Headset TRRS Mic Stream Probe** | ✅ | ✅ Verified | 1.0.13.14 | Codec Input (0x44) stream 0x45 probed on hardware |

---

## 1. Core Milestones (MacBookPro16,1) — 100% COMPLETE

All primary objectives required to completely replace `AppleAudio.sys` on the 16-inch MacBook Pro have been implemented, verified on physical hardware, and delivered in release v1.0.13.17:

1. ✅ **Audio Quality:** Full 6-speaker macOS soundstage with deep bass and clean highs.
2. ✅ **Reliability:** Zero crashes on boot, sleep, wake, or format switching.
3. ✅ **Seamless I/O:** Automatic plug/unplug switching between 3.5 mm headphones and internal speakers.
4. ✅ **Input:** Studio-quality beamforming microphone recording.
5. ✅ **Live Control:** Instant sound preset and tone switching via `T2Audio-Setup.ps1`.

---

## 2. Future / Optional Milestones

These items represent long-term enhancements and community-driven goals:

### 2.1 Extended Model Support
- **Current State:** KAIT2EN acoustic DSP profile for MacBookPro16,1 is fully verified in `src/T2AudioDspProfile16_1.h`. Future targets include additional models from the Linux kaiT2en research:
  - `MacBookPro15,1`, `15,2`, `15,3`, `15,4`
  - `MacBookPro16,1`, `16,2`, `16,3`, `16,4`
  - `MacBookAir8,1`, `8,2`, `9,1`
  - `iMac20,1`, `20,2`, `iMacPro1,1`, `MacPro7,1`
- **Milestone:** Enable community testing on physical hardware for non-16,1 models. The driver already queries SMBIOS to identify the machine.

### 2.2 Low-Latency Event-Driven Mode
- **Current State:** WaveRT polling mode with ~21 ms lead time.
- **Milestone:** Support event-driven WaveRT notifications for ultra-low latency pro-audio DAWs (ASIO/WASAPI exclusive).

### 2.3 WHQL / Attestation Signing
- **Current State:** Self-signed package using test certificate (`certificate/T2AudioCertNew.cer`). Requires Windows Test Mode (`bcdedit /set testsigning on`) and Secure Boot disabled in macOS Startup Security Utility.
- **Milestone:** If an organization funds a yearly Microsoft Hardware Dev Center EV code-signing certificate, the driver can be submitted for attestation signing, allowing installation with Secure Boot enabled.

---

## 3. Maintenance & Release Standards

- **Minimal Release Footprint:** Distribution packages (`dist/T2AudioPort-*.zip`) must strictly contain the signed driver (`packaging/`), setup scripts (`tools/`), certificate (`certificate/`), and SHA256 verification sums.
- **Zero Hallucination / Regression Policy:** All kernel changes must pass host unit tests (`tests/`) and be verified against physical hardware before being tagged in releases.
