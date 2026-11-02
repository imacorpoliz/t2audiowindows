# T2AudioPort - Management & Installation Suite
**MacBook Pro 16" (2019, MacBookPro16,1) · Apple T2 Audio for Windows**

## Overview

The entire lifecycle of the T2AudioPort driver — preparation, installation, real-time preset switching, DSP equalization, feature toggles, volume balancing, diagnostics, and rollback — is unified into a single master utility:

👉 **`tools\T2Audio-Setup.ps1`** (also accessible via companion alias `tools\T2-Manager.ps1`)

All companion scripts in `tools\` share the exact same configuration, validation rules, and device management paths.

---

## Unified Command Center: `T2Audio-Setup.ps1`

### 1. Interactive Switchboard & Dashboard
Run PowerShell as Administrator:
```powershell
.\tools\T2Audio-Setup.ps1
```
This opens the unified interactive console with live driver status, volume levels for both speakers and headphones, crash guard health, and a single-keystroke feature switchboard.

### 2. Quick CLI Actions

| Action | Command | Description |
|---|---|---|
| **Status** | `.\tools\T2Audio-Setup.ps1 -Action Status` | Live status dashboard (driver version, PnP health, speaker/headphone volume, active features). |
| **Prepare** | `.\tools\T2Audio-Setup.ps1 -Action Prepare` | Verifies Secure Boot is off and enables Windows test-signing mode. |
| **Install** | `.\tools\T2Audio-Setup.ps1 -Action Install -Preset macos` | Stages signed package, sets production parameters, un-mutes endpoints, and activates driver. |
| **Preset** | `.\tools\T2Audio-Setup.ps1 -Action Preset -Preset bright` | Switches sound profile (`macos`, `bright`, `bass`, `loud`, `movie`, `voice`, `night`, `warm`, `natural`, `raw`). |
| **Tune** | `.\tools\T2Audio-Setup.ps1 -Action Tune -BassDb 3 -TrebleDb 2` | Fine-tunes hardware tone controls (-12 dB to +12 dB) and compressor style. |
| **Balance** | `.\tools\T2Audio-Setup.ps1 -Action Balance -Volume 0.25` | Unmutes and equalizes Left/Right balance across both Speakers and Headphones. |
| **Switch** | `.\tools\T2Audio-Setup.ps1 -Action Switch -To Toggle` | Toggles default Windows playback endpoint between Speakers and Headphones. |
| **Features** | `.\tools\T2Audio-Setup.ps1 -Action Features -Headphones 1 -Microphone 1` | Enables/disables optional driver subsystems with a single safe PnP cycle. |
| **Doctor** | `.\tools\T2Audio-Setup.ps1 -Action Doctor` | Automated diagnostic audit of driver, services, registry, endpoints, and crash guard. |
| **Shortcut** | `.\tools\T2Audio-Setup.ps1 -Action Shortcut` | Creates a desktop shortcut with hotkey `Ctrl+Alt+H` to instantly switch outputs. |
| **Backup** | `.\tools\T2Audio-Setup.ps1 -Action Backup` | Exports current DSP and driver configuration to a timestamped JSON file. |
| **Restore** | `.\tools\T2Audio-Setup.ps1 -Action Restore` | Restores DSP settings from a previously saved JSON snapshot. |
| **ResetGuard**| `.\tools\T2Audio-Setup.ps1 -Action ResetGuard` | Clears watchdog crash guard strikes if an unclean shutdown occurred. |
| **Uninstall** | `.\tools\T2Audio-Setup.ps1 -Action Uninstall` | Completely removes T2AudioPort and restores Apple's official `AppleAudio.sys`. |

---

## Feature Switchboard Toggles

All features can be toggled via the interactive menu or CLI (`-Action Features`):

1. **Headphones (`-Headphones 1/0`)** — Exposes separate native 3.5 mm output endpoint in Windows with hardware jack auto-sensing and automatic speaker muting.
2. **Built-in Microphones (`-Microphone 1/0`)** — 3-microphone studio beamforming array.
3. **Resume Sound After Sleep (`-ResumeAfterSleep 1/0`)** — Automatically restarts audio streams upon lid open / power wake.
4. **16/24-bit Exclusive Formats (`-ExtraFormats 1/0`)** — Low-latency formats for DAWs and exclusive-mode WASAPI/ASIO clients.
5. **Headphone bs2b Crossfeed (`-HeadphoneCrossfeed 1/0`)** — Bauer stereophonic-to-binaural DSP in 64-bit fixed point (off by default for pure clean passthrough).
6. **Loudness Compensation (`-DspLoudness 1/0`)** — Fletcher-Munson dynamic low-volume bass and treble compensation.
7. **Woofer Anti-Rattle (`-AntiRattle 1/0`)** — Dynamic limiter preventing mechanical cone bottoming on heavy transients.

---

## Companion Scripts (Unified Wrappers)

To ensure muscle-memory compatibility, the specialized scripts forward to the unified core:

- **`T2-Manager.ps1`** — Drop-in alias for `T2Audio-Setup.ps1`.
- **`Install-T2AudioDriver.ps1`** — 1-click installer forwarding to `T2Audio-Setup.ps1 -Action Install`.
- **`Rollback-T2AudioDriver.ps1`** — 1-click restore forwarding to `T2Audio-Setup.ps1 -Action Uninstall`.
- **`Set-DefaultAudioDevice.ps1`** — PolicyConfig helper to switch Windows default output.
- **`Restart-T2AudioService.ps1`** — Gracefully cycles `AudioEndpointBuilder`, `Audiosrv`, and the T2 PnP device without hanging.
- **`PostInstall-Validate.ps1`** — Non-blocking 8-point automated sanity verification.
- **`Make-ReleasePackage.ps1`** — Builds distribution zips including catalog, driver, licenses, and documentation.
- **`Capture-T2AudioLog.ps1`** — Captures kernel debug output via DbgViewCLI with dynamic device ID resolution.
