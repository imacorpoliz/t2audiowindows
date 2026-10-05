# T2AudioPort Emergency Rollback Script
# Use this if the driver causes problems after installation
# Can be run from Safe Mode or normal boot

param(
    [switch]$Force = $false
)

$ErrorActionPreference = "Stop"

Write-Host "==================================================================" -ForegroundColor Red
Write-Host "  T2AudioPort Emergency Rollback" -ForegroundColor Red
Write-Host "==================================================================" -ForegroundColor Red
Write-Host ""

# Check admin rights
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "[FAIL] Administrator privileges required" -ForegroundColor Red
    exit 1
}
Write-Host "[OK] Running as Administrator" -ForegroundColor Green

# Find backup directory
$backupDir = "C:\Users\$env:USERNAME\Desktop\mbp\T2AudioPort\Backup"
if (-not (Test-Path -LiteralPath $backupDir)) {
    Write-Host "[ERROR] Backup directory not found: $backupDir" -ForegroundColor Red
    Write-Host "        Cannot proceed without backup" -ForegroundColor Red
    exit 1
}
Write-Host "[OK] Backup directory found: $backupDir" -ForegroundColor Green

# Find latest backup
$backups = Get-ChildItem -Path $backupDir -Filter "AppleAudio.sys.*.bak" -ErrorAction SilentlyContinue | 
    Sort-Object LastWriteTime -Descending

if ($backups.Count -eq 0) {
    Write-Host "[ERROR] No backup files found in $backupDir" -ForegroundColor Red
    Write-Host "        Expected: AppleAudio.sys.*.bak" -ForegroundColor Red
    exit 1
}

$latestBackup = $backups[0]
Write-Host "[OK] Latest backup found:" -ForegroundColor Green
Write-Host "     File: $($latestBackup.FullName)" -ForegroundColor Gray
Write-Host "     Date: $($latestBackup.LastWriteTime)" -ForegroundColor Gray
Write-Host "     Size: $($latestBackup.Length) bytes" -ForegroundColor Gray

Write-Host ""
Write-Host "This script will:" -ForegroundColor Yellow
Write-Host "  1. Stop audio services" -ForegroundColor White
Write-Host "  2. Uninstall T2AudioMiniport driver package" -ForegroundColor White
Write-Host "  3. Restore AppleAudio.sys from backup" -ForegroundColor White
Write-Host "  4. Re-enable AppleAudio service" -ForegroundColor White
Write-Host "  5. Restart audio services" -ForegroundColor White
Write-Host ""

if (-not $Force) {
    $confirm = Read-Host "Type 'ROLLBACK' to continue"
    if ($confirm -ne "ROLLBACK") {
        Write-Host "[ABORT] Rollback cancelled" -ForegroundColor Yellow
        exit 0
    }
}

Write-Host ""
Write-Host "Starting rollback..." -ForegroundColor Cyan
Write-Host ""

# Step 1: Stop audio services
Write-Host "[1/5] Stopping audio services..." -ForegroundColor Cyan
try {
    Stop-Service -Name audiosrv -Force -ErrorAction Stop
    Write-Host "      Audio service stopped" -ForegroundColor Green
} catch {
    Write-Host "[WARN] Could not stop audio service: $_" -ForegroundColor Yellow
}

# Also try to stop any T2AudioMiniport service
$t2Service = Get-Service | Where-Object { $_.Name -like "*T2Audio*" }
if ($t2Service) {
    try {
        Stop-Service -Name $t2Service.Name -Force -ErrorAction Stop
        Write-Host "      T2AudioMiniport service stopped: $($t2Service.Name)" -ForegroundColor Green
    } catch {
        Write-Host "[WARN] Could not stop T2AudioMiniport service: $_" -ForegroundColor Yellow
    }
}

# Step 2: Uninstall T2AudioMiniport driver package
Write-Host "[2/5] Uninstalling T2AudioMiniport driver package..." -ForegroundColor Cyan
try {
    # Find installed package
    $installedDrivers = pnputil.exe /enum-drivers 2>&1 | Select-String "T2Audio" -Context 2
    if ($installedDrivers) {
        Write-Host "      Found installed driver packages:" -ForegroundColor Gray
        Write-Host "      $installedDrivers" -ForegroundColor Gray
        
        # Extract OEM*.inf names
        $oemInfs = $installedDrivers | ForEach-Object {
            if ($_ -match "oem(\d+)\.inf") {
                "oem$($Matches[1]).inf"
            }
        } | Select-Object -Unique
        
        foreach ($oemInf in $oemInfs) {
            Write-Host "      Removing: $oemInf" -ForegroundColor Gray
            pnputil.exe /delete-driver $oemInf /uninstall /force 2>&1 | Out-Null
            if ($LASTEXITCODE -eq 0) {
                Write-Host "      Removed: $oemInf" -ForegroundColor Green
            } else {
                Write-Host "[WARN] Failed to remove $oemInf (error code: $LASTEXITCODE)" -ForegroundColor Yellow
            }
        }
    } else {
        Write-Host "      No T2AudioMiniport packages found (may already be removed)" -ForegroundColor Gray
    }
} catch {
    Write-Host "[WARN] Error during driver uninstall: $_" -ForegroundColor Yellow
}

# Step 3: Restore AppleAudio.sys from backup
Write-Host "[3/5] Restoring AppleAudio.sys from backup..." -ForegroundColor Cyan
$appleAudioPath = "C:\Windows\System32\drivers\AppleAudio.sys"

# Backup current file if it exists
if (Test-Path -LiteralPath $appleAudioPath) {
    $currentBackup = "$appleAudioPath.before_rollback"
    try {
        Copy-Item -LiteralPath $appleAudioPath -Destination $currentBackup -Force
        Write-Host "      Current driver backed up to: $currentBackup" -ForegroundColor Gray
    } catch {
        Write-Host "[WARN] Could not backup current driver: $_" -ForegroundColor Yellow
    }
}

# Restore from backup
try {
    Copy-Item -LiteralPath $latestBackup.FullName -Destination $appleAudioPath -Force
    Write-Host "      AppleAudio.sys restored" -ForegroundColor Green
    
    # Verify restore
    $restoredSize = (Get-Item -LiteralPath $appleAudioPath).Length
    if ($restoredSize -eq $latestBackup.Length) {
        Write-Host "      Verification: size match ($restoredSize bytes)" -ForegroundColor Green
    } else {
        Write-Host "[WARN] Size mismatch: backup=$($latestBackup.Length), restored=$restoredSize" -ForegroundColor Yellow
    }
} catch {
    Write-Host "[ERROR] Failed to restore AppleAudio.sys: $_" -ForegroundColor Red
    Write-Host "        Manual recovery required!" -ForegroundColor Red
    exit 1
}

# Step 4: Re-enable AppleAudio service
Write-Host "[4/5] Re-enabling AppleAudio service..." -ForegroundColor Cyan
$appleAudioService = Get-Service | Where-Object { $_.Name -like "*AppleAudio*" }
if ($appleAudioService) {
    try {
        sc.exe config $appleAudioService.Name start= auto | Out-Null
        Write-Host "      AppleAudio service re-enabled: $($appleAudioService.Name)" -ForegroundColor Green
    } catch {
        Write-Host "[WARN] Could not re-enable AppleAudio service: $_" -ForegroundColor Yellow
    }
} else {
    Write-Host "      AppleAudio service not found (may need manual setup)" -ForegroundColor Gray
}

# Step 5: Restart audio services
Write-Host "[5/5] Restarting audio services..." -ForegroundColor Cyan
try {
    Start-Service -Name audiosrv -ErrorAction Stop
    Write-Host "      Audio service started" -ForegroundColor Green
} catch {
    Write-Host "[WARN] Could not start audio service: $_" -ForegroundColor Yellow
    Write-Host "       Service may start automatically on reboot" -ForegroundColor Gray
}

if ($appleAudioService) {
    try {
        Start-Service -Name $appleAudioService.Name -ErrorAction Stop
        Write-Host "      AppleAudio service started" -ForegroundColor Green
    } catch {
        Write-Host "[WARN] Could not start AppleAudio service: $_" -ForegroundColor Yellow
    }
}

# Delete T2AudioMiniport.sys if still present
Write-Host ""
Write-Host "Cleaning up T2AudioMiniport files..." -ForegroundColor Cyan
$t2DriverPath = "C:\Windows\System32\drivers\T2AudioMiniport.sys"
if (Test-Path -LiteralPath $t2DriverPath) {
    try {
        Remove-Item -LiteralPath $t2DriverPath -Force
        Write-Host "      Removed: $t2DriverPath" -ForegroundColor Green
    } catch {
        Write-Host "[WARN] Could not remove T2AudioMiniport.sys: $_" -ForegroundColor Yellow
        Write-Host "       File may be locked, will be removed on reboot" -ForegroundColor Gray
    }
}

Write-Host ""
Write-Host "==================================================================" -ForegroundColor Green
Write-Host "  Rollback Complete" -ForegroundColor Green
Write-Host "==================================================================" -ForegroundColor Green
Write-Host ""

Write-Host "Next steps:" -ForegroundColor Yellow
Write-Host "  1. Reboot system to complete restoration" -ForegroundColor White
Write-Host "  2. After reboot, verify audio is working" -ForegroundColor White
Write-Host "  3. Check Device Manager for 'Apple Audio Device'" -ForegroundColor White
Write-Host ""

$rebootNow = Read-Host "Reboot now? (Y/n)"
if ($rebootNow -ne "n") {
    Write-Host "Rebooting in 10 seconds..." -ForegroundColor Cyan
    shutdown.exe /r /t 10 /c "T2AudioPort rollback - rebooting"
} else {
    Write-Host "[INFO] Reboot cancelled. Remember to reboot manually." -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Rollback script finished." -ForegroundColor Cyan
Write-Host ""
