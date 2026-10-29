# Hardware Log Excerpt - Step B: Driver 1.0.13.5 & English Installer Verification

**Date**: 2026-10-29  
**Machine**: Apple MacBookPro16,1 (T2 Chip)  
**OS**: Windows 11 x64  
**Driver Version**: `1.0.13.5`  
**Driver SHA256**: `090A7862A8DA27117471C750BECA13F22409ED22B5CF49A680239C15F2A2CCBA`  
**Signed**: `T2AudioPort Test Certificate` (SHA1: `CA2DE95D1B065F6D6CFF6C4158DC88E2424ABBCE`)

---

## 1. Test Suite Results (Host)
All 7 host unit test suites were executed on the test machine:
1. `BceProtocolLogicTest.exe` - **ALL PASSED** (0 failures)
2. `T2AudioBufferLogicTest.exe` - **ALL PASSED** (0 failures)
3. `T2AudioCrashGuardTest.exe` - **ALL PASSED** (0 failures)
4. `T2AudioDspTest.exe` - **ALL PASSED** (0 failures, all 4 levels, limiter, autogain, LUFS, tone controls)
5. `T2AudioMicTest.exe` - **ALL PASSED** (0 failures)
6. `T2AudioPipelineTest.exe` - **ALL PASSED** (0 failures)
7. `T2AudioSpeakerLogicTest.exe` / `test_lr.exe` - **ALL PASSED** (0 failures)

---

## 2. Installer & Endpoint Verification (`-Action Install -Preset macos`)
- Executed `.\tools\T2Audio-Setup.ps1 -Action Install -Preset macos -Force`.
- **English UI Output**:
  ```text
  ╔══════════════════════════════════════════════════════════╗
  ║        T2AudioPort  ·  Apple T2 audio for Windows        ║
  ║                  MacBook Pro 16" (2019)                  ║
  ╚══════════════════════════════════════════════════════════╝

  ── Installing T2AudioPort 
   ✓ Package: 10/29/2026,1.0.13.5, SHA256 090A7862A8DA2711…

   ! This driver is experimental. Once installed it starts on every boot.
     If two sessions in a row end uncleanly (BSOD, hang), it stops starting by itself;
     the Apple driver comes back with this script ("Restore AppleAudio") or from Safe Mode.
   ✓ Preset 'Like macOS (recommended)': SpeakerDsp=3, DspGainDb=6, TweeterTrimDb=0, WooferLimitDb=0, DspAutoGain=0, BassDb=0, TrebleDb=0, ClarityDb=0, CompStyle=0
   ✓ Persistent mode on, crash guard reset.
     Binding the driver to the device...
   ✓ T2AudioPort is installed and running.
   ✓ T2 speakers: unmuted, volume 30 %, L/R balance evened (mute=False volume=0,30; balance before: L=0,24 R=0,24, now: L=0,30 R=0,30)

     Pick the Apple T2 "Speakers" as the output in Sound settings if Windows did not do it by itself.
     Change the preset any time: .\T2Audio-Setup.ps1 -Action Preset -Preset bright
  ```
- Confirmed that volume and unmute target the **T2 speakers** endpoint, leaving headphones untouched.
- Balance evened at `L=0.30 R=0.30`.

---

## 3. Status Verification (`-Action Status`)
- Output:
  ```text
  ── Status 
   Driver:         T2AudioPort  1.0.13.5
   Device:         OK
   Persistent mode: on
   Preset:         Like macOS (recommended)
   Tone:           bass 0 dB, treble 0 dB, clarity 0 dB, compressor fast
   Crash guard:    OK
   T2 volume:      mute=False volume=0,30 L=0,30 R=0,30
   Headphones (3.5 mm output)                     on
   Built-in microphones                           on
   Sound back after sleep by itself               on
   16/24-bit formats                              off
   testsigning:    on
  ```

---

## 4. Optional Features Verification
- Ran: `.\tools\T2Audio-Setup.ps1 -Action Features -Headphones 1 -Microphone 1 -ResumeAfterSleep 1`
- Registry check: `(Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\T2AudioMiniport\Parameters).MicStartIo` = **`1`**.
- All three endpoints registered and `OK` (`CM_PROB_NONE`):
  - `Speakers (Apple T2 Audio Device (6-channel Native Driver))`
  - `Headphones (Apple T2 Audio Device (6-channel Native Driver))`
  - `Microphone Array (Apple T2 Audio Device (6-channel Native Driver))`

---

## 5. Windows Update Guard (`-Action UpdateGuard`)
- Tested `-Block 1 -Force`:
  - `HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\ExcludeWUDriversInQualityUpdate` = `1`
  - `HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\DriverSearching\SearchOrderConfig` = `0`
- Tested `-Block 0 -Force`: restores previous values and removes saved parameters.
- Re-armed with `-Block 1 -Force` to prevent Windows Update from replacing T2AudioPort with AppleAudio.
