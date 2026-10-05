<#
.SYNOPSIS
    Automated kernel debug log capture for T2AudioPort driver

.DESCRIPTION
    Captures kernel debug output using DbgViewCLI, optionally restarts the device,
    and extracts T2Audio-specific messages with diagnostic information.

.PARAMETER Duration
    Capture duration in seconds (default: 60)

.PARAMETER InstanceId
    PnP device instance ID to restart (default: T2 Audio device)

.PARAMETER RestartDevice
    If specified, restarts the device after capture starts

.EXAMPLE
    .\Capture-T2AudioLog.ps1 -RestartDevice -Duration 30
#>

[CmdletBinding()]
param(
    [int]$Duration = 60,
    [string]$InstanceId = "PCI\VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01\4&3AC8FC3&0&03D8",
    [switch]$RestartDevice
)

$ErrorActionPreference = "Stop"

# Check administrator rights
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error "This script requires Administrator privileges. Please run as Administrator."
    exit 1
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptRoot

# Locate DbgViewCLI
$dbgViewCLI = @(
    "$scriptRoot\..\DebugView\DbgViewCLI.exe",
    "C:\Users\othysa\Desktop\mbp\DebugView\DbgViewCLI.exe",
    "C:\Program Files\DebugView\DbgViewCLI.exe",
    "C:\Tools\DebugView\DbgViewCLI.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $dbgViewCLI) {
    Write-Error "DbgViewCLI.exe not found. Download from https://learn.microsoft.com/en-us/sysinternals/downloads/debugview"
    exit 1
}

Write-Host "Using DbgViewCLI: $dbgViewCLI" -ForegroundColor Cyan

# Verify signature
$sig = Get-AuthenticodeSignature $dbgViewCLI
if ($sig.Status -ne "Valid" -or $sig.SignerCertificate.Subject -notlike "*Microsoft Corporation*") {
    Write-Warning "DbgViewCLI signature verification failed: $($sig.Status)"
    Write-Warning "Signer: $($sig.SignerCertificate.Subject)"
    $confirm = Read-Host "Continue anyway? (yes/no)"
    if ($confirm -ne "yes") { exit 1 }
}

# Check for running instances
$running = Get-Process -Name "DbgViewCLI","Dbgview" -ErrorAction SilentlyContinue
if ($running) {
    Write-Warning "Found running DebugView instances:"
    $running | Format-Table Name, Id, StartTime
    Write-Warning "These may interfere with capture. Consider closing them."
    $confirm = Read-Host "Continue anyway? (yes/no)"
    if ($confirm -ne "yes") { exit 1 }
}

# Create timestamped capture directory
$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$captureDir = Join-Path $projectRoot "docs\logs\capture_$timestamp"
New-Item -ItemType Directory -Path $captureDir -Force | Out-Null

Write-Host "`nCapture directory: $captureDir" -ForegroundColor Cyan

# Gather pre-capture diagnostics
$diagnostics = @{
    CaptureStart = Get-Date
    SystemBootTime = (Get-CimInstance -ClassName Win32_OperatingSystem).LastBootUpTime
    Duration = $Duration
    RestartDevice = $RestartDevice.IsPresent
}

# Get current git commit
Push-Location $projectRoot
try {
    $diagnostics.GitCommit = git rev-parse HEAD 2>$null
    $diagnostics.GitBranch = git rev-parse --abbrev-ref HEAD 2>$null
} catch {
    $diagnostics.GitCommit = "unknown"
    $diagnostics.GitBranch = "unknown"
}
Pop-Location

# Check installed driver
$driverPath = "C:\Windows\System32\drivers\T2AudioMiniport.sys"
if (Test-Path $driverPath) {
    $diagnostics.DriverHash = (Get-FileHash $driverPath -Algorithm SHA256).Hash
    $diagnostics.DriverSize = (Get-Item $driverPath).Length
} else {
    $diagnostics.DriverHash = "not_found"
    $diagnostics.DriverSize = 0
}

# Get device status before restart
try {
    $device = Get-PnpDevice -InstanceId $InstanceId -ErrorAction Stop
    $diagnostics.DeviceStatusBefore = $device.Status
    $diagnostics.DeviceProblemBefore = $device.Problem
} catch {
    Write-Warning "Device not found: $InstanceId"
    $diagnostics.DeviceStatusBefore = "not_found"
    $diagnostics.DeviceProblemBefore = "not_found"
}

# Save diagnostics
$diagnostics | ConvertTo-Json | Out-File (Join-Path $captureDir "diagnostics.json")

Write-Host "`n=== Pre-Capture Diagnostics ===" -ForegroundColor Yellow
Write-Host "System boot time:  $($diagnostics.SystemBootTime)"
Write-Host "Git commit:        $($diagnostics.GitCommit)"
Write-Host "Driver hash:       $($diagnostics.DriverHash)"
Write-Host "Device status:     $($diagnostics.DeviceStatusBefore)"
Write-Host "Device problem:    $($diagnostics.DeviceProblemBefore)"

# Prepare capture files
$rawLogFile = Join-Path $captureDir "kernel_raw.csv"
$stdoutFile = Join-Path $captureDir "dbgviewcli_stdout.txt"
$stderrFile = Join-Path $captureDir "dbgviewcli_stderr.txt"

# Start DbgViewCLI
Write-Host "`n=== Starting DbgViewCLI ===" -ForegroundColor Yellow
Write-Host "Duration: $Duration seconds"
Write-Host "Format: CSV"
Write-Host "Filters: kernel only, no Win32"

$cliArgs = @(
    "--kernel"
    "--no-win32"
    "--duration", $Duration
    "--format", "csv"
    "--log", $rawLogFile
    "--no-banner"
    "--clock-ms"
    "--accepteula"
)

Write-Host "Command: $dbgViewCLI $($cliArgs -join ' ')"

$process = Start-Process -FilePath $dbgViewCLI -ArgumentList $cliArgs -PassThru -NoNewWindow `
    -RedirectStandardOutput $stdoutFile -RedirectStandardError $stderrFile

Write-Host "DbgViewCLI started with PID: $($process.Id)" -ForegroundColor Green

# Wait for capture to initialize
Write-Host "Waiting 3 seconds for kernel capture initialization..."
Start-Sleep -Seconds 3

# Check if process is still running
if ($process.HasExited) {
    Write-Error "DbgViewCLI exited prematurely with code: $($process.ExitCode)"
    Write-Host "STDERR content:"
    Get-Content $stderrFile -ErrorAction SilentlyContinue
    exit 1
}

# Restart device if requested
if ($RestartDevice) {
    Write-Host "`n=== Restarting Device ===" -ForegroundColor Yellow
    Write-Host "Instance ID: $InstanceId"
    
    $restartOutput = pnputil /restart-device $InstanceId 2>&1
    $restartExitCode = $LASTEXITCODE
    
    $restartOutput | Out-File (Join-Path $captureDir "device_restart.txt")
    
    Write-Host "Restart exit code: $restartExitCode"
    if ($restartExitCode -eq 0) {
        Write-Host "Device restart SUCCESS" -ForegroundColor Green
    } else {
        Write-Warning "Device restart returned non-zero exit code"
    }
    
    Write-Host "Waiting 2 seconds for driver initialization..."
    Start-Sleep -Seconds 2
}

# Wait for capture to complete
$remaining = $Duration - 5
if ($RestartDevice) { $remaining -= 2 }
if ($remaining -gt 0) {
    Write-Host "`nCapturing for $remaining more seconds..."
    Start-Sleep -Seconds $remaining
}

# Wait for process to exit
Write-Host "Waiting for DbgViewCLI to finish..."
$process.WaitForExit(10000) | Out-Null

if (-not $process.HasExited) {
    Write-Warning "DbgViewCLI did not exit cleanly, stopping..."
    $process.Kill()
}

Write-Host "DbgViewCLI exit code: $($process.ExitCode)" -ForegroundColor Cyan

# Get device status after restart
try {
    $device = Get-PnpDevice -InstanceId $InstanceId -ErrorAction Stop
    $diagnostics.DeviceStatusAfter = $device.Status
    $diagnostics.DeviceProblemAfter = $device.Problem
} catch {
    $diagnostics.DeviceStatusAfter = "not_found"
    $diagnostics.DeviceProblemAfter = "not_found"
}

# Process captured log
Write-Host "`n=== Processing Captured Log ===" -ForegroundColor Yellow

if (-not (Test-Path $rawLogFile)) {
    Write-Error "Raw log file not created: $rawLogFile"
    Write-Host "Check stderr:"
    Get-Content $stderrFile -ErrorAction SilentlyContinue
    exit 1
}

$rawLogSize = (Get-Item $rawLogFile).Length
Write-Host "Raw log size: $rawLogSize bytes"

if ($rawLogSize -eq 0) {
    Write-Warning "Raw log file is EMPTY. Kernel capture may have failed."
    Write-Host "`nPossible causes:"
    Write-Host "1. Kernel debug output is not enabled"
    Write-Host "2. No kernel messages were generated during capture"
    Write-Host "3. DbgViewCLI does not have proper permissions"
    Write-Host "`nCheck stderr output:"
    Get-Content $stderrFile -ErrorAction SilentlyContinue
} else {
    # Extract T2Audio lines
    $t2AudioFile = Join-Path $captureDir "t2audio_messages.txt"
    $allLines = Get-Content $rawLogFile
    $t2Lines = $allLines | Where-Object { $_ -match "T2Audio:" }
    
    Write-Host "Total lines captured: $($allLines.Count)"
    Write-Host "T2Audio lines found: $($t2Lines.Count)" -ForegroundColor Cyan
    
    if ($t2Lines.Count -gt 0) {
        # Parse CSV and extract clean messages
        $cleanMessages = $t2Lines | ForEach-Object {
            # CSV format: "timestamp","source","message"
            if ($_ -match '"([^"]*?)","([^"]*?)","(.*?T2Audio:.*?)"') {
                "$($matches[1]) $($matches[3])"
            } else {
                $_
            }
        }
        
        $cleanMessages | Out-File $t2AudioFile
        
        Write-Host "`nT2Audio messages extracted to: $t2AudioFile" -ForegroundColor Green
        
        Write-Host "`n=== T2Audio Messages (last 40) ===" -ForegroundColor Yellow
        $cleanMessages | Select-Object -Last 40 | ForEach-Object { Write-Host $_ }
        
    } else {
        Write-Warning "No T2Audio messages found in captured output"
        Write-Host "`nThis could mean:"
        Write-Host "1. Driver did not load/restart during capture"
        Write-Host "2. Debug output is disabled in driver build"
        Write-Host "3. Device restart was not triggered"
    }
}

# Update and save final diagnostics
$diagnostics.CaptureEnd = Get-Date
$diagnostics.CaptureDurationActual = ($diagnostics.CaptureEnd - $diagnostics.CaptureStart).TotalSeconds
$diagnostics.RawLogSize = $rawLogSize
$diagnostics.T2AudioLineCount = if ($t2Lines) { $t2Lines.Count } else { 0 }
$diagnostics.DbgViewExitCode = $process.ExitCode

$diagnostics | ConvertTo-Json | Out-File (Join-Path $captureDir "diagnostics.json")

# Summary
Write-Host "`n=== Capture Summary ===" -ForegroundColor Green
Write-Host "Capture directory: $captureDir"
Write-Host "Raw log size: $rawLogSize bytes"
Write-Host "T2Audio messages: $($diagnostics.T2AudioLineCount)"
Write-Host "Device status before: $($diagnostics.DeviceStatusBefore) / Problem: $($diagnostics.DeviceProblemBefore)"
Write-Host "Device status after:  $($diagnostics.DeviceStatusAfter) / Problem: $($diagnostics.DeviceProblemAfter)"
Write-Host "Actual duration: $([math]::Round($diagnostics.CaptureDurationActual, 1)) seconds"

Write-Host "`nFiles created:"
Get-ChildItem $captureDir | Select-Object Name, Length | Format-Table -AutoSize

if ($diagnostics.T2AudioLineCount -eq 0 -and $RestartDevice) {
    Write-Warning "`nNo T2Audio messages captured despite device restart."
    Write-Warning "Verify that driver is actually loading and DbgPrint is enabled."
}

Write-Host "`nCapture complete." -ForegroundColor Green
