# T2AudioPort Diagnostic Tools

## Capture-T2AudioLog.ps1

Automated kernel debug log capture for T2AudioPort driver testing.

### Requirements

- Windows with Administrator privileges
- DbgViewCLI.exe (Microsoft Sysinternals DebugView CLI)
  - Download: https://learn.microsoft.com/en-us/sysinternals/downloads/debugview
  - Expected location: `C:\Users\othysa\Desktop\mbp\DebugView\DbgViewCLI.exe`

### Usage

```powershell
# Basic capture (60 seconds, no device restart)
.\tools\Capture-T2AudioLog.ps1

# Capture with device restart (recommended for driver testing)
.\tools\Capture-T2AudioLog.ps1 -RestartDevice

# Custom duration
.\tools\Capture-T2AudioLog.ps1 -RestartDevice -Duration 30

# Custom device instance ID
.\tools\Capture-T2AudioLog.ps1 -RestartDevice -InstanceId "PCI\VEN_106B&DEV_1803..."
```

### Output

Creates timestamped directory in `docs\logs\capture_YYYYMMDD_HHMMSS\`:

- `diagnostics.json` - Pre/post capture system state, git commit, driver hash
- `kernel_raw.csv` - Complete kernel debug output in CSV format
- `t2audio_messages.txt` - Filtered T2Audio messages (clean format)
- `device_restart.txt` - pnputil restart output (if `-RestartDevice` used)
- `dbgviewcli_stdout.txt` - DbgViewCLI standard output
- `dbgviewcli_stderr.txt` - DbgViewCLI error messages

### What It Does

1. Verifies Administrator privileges
2. Locates and validates DbgViewCLI.exe (checks Microsoft signature)
3. Warns about running DebugView instances
4. Creates timestamped capture directory
5. Gathers pre-capture diagnostics:
   - System boot time
   - Git commit and branch
   - Installed driver SHA256 hash
   - Device status and problem code
6. Starts DbgViewCLI with kernel capture (no Win32)
7. Waits 3 seconds for initialization
8. Optionally restarts T2 Audio device via pnputil
9. Captures for specified duration
10. Extracts T2Audio-specific messages
11. Gathers post-capture device status
12. Displays summary and file locations

### Success Criteria

- Raw log file size > 0 bytes
- T2Audio message count > 0 (if driver loaded)
- Device status: OK / Problem: CM_PROB_NONE
- DbgViewCLI exit code: 0

### Troubleshooting

**Empty log file (0 bytes)**
- Kernel capture not enabled: DbgViewCLI requires admin rights
- No kernel messages during capture window
- Debug output suppressed by another capture tool

**No T2Audio messages**
- Driver not loaded/restarted during capture
- Debug output disabled in driver build (check DbgPrint calls)
- Device restart failed (check `device_restart.txt`)

**DbgViewCLI not found**
- Download from official Microsoft Sysinternals link
- Extract to `C:\Users\othysa\Desktop\mbp\DebugView\`
- Verify signature: `Get-AuthenticodeSignature DbgViewCLI.exe`

**Running DebugView instances warning**
- Close GUI DebugView (may compete for kernel buffer)
- Kill stale DbgViewCLI processes: `Stop-Process -Name DbgViewCLI`

### Notes

- Kernel capture does not retroactively capture boot messages
  - Use `--boot-enable` for boot-time logging (requires reboot)
  - This script captures only messages during active capture window
- Device restart via pnputil may not trigger DriverEntry
  - Existing driver instance may handle restart via IRP_MN_START_DEVICE
  - Look for "StartDevice entry" in log, not "DriverEntry"
- CSV format ensures proper parsing of multi-line messages
- Script automatically filters and extracts T2Audio: prefix messages
- Full raw log retained for manual inspection if needed

### For CI/Automated Testing

```powershell
# Non-interactive mode (assumes DbgViewCLI present and UAC pre-approved)
$result = .\tools\Capture-T2AudioLog.ps1 -RestartDevice -Duration 20
if ($LASTEXITCODE -ne 0) {
    Write-Error "Capture failed"
    exit 1
}

# Parse diagnostics
$diag = Get-Content "docs\logs\capture_*\diagnostics.json" -Raw | ConvertFrom-Json | Sort-Object CaptureStart -Descending | Select-Object -First 1

if ($diag.T2AudioLineCount -eq 0) {
    Write-Warning "No T2Audio messages captured"
}

if ($diag.DeviceStatusAfter -ne "OK") {
    Write-Error "Device status after capture: $($diag.DeviceStatusAfter)"
    exit 1
}
```
