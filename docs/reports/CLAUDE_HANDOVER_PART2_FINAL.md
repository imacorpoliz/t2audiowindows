# Handover Report for Claude - Part 2 Completion (Microphone, Headphones, Sleep)

**Date**: 2026-10-29  
**Machine**: Apple MacBookPro16,1 (T2 Chip)  
**Author**: Gemini Agent  
**Target Branch**: `gemini/part2-1.0.13.0` -> `origin/diagnostics` ([PR #13](https://github.com/imacorpoliz/t2audiowindows/pull/13))  
**Owner Verdict**: *"все работает шикарно, только звук с наушников на динамики и обратно сам не переключается приходится вручную тыкать в микшере с наушников на динамики. пиши отчет для клода дальше он сам, микрофон точно работает?"* ("Everything works wonderfully, except sound doesn't switch automatically between headphones and speakers, have to click manually in mixer. Write report for Claude, from here on he will take over. Is the microphone definitely working?")

---

## 1. Executive Summary

All three core endpoints are fully functional and verified on bare-metal hardware:
1. **Speakers (`Speakers (Apple T2 Audio Device (6-channel Native Driver))`)**:
   - Stereo endpoint with 6-channel hardware mapping and DSP level 3 (`SpeakerDsp = 3`).
   - Clean, stable playback, zero dropouts.
2. **Microphones (`Microphone Array (Apple T2 Audio Device (6-channel Native Driver))`)**:
   - 48 kHz mono 32-bit capture from BridgeOS 3-channel mic array (`Digital Mic`, device `0x21`, stream `0x22`).
   - `START_IO` and `STOP_IO` to device `0x21` confirmed (`0x00000000`).
   - Real acoustic recording confirmed: `Record-WasapiMic.ps1` recorded `C:\Users\othysa\Desktop\mic_test.wav` (Peak = 0.177, RMS = -37 dBFS).
3. **Headphones (`Headphones (Apple T2 Audio Device (6-channel Native Driver))`)**:
   - 48 kHz stereo 24-in-32 bit output to BridgeOS 3.5 mm jack DAC (`Codec Output`, device `0x25`, stream `0x26`).
   - `START_IO` and `STOP_IO` to device `0x25` confirmed (`0x00000000`).
   - Owner confirmed physical playback works ("все работает шикарно").

---

## 2. Key Technical Breakthroughs & Bug Fixes

### A. Dynamic BufferStruct Device Append (1.0.13.3)
- **Problem**: On cold boot, BridgeOS initializes `BufferStruct` with only 3 output devices (`Bridge Loopback`, `Speaker`, `Codec Output`). `"Digital Mic"` was absent from device memory.
- **Reverse Engineering**: Static analysis of `AppleAudio.sys` (`FUN_140004558`) revealed AppleAudio dynamically appends `"Digital Mic"` into `BufferStruct.Devices[3]`, sets `NumInputStreams = 1`, and increments `NumDevices` from 3 to 4.
- **Fix**: Implemented `T2AudioMicFindOrAddDeviceLocked` matching AppleAudio.

### B. BCE Message 9 (`SET_PROPERTY 'nsrt'`, 66 bytes, 48000.0)
- **Problem**: Missing nominal sample rate configuration before `START_IO` caused firmware desync.
- **Fix**: Implemented `T2AudioBceBuildSetPropertyNominalSampleRate` and floating-point-free `T2AudioBceEncodeSampleRateToDoubleBits` to prevent kernel `_fltused` emission.

### C. D0 Resume Work Item Collision & Watchdog Panic (1.0.13.4)
- **Problem**: During Step P2-B testing, audio capture began and transferred 258,996 frames, but crashed after 5 seconds with a watchdog reset (`Kernel-Power` Event 41).
- **Diagnosis via `DbgViewCLI`**: `PowerChangeState(PowerDeviceD0)` queued `T2AudioResumeWorkRoutine` unconditionally on driver load. After a 500 ms sleep, the work item executed while WASAPI capture was actively streaming, sending BCE Msg 18 (`SET_INPUT_STREAM_ADDRESS_RANGES`) and BCE Msg 9 (`SET_PROPERTY 'nsrt'`) to BridgeOS. BridgeOS firmware blocked property modifications during streaming, hitting a 5000 ms BCE timeout (`0xC0000186`).
- **Fix**:
  1. Guarded `PowerChangeState(D0)`: only queue `T2AudioResumeWorkRoutine` when waking from an actual power-down state (`wasPoweredDown == TRUE`).
  2. Guarded `T2AudioResumeWorkRoutine` and `T2AudioMicReRegister`: check `context->MicIoActive == 0`. Never re-announce ranges or change sample rates while I/O is active.
- **Result**: Tested on hardware (build 1.0.13.4, SHA256 `EFDE38A52B51E458C53A651FC35528CCD43EFFCE85788278A89FD6F7F223B22F`). 141,486 frames captured, monotonic `VHCI UPDATE_TIMESTAMP` #1..#8, zero strikes, zero timeouts, clean `STOP_IO`.

---

## 3. Findings on Headphone Auto-Switching

### Owner Observation
The owner reports that headphones sound works, but Windows does not automatically switch default audio between Speakers and Headphones when plugging/unplugging the 3.5 mm jack; the user manually selects the device in the Windows volume tray / mixer.

### Technical Analysis
- The driver exposes two independent PortCls filter pairs:
  - `Speakers` (`TopologySpeaker` + `WaveSpeaker`)
  - `Headphones` (`TopologyHeadphone` + `WaveHeadphone`)
- Automatic switching in Windows requires Windows MMDevAPI to receive a Jack Status Change event (`KSEVENT_PINCAPS_JACKINFOCHANGE`) signaling that the jack is unplugged, which causes Windows to redirect streams to the next available endpoint.
- In `BceTransport.c`:
  - `status = T2AudioGetU32Property(0x25, 0x25, 'outp', 'jack', &plugged);`
  - In kernel logs, this query succeeds (`status = 0x00000000`), but `plugged` is consistently returned as `0` by BridgeOS on MacBookPro16,1, even with headphones inserted.
  - As Claude previously noted in `JackProbeAll` comments:
    `// on hardware the 'outp'/'jack' read of "Codec Output" stayed 0 through plug/unplug (EXCERPT_20261028_step5_presets.md)`
- Because `plugged` stays 0, `JackSeenPlugged` is never set, and the driver defaults `T2AudioTopologyJackConnected` to `TRUE` so the endpoint remains selectable in Windows.
- **Recommended next steps for Claude**:
  1. Inspect the 8 probe addresses from `JackProbeAll` or RE `AppleAudio.sys` to identify the real selector/scope for 3.5 mm jack insertion (e.g. `Codec Input` `0x44`, `'pfta'`, or `0x222020` asynchronous VHCI notifications).
  2. Alternatively, evaluate a unified single endpoint architecture where PortCls exposes one "Speakers/Headphones" endpoint and the driver switches hardware output rings internally when jack presence is detected.

---

## 4. Current Configuration & Installed Driver

- **Installed Driver Binary**: `C:\Windows\System32\drivers\T2AudioMiniport.sys`
- **Version**: `1.0.13.4`
- **Driver SHA256**: `EFDE38A52B51E458C53A651FC35528CCD43EFFCE85788278A89FD6F7F223B22F`
- **Certificate**: Signed with `T2AudioPort Test Certificate` (SHA1: `CA2DE95D1B065F6D6CFF6C4158DC88E2424ABBCE`).
- **Registry Parameters**:
  - `EnableProduction = 1`
  - `CrashGuardStrikes = 0`
  - `SpeakerDsp = 3`
  - `MicInput = 1`
  - `MicStartIo = 1`
  - `HeadphoneOutput = 1`
  - `JackEvents = 1`
  - `HeadphoneAutoMute = 1`
  - `ResumeAfterSleep = 1`
  - `ExtendedProbe = 1`
- **PnP Status**:
  - `Apple T2 Audio Device (6-channel Native Driver)`: `OK` (`CM_PROB_NONE`)
  - `Speakers`: `OK` (`CM_PROB_NONE`)
  - `Headphones`: `OK` (`CM_PROB_NONE`)
  - `Microphone Array`: `OK` (`CM_PROB_NONE`)
