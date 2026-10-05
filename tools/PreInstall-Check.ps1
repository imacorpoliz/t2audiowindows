# T2AudioPort Pre-Installation Safety Check
# Run this script BEFORE installing the driver
# This script will:
# 1. Verify system state
# 2. Create backups
# 3. Check prerequisites
# 4. Enable kernel debugging if needed

param(
    [switch]$SkipDebugger = $false,
    [switch]$Force = $false
)

$ErrorActionPreference = "Stop"
$WarningPreference = "Continue"

Write-Host "==================================================================" -ForegroundColor Cyan
Write-Host "  T2AudioPort Driver - Pre-Installation Safety Check" -ForegroundColor Cyan
Write-Host "==================================================================" -ForegroundColor Cyan
Write-Host ""

# Check admin rights
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "[FAIL] Administrator privileges required" -ForegroundColor Red
    exit 1
}
Write-Host "[OK] Running as Administrator" -ForegroundColor Green

# Check Windows version
$osVersion = [System.Environment]::OSVersion.Version
if ($osVersion.Major -lt 10) {
    Write-Host "[FAIL] Windows 10/11 required" -ForegroundColor Red
    exit 1
}
Write-Host "[OK] Windows version: $($osVersion.Major).$($osVersion.Minor)" -ForegroundColor Green

# Check if running on MacBook Pro 16,1
$computerModel = (Get-WmiObject -Class Win32_ComputerSystem).Model
Write-Host "[INFO] Computer model: $computerModel" -ForegroundColor Yellow
if ($computerModel -notlike "*MacBook*" -and -not $Force) {
    Write-Host "[WARN] Not a MacBook - use -Force to override" -ForegroundColor Yellow
    exit 1
}

# Check for Apple T2 Audio Device
$t2Device = Get-PnpDevice | Where-Object {
    $_.InstanceId -like "PCI\VEN_106B&DEV_1803*"
}
if (-not $t2Device) {
    Write-Host "[FAIL] Apple T2 Audio Device not found" -ForegroundColor Red
    Write-Host "       Expected: PCI\VEN_106B&DEV_1803" -ForegroundColor Red
    exit 1
}
Write-Host "[OK] Apple T2 Audio Device found: $($t2Device.InstanceId)" -ForegroundColor Green
Write-Host "     Status: $($t2Device.Status)" -ForegroundColor Gray

# Check AppleAudio.sys exists
$appleAudioPath = "C:\Windows\System32\drivers\AppleAudio.sys"
if (-not (Test-Path -LiteralPath $appleAudioPath)) {
    Write-Host "[WARN] AppleAudio.sys not found at $appleAudioPath" -ForegroundColor Yellow
} else {
    Write-Host "[OK] AppleAudio.sys found" -ForegroundColor Green
}

# Create backup directory
$backupDir = "C:\Users\$env:USERNAME\Desktop\mbp\T2AudioPort\Backup"
if (-not (Test-Path -LiteralPath $backupDir)) {
    New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
}
Write-Host "[OK] Backup directory: $backupDir" -ForegroundColor Green

# Backup AppleAudio.sys
if (Test-Path -LiteralPath $appleAudioPath) {
    $timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $backupPath = Join-Path $backupDir "AppleAudio.sys.$timestamp.bak"
    Copy-Item -LiteralPath $appleAudioPath -Destination $backupPath -Force
    Write-Host "[OK] Backup created: $backupPath" -ForegroundColor Green
    
    # Verify backup
    $originalHash = (Get-FileHash -LiteralPath $appleAudioPath -Algorithm SHA256).Hash
    $backupHash = (Get-FileHash -LiteralPath $backupPath -Algorithm SHA256).Hash
    if ($originalHash -eq $backupHash) {
        Write-Host "[OK] Backup verified (SHA256 match)" -ForegroundColor Green
    } else {
        Write-Host "[FAIL] Backup verification failed" -ForegroundColor Red
        exit 1
    }
}

# Check test signing
$testSigningEnabled = (bcdedit.exe /enum | Select-String "testsigning\s+Yes").Matches.Success
if ($testSigningEnabled) {
    Write-Host "[OK] Test signing is enabled" -ForegroundColor Green
} else {
    Write-Host "[WARN] Test signing is disabled" -ForegroundColor Yellow
    Write-Host "       Driver is unsigned and will not load" -ForegroundColor Yellow
}

# Check kernel debugging
if (-not $SkipDebugger) {
    $debugEnabled = (bcdedit.exe /enum | Select-String "debug\s+Yes").Matches.Success
    if ($debugEnabled) {
        Write-Host "[OK] Kernel debugging is enabled" -ForegroundColor Green
    } else {
        Write-Host "[WARN] Kernel debugging is disabled" -ForegroundColor Yellow
        Write-Host "       STRONGLY RECOMMENDED for first installation" -ForegroundColor Yellow
        
        $enableDebug = Read-Host "Enable kernel debugging now? (y/N)"
        if ($enableDebug -eq "y") {
            bcdedit.exe /debug on | Out-Null
            bcdedit.exe /dbgsettings serial debugport:1 baudrate:115200 | Out-Null
            Write-Host "[OK] Kernel debugging enabled (serial port 1, 115200 baud)" -ForegroundColor Green
            Write-Host "     Reboot required to activate" -ForegroundColor Yellow
        }
    }
}

# Check AppleUSBVHCI service
$vhciService = Get-Service | Where-Object { $_.Name -like "*AppleUSBVHCI*" -or $_.Name -like "*USBVHCI*" }
if ($vhciService) {
    Write-Host "[OK] AppleUSBVHCI service found: $($vhciService.Name) ($($vhciService.Status))" -ForegroundColor Green
} else {
    Write-Host "[WARN] AppleUSBVHCI service not found" -ForegroundColor Yellow
    Write-Host "       BCE transport will fail, but driver will still load" -ForegroundColor Yellow
}

# Check free disk space
$systemDrive = Get-PSDrive -Name C
$freeSpaceMB = [math]::Round($systemDrive.Free / 1MB, 2)
if ($freeSpaceMB -lt 100) {
    Write-Host "[WARN] Low disk space: $freeSpaceMB MB free" -ForegroundColor Yellow
} else {
    Write-Host "[OK] Free disk space: $freeSpaceMB MB" -ForegroundColor Green
}

# Create rollback script
$rollbackScript = Join-Path $backupDir "rollback.ps1"
$rollbackContent = @"
# T2AudioPort Rollback Script
# Generated: $(Get-Date -Format "yyyy-MM-dd HH:mm:ss")

Write-Host "Rolling back to AppleAudio.sys..." -ForegroundColor Yellow

# Find latest backup
`$backups = Get-ChildItem -Path "$backupDir" -Filter "AppleAudio.sys.*.bak" | Sort-Object LastWriteTime -Descending
if (`$backups.Count -eq 0) {
    Write-Host "[FAIL] No backup found" -ForegroundColor Red
    exit 1
}

`$latestBackup = `$backups[0].FullName
Write-Host "[INFO] Restoring from: `$latestBackup" -ForegroundColor Gray

# Stop audio service
Stop-Service -Name audiosrv -Force -ErrorAction SilentlyContinue

# Restore driver
Copy-Item -LiteralPath `$latestBackup -Destination "C:\Windows\System32\drivers\AppleAudio.sys" -Force

# Restart audio service
Start-Service -Name audiosrv

Write-Host "[OK] Rollback complete" -ForegroundColor Green
Write-Host "     Reboot may be required" -ForegroundColor Yellow
"@
Set-Content -Path $rollbackScript -Value $rollbackContent -Force
Write-Host "[OK] Rollback script created: $rollbackScript" -ForegroundColor Green

Write-Host ""
Write-Host "==================================================================" -ForegroundColor Cyan
Write-Host "  Pre-Installation Check Complete" -ForegroundColor Cyan
Write-Host "==================================================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Yellow
Write-Host "  1. Review any warnings above" -ForegroundColor White
Write-Host "  2. Connect kernel debugger if enabled" -ForegroundColor White
Write-Host "  3. Run: .\Install-T2AudioDriver.ps1" -ForegroundColor White
Write-Host ""
Write-Host "If something goes wrong:" -ForegroundColor Yellow
Write-Host "  - Boot to Safe Mode" -ForegroundColor White
Write-Host "  - Run: $rollbackScript" -ForegroundColor White
Write-Host "  - Or boot to WinRE and restore backup manually" -ForegroundColor White
Write-Host ""
