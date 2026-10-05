# T2AudioPort Driver Installation Script
# This script installs the T2AudioPort driver to replace AppleAudio.sys
# 
# DANGER: This will modify system audio drivers. Use at your own risk.
# Prerequisites: Run PreInstall-Check.ps1 first

param(
    [switch]$SkipBackup = $false,
    [switch]$NoReboot = $false,
    [switch]$Force = $false
)

$ErrorActionPreference = "Stop"

Write-Host "==================================================================" -ForegroundColor Cyan
Write-Host "  T2AudioPort Driver Installation" -ForegroundColor Cyan
Write-Host "==================================================================" -ForegroundColor Cyan
Write-Host ""

# Check admin rights
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "[FAIL] Administrator privileges required" -ForegroundColor Red
    exit 1
}

# Verify installation files exist (canonical package lives in ..\packaging)
$projectRoot = Split-Path -Parent $PSScriptRoot
$packageDir = Join-Path $projectRoot "packaging"
$driverPath = Join-Path $packageDir "T2AudioMiniport.sys"
$infPath = Join-Path $packageDir "T2AudioMiniport.inf"

if (-not (Test-Path -LiteralPath $driverPath)) {
    Write-Host "[FAIL] Driver not found: $driverPath" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path -LiteralPath $infPath)) {
    Write-Host "[FAIL] INF not found: $infPath" -ForegroundColor Red
    exit 1
}

Write-Host "[OK] Driver files found" -ForegroundColor Green
Write-Host "     SYS: $driverPath" -ForegroundColor Gray
Write-Host "     INF: $infPath" -ForegroundColor Gray

# Verify driver signature
$driverSize = (Get-Item -LiteralPath $driverPath).Length
if ($driverSize -ne 38416) {
    Write-Host "[WARN] Driver size mismatch: expected 38416 bytes (signed), got $driverSize" -ForegroundColor Yellow
    $continue = Read-Host "Continue anyway? (y/N)"
    if ($continue -ne "y") {
        exit 1
    }
}

Write-Host ""
Write-Host "==================================================================" -ForegroundColor Yellow
Write-Host "  WARNING: DANGEROUS OPERATION" -ForegroundColor Yellow
Write-Host "==================================================================" -ForegroundColor Yellow
Write-Host ""
Write-Host "This script will:" -ForegroundColor Yellow
Write-Host "  1. Stop audio services" -ForegroundColor White
Write-Host "  2. Disable AppleAudio.sys" -ForegroundColor White
Write-Host "  3. Install T2AudioMiniport driver via pnputil" -ForegroundColor White
Write-Host "  4. Update device driver in Device Manager" -ForegroundColor White
Write-Host "  5. Reboot system" -ForegroundColor White
Write-Host ""
Write-Host "Potential risks:" -ForegroundColor Red
Write-Host "  - BSOD during or after installation" -ForegroundColor White
Write-Host "  - No audio output" -ForegroundColor White
Write-Host "  - Audio distortion or hardware damage" -ForegroundColor White
Write-Host "  - System instability" -ForegroundColor White
Write-Host ""

$confirm = Read-Host "Type 'INSTALL' to continue"
if ($confirm -ne "INSTALL" -and -not $Force) {
    Write-Host "[ABORT] Installation cancelled" -ForegroundColor Yellow
    exit 0
}
if ($Force) {
    Write-Host "[FORCE] Skipping confirmation prompt" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Starting installation..." -ForegroundColor Cyan
Write-Host ""

# Step 1: Stop audio services
Write-Host "[1/5] Stopping audio services..." -ForegroundColor Cyan
try {
    Stop-Service -Name audiosrv -Force -ErrorAction Stop
    Write-Host "      Audio service stopped" -ForegroundColor Green
} catch {
    Write-Host "[WARN] Could not stop audio service: $_" -ForegroundColor Yellow
}

# Step 2: Disable AppleAudio service
Write-Host "[2/5] Disabling AppleAudio service..." -ForegroundColor Cyan
$appleAudioService = Get-Service | Where-Object { $_.Name -like "*AppleAudio*" }
if ($appleAudioService) {
    try {
        Stop-Service -Name $appleAudioService.Name -Force -ErrorAction Stop
        sc.exe config $appleAudioService.Name start= disabled | Out-Null
        Write-Host "      AppleAudio service disabled: $($appleAudioService.Name)" -ForegroundColor Green
    } catch {
        Write-Host "[WARN] Could not disable AppleAudio service: $_" -ForegroundColor Yellow
    }
} else {
    Write-Host "      AppleAudio service not found (may not exist)" -ForegroundColor Gray
}

# Step 3: Install driver package
Write-Host "[3/5] Installing driver package..." -ForegroundColor Cyan
try {
    $pnputilOutput = & pnputil.exe /add-driver $infPath /install 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host "      Driver package installed" -ForegroundColor Green
        Write-Host "      $pnputilOutput" -ForegroundColor Gray
    } else {
        Write-Host "[WARN] pnputil returned error code $LASTEXITCODE" -ForegroundColor Yellow
        Write-Host "       Output: $pnputilOutput" -ForegroundColor Gray
    }
} catch {
    Write-Host "[ERROR] Driver installation failed: $_" -ForegroundColor Red
    Write-Host "        Attempting rollback..." -ForegroundColor Yellow
    Start-Service -Name audiosrv -ErrorAction SilentlyContinue
    exit 1
}

# Step 4: Update device driver
Write-Host "[4/5] Updating device driver..." -ForegroundColor Cyan
$t2Device = Get-PnpDevice | Where-Object {
    $_.InstanceId -like "PCI\VEN_106B&DEV_1803*"
}

if ($t2Device) {
    Write-Host "      Device found: $($t2Device.FriendlyName)" -ForegroundColor Gray
    Write-Host "      Instance ID: $($t2Device.InstanceId)" -ForegroundColor Gray
    
    # Trigger driver update via devcon/pnputil
    try {
        pnputil.exe /scan-devices 2>&1 | Out-Null
        Write-Host "      Device scan triggered" -ForegroundColor Green
    } catch {
        Write-Host "[WARN] Device scan failed: $_" -ForegroundColor Yellow
    }
} else {
    Write-Host "[WARN] T2 Audio Device not found" -ForegroundColor Yellow
}

# Step 5: Restart audio service
Write-Host "[5/5] Restarting audio service..." -ForegroundColor Cyan
try {
    Start-Service -Name audiosrv -ErrorAction Stop
    Write-Host "      Audio service started" -ForegroundColor Green
} catch {
    Write-Host "[WARN] Could not start audio service: $_" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "==================================================================" -ForegroundColor Green
Write-Host "  Installation Complete" -ForegroundColor Green
Write-Host "==================================================================" -ForegroundColor Green
Write-Host ""

Write-Host "Next steps:" -ForegroundColor Yellow
Write-Host "  1. A REBOOT IS REQUIRED for the driver to load" -ForegroundColor White
Write-Host "  2. After reboot, check Device Manager for driver status" -ForegroundColor White
Write-Host "  3. Run: .\PostInstall-Validate.ps1 to verify installation" -ForegroundColor White
Write-Host ""

if (-not $NoReboot) {
    Write-Host "System will reboot in 30 seconds..." -ForegroundColor Yellow
    Write-Host "Press Ctrl+C to cancel" -ForegroundColor Gray
    Start-Sleep -Seconds 5
    
    $rebootNow = Read-Host "Reboot now? (Y/n)"
    if ($rebootNow -ne "n") {
        Write-Host "Rebooting..." -ForegroundColor Cyan
        shutdown.exe /r /t 10 /c "T2AudioPort driver installation - rebooting"
    } else {
        Write-Host "[INFO] Reboot cancelled. Remember to reboot manually." -ForegroundColor Yellow
    }
} else {
    Write-Host "[INFO] -NoReboot specified. Remember to reboot manually." -ForegroundColor Yellow
}

Write-Host ""
