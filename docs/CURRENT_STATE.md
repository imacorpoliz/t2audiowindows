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

### v1.0.13.29 (2026-11-05, installed on the test machine)
- Built-in microphone endpoint declared as a plain microphone (was a
  3-capsule array with geometry, while the stream is mono).
- `MicDsp=3` (`-Action Features -MicBeam 1`, off by default): level-matched
  average of all three capsules. Host test: +2 dB speech-to-noise against
  MicDsp=1. On hardware it adapts (mic1 gain ~1.7) and records about 2-4 dB
  louder; quality by ear not judged yet.
- Headphone run summary logs per-channel peaks and frames with the right
  channel at digital zero while the left plays, to tell a driver-side
  channel loss from one at the jack (reported right-channel dropouts with
  external speakers on the 3.5 mm jack; no channel-specific path found in
  the driver).

### v1.0.13.27 (2026-11-04)
Hardware run on MacBookPro16,1, Windows 11 build 28000 (2026-11-03/04), installed
through `-Action Install` (update path, settings kept):

- **Jack switching fixed for real.** `KSPROPERTY_JACK_DESCRIPTION(2)` and
  `KSEVENT_PINCAPS_JACKINFOCHANGE` were in the *pin* automation tables; Windows
  asks the topology *filter* with a KSP_PIN, so it had never read the jack
  (no `JackProperty` line in any log). They are now on the filters (speaker,
  headphone, both microphone topologies). Verified: Windows queries the jack
  right after start, and with nothing plugged in the headphone endpoint is
  `Unplugged` (state 8). Plugging in by hand on this build is still to do.
- **Auto-switch:** with `HeadphoneAutoMute` the speaker pin reports
  "unplugged" while headphones are in, so Windows moves the default output by
  itself (it only does so when the current default goes away). The built-in
  microphones yield to a headset the same way.
- **Headset microphone:** "Codec Input" (device 0x48 on this boot) answers
  `'inpt'/'jack'` with 0xC0000010; "Codec Output" answers it. The poll falls
  back to Codec Output. Stream 0x49: mono S32, flags 0xC, latency 47 frames.
  Capture itself not yet tested (no headset at hand).
- **Built-in microphones:** recording verified (RMS -42 dBFS room noise).
  Per-capsule levels differ: mic1 is 4.6 dB below mic0 / mic2.
- **MicDsp=2 (beta, off by default):** noise canceller - voice mic plus one
  reference from the other two (level-matched, DC-blocked). Adapts on hardware
  (100 % of samples, no resets); the speaker-sound reduction could not be
  measured at the very low test volume and is not claimed yet.
- MicDsp, MicVoiceMic and MicNoiseSuppression change live (no restart).
- Event-driven WaveRT (`IMiniportWaveRTStreamNotification`) for render and
  capture; 16/24-bit capture with `ExtraFormats`.
- Installer: keeps settings on update, checks the package (signatures, INF in
  the catalog), warns on downgrade, removes stale driver-store copies (11
  removed on the test machine), returns exit codes; menu fixes.
- Release artifacts: `T2AudioMiniport.sys` SHA256
  `833FE84612BA7624D5D94415B91CB1829EF8825932E340FD7A46070E9CBDFE98`;
  `t2audiominiport.cat` SHA256
  `A18EC68E3138CA4CEFAF4FEC084F2FFF4DDB24072CF30262006F9FCB9A208C30`.

### v1.0.13.21 (2026-11-02)
- Fixed headphone auto-switching: corrected initial presence reporting in `Topology.c` (`KSJACK_DESCRIPTION2.DeviceStateInfo`) and hardware validation in `BceTransport.c` (`T2AudioProbeBceDevices` sets `JackSeenPlugged` on successful property read).
- Restored clean direct headphone rendering: delegated headphone frame processing to `T2AudioStreamRenderHeadphoneFrames` supporting both stereo and surround downmixing with pure passthrough (crossfeed off by default).
- Added hardware speaker auto-cut: speaker stream automatically zeroes output frames while headphones are plugged in when `HeadphoneAutoMute=1`.
- Defaulted `JackEvents=1` and `HeadphoneAutoMute=1` across the kernel driver and installer.
- Unified all management and installation scripts into `tools\T2Audio-Setup.ps1` with companion wrapper `tools\T2-Manager.ps1`.
- Expanded `Set-T2Volume` to initialize, unmute, and balance both Speakers and Headphones endpoints.

### v1.0.13.20 (2026-11-02)
- Fixed jack polling admission and teardown synchronization; the poll now starts
  only when jack events and a valid headphone device are available.
- Replaced fixed teardown timeouts for jack/resume work items with lock-protected
  completion events, preventing device-context use-after-free during PnP stop.
- Hardened Digital Mic table-capacity validation and the installer feature
  switchboard; preset/feature changes apply with one safe device restart.
- Host test executables pass; physical hardware validation of these new changes
  remains pending in the current environment.
- Release artifacts: `T2AudioMiniport.sys` SHA256
  `632FD1D84AB411EF0093AAA2AD03F475803A6790EFA69A6CB0A635C5D0BDC45E`;
  `t2audiominiport.cat` SHA256
  `9A4685F00BF65E97FABA13C7F118BE73ABDB7DEA04849E6ED1AED91DAFCEFEE1`;
  ZIP SHA256
  `CEF13C9DCE6721884F6D5546934FEDB7CCCFAF9E5265634E3D3A7FB975641213`.

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
