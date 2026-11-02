# T2AudioPort — Current State and Hardware Verification

## Overview
This document tracks the verified status, supported features, and release history of the native Windows kernel WaveRT audio driver for the Apple T2 Security Chip on Apple MacBook Pro (16-inch, 2019, `MacBookPro16,1`).

## Supported Hardware & Features (✅ Hardware Verified)

| Component | Hardware Endpoint | Verified Features |
|---|---|---|
| **Speakers** | Speakers (Apple T2 Audio Device) | 6 physical transducers (4 force-cancelling woofers + 2 tweeters) driven via KAIT2EN DSP: crossover, EQ, XTC (crosstalk cancellation), virtual bass, multiband compressor, lookahead limiter. |
| **Multichannel** | Speakers (5.1 Surround) | Dynamic 5.1 surround downmixing (`KSAUDIO_SPEAKER_5POINT1` / `KSAUDIO_SPEAKER_5POINT1_SURROUND`) into the 6 physical transducers with acoustic soundstage expansion. |
| **Headphones** | Headphones (Apple T2 Audio Device) | 3.5 mm stereo output with integrated Bauer bs2b binaural crossfeed DSP (64-bit Q16 precision) and -12 dB safety attenuation. |
| **Jack Switching**| Hardware Presence Detect | Dynamic presence detection via `KSPROPERTY_JACK_DESCRIPTION2`. Automatic routing between Speakers and Headphones on physical plug/unplug events. |
| **Microphone** | Microphone Array (Apple T2 Audio) | 3-microphone studio array capture with beamforming DSP and Windows standard microphone array geometry. |
| **System Model** | SMBIOS Auto-Detection | Dynamic reading of `SystemProductName` from BIOS registry for targeted model profiling. |
| **Protection** | Kernel Crash Guard | Persistent safe-mode start guard preventing reboot loops (trips after 2 unclean terminations). |
| **Power States** | D0 / D3hot / D3cold | Full sleep and hibernation survival (`ResumeAfterSleep`). |

## Release History

### v1.0.13.19 (2026-11-02)
- Re-enabled separate native Windows audio endpoints (`SeparateHeadphoneEndpoint = 1`) by default.
- Windows exposes independent `Speakers` and `Headphones` endpoints with individual volume controls.
- Automatic hardware jack detection (`KSEVENT_PINCAPS_JACKINFOCHANGE`) seamlessly switches default device and preserves independent volume levels between speakers and headphones.

### v1.0.13.18 (2026-11-02)
- Upgraded headphone bs2b crossfeed IIR state from 32-bit `int` to 64-bit `long long`, eliminating 32-bit integer overflow on 24-bit audio that caused severe digital distortion/crackling.
- Disabled `HeadphoneCrossfeed` by default (`0`) for pure, uncolored bit-exact direct headphone passthrough.
- Retained full crossfeed capability with safe 64-bit fixed-point arithmetic if enabled.

### v1.0.13.17 (2026-11-02)
- Fixed headphone BCE stream query condition (`HeadphoneOutput == 1` gate removed) ensuring physical audio format is valid.
- Fixed `HeadphoneDeviceId` initialization in `Driver.c` in unified single-endpoint mode (bypassing separate filter registration check).
- Synchronized `JackSeenPlugged` on successful BCE query response.
- Hardened tools and test suites: dynamic PnP discovery in diagnostic scripts, full 8/8 pass in `PostInstall-Validate.ps1`, corrected DSP and pipeline unit test assertions.

### v1.0.13.16 (2026-11-02)
- Unified single audio endpoint in Windows: automatic dynamic routing between Speakers and Headphones.
- When headphones are unplugged -> sound plays only on Speakers.
- When headphones are plugged in -> sound plays only on Headphones (with bs2b crossfeed DSP & safe attenuation).
- Fixed installer and release packaging (added LICENSE, THIRD_PARTY_LICENSES, dynamic PnP discovery).

### v1.0.13.15 (2026-11-02)
- Fixed TOCTOU race in jack poll routine and teardown synchronization (prevents kernel UAF on device unload/stop).
- Added cancellation and wait loop for `T2AudioResumeWorkRoutine` during PnP Stop/Remove (`T2AudioStopResumeWork`).
- Synchronized audio format and buffer size reads under `StreamLock` in `T2AudioStreamGetPosition` and `T2AudioStreamPlayPositionBytes`.
- Added `volatile` qualification to power, jack, and mic state flags.
- Fixed `ExtraFormats` fallback restoring 32-bit minimum sample size.
- Improved Digital Mic presence check in BufferStruct and speaker buffer size validation.

### v1.0.13.14 (2026-11-02)
- Added 5.1 Virtual Surround Sound (`T2DspRender5Point1ToSpeakers`).
- Implemented SMBIOS hardware model auto-detection (`T2AudioDetectHardwareModel`).
- Added headset microphone probe integration (device 0x44, stream 0x45).
- Cleaned and organized repository layout; minimal release distribution.

### v1.0.13.13 (2026-11-02)
- Fixed hardware jack presence detection (`JACKDESC2_PRESENCE_DETECT_CAPABILITY`) for automatic headphone/speaker audio routing in Windows `AudioEndpointBuilder`.
- Upgraded Bauer bs2b crossfeed filter state to 64-bit Q16 arithmetic.

### v1.0.13.12 (2026-11-01)
- Live DSP tuning without stream restart (`T2AudioReloadLiveParameters`).
- Dynamic Loudness compensation and Anti-Rattle woofer protection limiter.

### v1.0.13.11 (2026-11-01)
- Bauer bs2b binaural crossfeed implementation for headphones.
- Windows Microphone Array Geometry descriptor integration.
- Driver migration to modern `ExAllocatePool2`.

### v1.0.13 (2026-10-29)
- Independent WaveRT headphone endpoint.
- 3-microphone studio array capture support.
- Sleep/resume power state handler.

### v1.0.12 (2026-10-28)
- Persistent boot mode with Crash Guard protection.
- Tone controls (Bass, Treble, Clarity) and 13 audio tuning presets.
