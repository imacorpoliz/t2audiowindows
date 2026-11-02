# T2AudioPort Tools & Diagnostics

This directory contains management, configuration, testing, and diagnostic utilities for the Apple T2 Audio Driver on MacBook Pro 16" (2019).

## 🚀 Primary Master Utility: `T2Audio-Setup.ps1`

The complete lifecycle is managed by **`T2Audio-Setup.ps1`** (and companion alias **`T2-Manager.ps1`**):

- **Interactive Switchboard:** Run `.\tools\T2Audio-Setup.ps1` without arguments as Administrator.
- **Preset Management:** `.\tools\T2Audio-Setup.ps1 -Action Preset -Preset macos`
- **Hardware Tone Tuning:** `.\tools\T2Audio-Setup.ps1 -Action Tune -BassDb 3 -TrebleDb 2`
- **Output Switching:** `.\tools\T2Audio-Setup.ps1 -Action Switch -To Toggle`
- **Feature Switchboard:** `.\tools\T2Audio-Setup.ps1 -Action Features -Headphones 1 -Microphone 1`
- **Sanity Verification:** `.\tools\PostInstall-Validate.ps1`

For full details, see [INSTALL_SCRIPTS_README.md](INSTALL_SCRIPTS_README.md).

---

## Diagnostic Tools

### Capture-T2AudioLog.ps1

Automated kernel debug log capture for T2AudioPort driver testing.

#### Requirements

- Windows with Administrator privileges
- DbgViewCLI.exe (Microsoft Sysinternals DebugView CLI)
  - Download: https://learn.microsoft.com/en-us/sysinternals/downloads/debugview
  - Looked up in this order: `$env:T2AUDIO_DBGVIEW`, `DebugView\DbgViewCLI.exe` next to the project folder, `C:\Program Files\DebugView\`, then `PATH`

#### Usage

```powershell
# Basic capture (60 seconds, no device restart)
.\tools\Capture-T2AudioLog.ps1

# Capture with device restart (recommended for driver testing)
.\tools\Capture-T2AudioLog.ps1 -RestartDevice

# Custom duration
.\tools\Capture-T2AudioLog.ps1 -RestartDevice -Duration 30
```

#### Output

Creates timestamped directory in `docs\logs\capture_YYYYMMDD_HHMMSS\`:

- `diagnostics.json` - Pre/post capture system state, git commit, driver hash
- `kernel_raw.csv` - Complete kernel debug output in CSV format
- `t2audio_messages.txt` - Filtered T2Audio messages (clean format)
- `device_restart.txt` - pnputil restart output (if `-RestartDevice` used)
- `dbgviewcli_stdout.txt` - DbgViewCLI standard output
- `dbgviewcli_stderr.txt` - DbgViewCLI error messages
