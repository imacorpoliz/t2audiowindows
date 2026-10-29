#Requires -RunAsAdministrator
<#
.SYNOPSIS
    T2 Audio Control Center & Rollback Utility
    MacBook Pro 16" (2019, Apple T2) - Windows 10/11 x64
#>

param(
    [ValidateSet("Menu", "RollbackToApple", "SwitchToT2Audio", "Preset", "Doctor", "SoundTest", "MicTest", "ResetVerifier", "ResetGuard", "Backup", "Restore", "Shortcut", "Status")]
    [string]$Action = "Menu",
    [string]$PresetName = "macos",
    [switch]$Force
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8
$ErrorActionPreference = "Stop"

# Paths
$ScriptDir    = $PSScriptRoot
$ProjectRoot  = if (Test-Path (Join-Path $ScriptDir "T2AudioPort")) { $ScriptDir } else { Split-Path -Parent $ScriptDir }
$T2PortDir    = Join-Path $ProjectRoot "T2AudioPort"
$AppleDir     = Join-Path $ProjectRoot "MBP-Audio-driver"
$AppleInf     = Join-Path $AppleDir "appleaudio.inf"
$T2SetupScript= Join-Path $T2PortDir "tools\T2Audio-Setup.ps1"
$BackupDir    = Join-Path $ProjectRoot "backups"
$HwIdPattern  = "VEN_106B&DEV_1803"

if (-not (Test-Path $BackupDir)) { New-Item -ItemType Directory -Force -Path $BackupDir | Out-Null }

function Write-Header($text) {
    Write-Host ""
    Write-Host ("=" * 66) -ForegroundColor Cyan
    Write-Host "   $text" -ForegroundColor Cyan
    Write-Host ("=" * 66) -ForegroundColor Cyan
}
function Write-Ok($m)   { Write-Host "   [OK] $m" -ForegroundColor Green }
function Write-Info($m) { Write-Host "   $m" -ForegroundColor Gray }
function Write-Warn($m) { Write-Host "   [WARN] $m" -ForegroundColor Yellow }
function Write-Fail($m) { Write-Host "   [ERROR] $m" -ForegroundColor Red }

function Pause-Menu {
    Write-Host ""
    $null = Read-Host "   Press Enter to return to menu..."
}

function Test-Admin {
    return ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-T2AudioDevice {
    return Get-PnpDevice | Where-Object { $_.InstanceId -like "*$HwIdPattern*" } | Select-Object -First 1
}

function Get-CurrentDriverType {
    $dev = Get-T2AudioDevice
    if (-not $dev) { return "None" }
    $svc = (Get-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)" -ErrorAction SilentlyContinue).Service
    if ($svc -like "*T2Audio*") { return "T2AudioPort" }
    if ($svc -like "*AppleAudio*") { return "AppleAudio" }
    return $svc
}

# Preset definitions for instant on-the-fly switching
$PresetDefinitions = [ordered]@{
    "macos"   = @{ Title = "Like macOS (Balanced & Clean)"; BassDb = 0;  TrebleDb = 0; ClarityDb = 0; DspGainDb = 6; TweeterTrimDb = 0; WooferLimitDb = 0; DspAutoGain = 0; CompStyle = 0; SpeakerDsp = 3 }
    "bass"    = @{ Title = "Deep Punchy Bass Boost";       BassDb = 6;  TrebleDb = 0; ClarityDb = 0; DspGainDb = 6; TweeterTrimDb = 0; WooferLimitDb = 2; DspAutoGain = 0; CompStyle = 1; SpeakerDsp = 4 }
    "bright"  = @{ Title = "Crystal Clear Treble & Air";   BassDb = 0;  TrebleDb = 5; ClarityDb = 3; DspGainDb = 6; TweeterTrimDb = 3; WooferLimitDb = 0; DspAutoGain = 0; CompStyle = 0; SpeakerDsp = 3 }
    "movie"   = @{ Title = "Cinematic Dynamic Spatial";    BassDb = 3;  TrebleDb = 3; ClarityDb = 3; DspGainDb = 6; TweeterTrimDb = 0; WooferLimitDb = 0; DspAutoGain = 1; CompStyle = 1; SpeakerDsp = 3 }
    "voice"   = @{ Title = "Speech Clarity for Calls";     BassDb = -4; TrebleDb = 2; ClarityDb = 5; DspGainDb = 6; TweeterTrimDb = 0; WooferLimitDb = 0; DspAutoGain = 1; CompStyle = 0; SpeakerDsp = 3 }
    "night"   = @{ Title = "Night Mode (Compressed Range)";BassDb = -5; TrebleDb = 0; ClarityDb = 2; DspGainDb = 3; TweeterTrimDb = 0; WooferLimitDb = 6; DspAutoGain = 1; CompStyle = 1; SpeakerDsp = 4 }
}

# -------------------------------------------------------------
# Quick Preset Switcher (Instant On-The-Fly)
# -------------------------------------------------------------
function Set-SoundPreset([string]$name) {
    Write-Header "Applying Sound Preset: $name"
    $key = $name.ToLower()
    $p = $PresetDefinitions[$key]
    if (-not $p) {
        Write-Fail "Unknown preset: $name"
        return
    }

    $paramPath = "HKLM:\SYSTEM\CurrentControlSet\Services\T2AudioMiniport\Parameters"
    if (-not (Test-Path $paramPath)) {
        New-Item -Path $paramPath -Force | Out-Null
    }

    foreach ($k in $p.Keys) {
        if ($k -ne "Title") {
            Set-ItemProperty -Path $paramPath -Name $k -Value $p[$k] -Type DWord -Force
        }
    }

    Write-Ok "Preset '$name' ($($p.Title)) applied in real-time!"
    Write-Info "Active tuning: Bass=$($p.BassDb) dB | Treble=$($p.TrebleDb) dB | Clarity=$($p.ClarityDb) dB | Gain=$($p.DspGainDb) dB"
    Write-Info "Driver live DSP updates parameters within 500 ms without stopping audio."
}

# -------------------------------------------------------------
# Snapshot & Backup / Restore
# -------------------------------------------------------------
function Save-Snapshot {
    $ts = (Get-Date).ToString("yyyyMMdd-HHmmss")
    $file = Join-Path $BackupDir "snapshot-$ts.json"
    $paramDict = [ordered]@{}
    $reg = Get-Item "HKLM:\SYSTEM\CurrentControlSet\Services\T2AudioMiniport\Parameters" -ErrorAction SilentlyContinue
    if ($reg) {
        foreach ($v in $reg.GetValueNames()) {
            $paramDict[$v] = $reg.GetValue($v)
        }
    }
    
    $data = [PSCustomObject]@{
        Timestamp   = $ts
        DriverType  = (Get-CurrentDriverType)
        Parameters  = $paramDict
    }
    $data | ConvertTo-Json | Out-File $file -Encoding utf8
    Write-Ok "Backup snapshot saved: $(Split-Path -Leaf $file)"
    return $file
}

function Invoke-RestoreSnapshot {
    Write-Header "Restore Audio Settings from Snapshot"
    $snaps = Get-ChildItem -Path $BackupDir -Filter "snapshot-*.json" | Sort-Object LastWriteTime -Descending
    if (-not $snaps -or $snaps.Count -eq 0) {
        Write-Warn "No snapshots found in $BackupDir."
        return
    }

    Write-Host ""
    for ($i = 0; $i -lt $snaps.Count; $i++) {
        $s = $snaps[$i]
        $content = Get-Content $s.FullName -Raw | ConvertFrom-Json -ErrorAction SilentlyContinue
        $drv = if ($content.DriverType) { $content.DriverType } else { "Unknown" }
        Write-Host ("   [{0}] {1} (Driver: {2}, Date: {3})" -f ($i + 1), $s.Name, $drv, $s.LastWriteTime.ToString("yyyy-MM-dd HH:mm:ss")) -ForegroundColor White
    }
    Write-Host "   [0] Cancel" -ForegroundColor Gray
    Write-Host ""
    $pick = Read-Host "   Select snapshot to restore (1-$($snaps.Count), 0)"
    if ($pick -eq "0" -or -not ($pick -match "^\d+$") -or [int]$pick -gt $snaps.Count) {
        Write-Info "Restore cancelled."
        return
    }

    $targetSnap = $snaps[[int]$pick - 1]
    Write-Info "Restoring: $($targetSnap.Name)..."
    $data = Get-Content $targetSnap.FullName -Raw | ConvertFrom-Json
    
    if ($data.Parameters) {
        $paramPath = "HKLM:\SYSTEM\CurrentControlSet\Services\T2AudioMiniport\Parameters"
        if (-not (Test-Path $paramPath)) {
            New-Item -Path $paramPath -Force | Out-Null
        }
        foreach ($prop in $data.Parameters.PSObject.Properties) {
            if ($prop.Name -notmatch "^PS") {
                Set-ItemProperty -Path $paramPath -Name $prop.Name -Value $prop.Value -Force -ErrorAction SilentlyContinue
            }
        }
        Write-Ok "Registry parameters restored."
    }

    Write-Ok "Snapshot $($targetSnap.Name) applied successfully!"
}

# -------------------------------------------------------------
# 1-Click Rollback to AppleAudio (Guaranteed Clean PnP Switch)
# -------------------------------------------------------------
function Invoke-RollbackToApple {
    Write-Header "Rollback to Apple Official Driver (AppleAudio.sys)"
    Save-Snapshot | Out-Null

    Write-Info "1. Disarming T2AudioMiniport production mode..."
    Set-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Services\T2AudioMiniport\Parameters" -Name "EnableProduction" -Value 0 -ErrorAction SilentlyContinue

    Write-Info "2. Cleaning T2AudioMiniport driver store packages..."
    $infs = Get-ChildItem -Path "$env:WINDIR\INF\oem*.inf" -File -ErrorAction SilentlyContinue | Where-Object {
        Select-String -LiteralPath $_.FullName -Pattern "T2AudioMiniport" -SimpleMatch -Quiet
    }
    if ($infs) {
        foreach ($inf in $infs) {
            Write-Info "   Deleting $($inf.Name)..."
            $null = pnputil /delete-driver $inf.Name /uninstall /force
        }
        Write-Ok "T2AudioMiniport OEM packages removed from driver store."
    } else {
        Write-Info "   No conflicting T2AudioMiniport packages found."
    }

    if (Test-Path $AppleInf) {
        Write-Info "3. Staging and installing Apple official driver package..."
        $null = pnputil /add-driver $AppleInf /install
    } else {
        Write-Warn "   AppleAudio INF not found at $AppleInf; rescanning existing driver store."
    }

    Write-Info "4. Rescanning Plug and Play devices..."
    $null = pnputil /scan-devices

    Write-Info "5. Verifying active driver..."
    $timeout = 15
    $switched = $false
    for ($i = 0; $i -lt $timeout; $i++) {
        Start-Sleep -Seconds 1
        $cur = Get-CurrentDriverType
        $dev = Get-T2AudioDevice
        if ($cur -eq "AppleAudio" -and $dev.Status -eq "OK") {
            $switched = $true
            break
        }
    }

    $cur = Get-CurrentDriverType
    $dev = Get-T2AudioDevice
    if ($switched -or $cur -eq "AppleAudio") {
        Write-Ok "Device successfully rolled back to official AppleAudio (Status: $($dev.Status))!"
    } else {
        Write-Warn "Current driver: $cur, Device Status: $($dev.Status). A system reboot may be required to finish switching."
    }
}

# -------------------------------------------------------------
# 1-Click Switch / Restore to T2AudioPort
# -------------------------------------------------------------
function Invoke-SwitchToT2Audio {
    Write-Header "Switching to T2AudioPort (Custom DSP + Headphone Jack + Mic)"
    Save-Snapshot | Out-Null

    if (-not (Test-Path $T2SetupScript)) {
        Write-Fail "T2Audio-Setup.ps1 not found at: $T2SetupScript"
        return
    }

    Write-Info "Installing T2AudioPort driver and activating features..."
    powershell -ExecutionPolicy Bypass -File $T2SetupScript -Action Install -Preset $PresetName -Force
    powershell -ExecutionPolicy Bypass -File $T2SetupScript -Action Features -Headphones 1 -Microphone 1 -ResumeAfterSleep 1

    Start-Sleep -Seconds 1
    $cur = Get-CurrentDriverType
    $dev = Get-T2AudioDevice
    if ($cur -eq "T2AudioPort" -and $dev.Status -eq "OK") {
        Write-Ok "T2AudioPort is active, verified, and running (Status: OK)!"
    } else {
        Write-Warn "Current driver: $cur, Device Status: $($dev.Status)"
    }
}

# -------------------------------------------------------------
# Diagnostics & Verifier
# -------------------------------------------------------------
function Invoke-SoundTest {
    if (Test-Path $T2SetupScript) {
        powershell -ExecutionPolicy Bypass -File $T2SetupScript -Action SoundTest
    }
}

function Invoke-MicTest {
    if (Test-Path $T2SetupScript) {
        powershell -ExecutionPolicy Bypass -File $T2SetupScript -Action MicTest
    }
}

function Invoke-Doctor {
    if (Test-Path $T2SetupScript) {
        powershell -ExecutionPolicy Bypass -File $T2SetupScript -Action Doctor
    }
}

function Reset-DriverVerifier {
    Write-Header "Resetting Microsoft Driver Verifier"
    $out = verifier /reset 2>&1 | Out-String
    Write-Ok "Driver Verifier reset complete. Changes take effect on next reboot."
    Write-Info ($out.Trim())
}

function Reset-CrashGuard {
    Write-Header "Resetting Crash Guard Watchdog"
    if (Test-Path $T2SetupScript) {
        powershell -ExecutionPolicy Bypass -File $T2SetupScript -Action ResetGuard
    }
}

# -------------------------------------------------------------
# Desktop Shortcut
# -------------------------------------------------------------
function New-ManagerShortcut {
    $desktop = [Environment]::GetFolderPath("Desktop")
    $linkPath = Join-Path $desktop "T2 Audio Control Center.lnk"
    $ws = New-Object -ComObject WScript.Shell
    $sc = $ws.CreateShortcut($linkPath)
    $sc.TargetPath = "powershell.exe"
    $sc.Arguments = "-NoExit -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    $sc.WorkingDirectory = $ProjectRoot
    $sc.Description = "T2 Audio Control Center & Rollback Manager"
    $sc.Save()
    Write-Ok "Desktop shortcut created: $linkPath"
}

# -------------------------------------------------------------
# Status Display
# -------------------------------------------------------------
function Show-StatusSummary {
    $dev = Get-T2AudioDevice
    $drv = Get-CurrentDriverType
    $params = Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Services\T2AudioMiniport\Parameters" -ErrorAction SilentlyContinue

    Write-Host ""
    Write-Host "   ====================================================" -ForegroundColor DarkCyan
    Write-Host "            T2 AUDIO CONTROL & STATUS MONITOR          " -ForegroundColor Cyan
    Write-Host "   ====================================================" -ForegroundColor DarkCyan
    Write-Host ("   Driver in use:      {0}" -f $drv) -ForegroundColor $(if ($drv -eq "T2AudioPort") { "Green" } else { "Yellow" })
    Write-Host ("   Hardware Status:    {0}" -f ($dev.Status)) -ForegroundColor Green
    
    if ($drv -eq "T2AudioPort") {
        $preset = if ($params.BassDb -eq 6) { "bass" } `
                  elseif ($params.TrebleDb -eq 5) { "bright" } `
                  elseif ($params.BassDb -eq 3 -and $params.TrebleDb -eq 3) { "movie" } `
                  elseif ($params.ClarityDb -eq 5) { "voice" } `
                  elseif ($params.WooferLimitDb -eq 6 -and $params.BassDb -eq -5) { "night" } `
                  else { "macos (standard)" }

        $bass = if ($null -ne $params.BassDb) { "$($params.BassDb) dB" } else { "0 dB" }
        $treble = if ($null -ne $params.TrebleDb) { "$($params.TrebleDb) dB" } else { "0 dB" }
        $clarity = if ($null -ne $params.ClarityDb) { "$($params.ClarityDb) dB" } else { "0 dB" }
        $gain = if ($null -ne $params.DspGainDb) { "$($params.DspGainDb) dB" } else { "6 dB" }

        $jack = if ($params.JackEvents -eq 1) { "ENABLED (Auto-Switch)" } else { "DISABLED" }
        $mic = if ($params.MicInput -eq 1) { "ENABLED (3-Mic Array)" } else { "DISABLED" }
        $strikes = if ($params.CrashGuardStrikes) { $params.CrashGuardStrikes } else { 0 }

        Write-Host ("   Active Preset:      {0}" -f $preset) -ForegroundColor White
        Write-Host ("   Tone Tuning:        Bass {0} | Treble {1} | Clarity {2} | Gain {3}" -f $bass, $treble, $clarity, $gain) -ForegroundColor Cyan
        Write-Host ("   Headphone Auto-Jack:{0}" -f $jack) -ForegroundColor White
        Write-Host ("   Microphone Input:   {0}" -f $mic) -ForegroundColor White
        Write-Host ("   Crash Guard:        {0} strikes" -f $strikes) -ForegroundColor $(if ($strikes -eq 0) { "Green" } else { "Red" })
    }

    $verf = verifier /querysettings 2>&1 | Out-String
    $verfStatus = if ($verf -match "T2AudioMiniport.sys") { "ARMED for T2Audio" } else { "Inactive" }
    Write-Host ("   Driver Verifier:    {0}" -f $verfStatus) -ForegroundColor $(if ($verfStatus -match "ARMED") { "Yellow" } else { "Gray" })
    Write-Host "   ====================================================" -ForegroundColor DarkCyan
}

# -------------------------------------------------------------
# Interactive Menu (Continuous Loop)
# -------------------------------------------------------------
function Show-MainMenu {
    while ($true) {
        Clear-Host
        Show-StatusSummary

        $menu = @"

   ROLLBACK & DRIVER SWITCHING:
     [1] Rollback to official AppleAudio.sys
     [2] Switch / Restore T2AudioPort (Custom DSP + Headphone Jack + Mic)
     [3] Save manual settings snapshot
     [R] Restore settings snapshot

   SOUND PRESETS (Instant On-The-Fly Switching):
     [4] Preset 'macos'  (Balanced & clean Apple tone)
     [5] Preset 'bass'   (Deep punchy bass boost)
     [6] Preset 'bright' (Crystal clear treble & air)
     [7] Preset 'movie'  (Cinematic dynamic audio)
     [8] Preset 'voice'  (Optimized speech clarity for calls & video)
     [9] Preset 'night'  (Night mode: compressed dynamic range)

   HARDWARE TESTS & DIAGNOSTICS:
     [T] Sound Test (Test stereo speakers: Left / Right / Both)
     [M] Mic Test (Record 5s audio and playback test)
     [D] Doctor (Comprehensive system & driver diagnostic)

   SAFETY & RECOVERY:
     [V] Reset Driver Verifier (Remove Microsoft Driver Verifier)
     [G] Reset Crash Guard (Reset watchdog failure counter)
     [K] Create Desktop Shortcut (Place shortcut on Desktop)

     [0] Exit

"@
        Write-Host $menu
        $choice = Read-Host "   Select action (0-9, R, T, M, D, V, G, K)"
        if ($choice -eq "0") {
            Write-Info "Exiting T2 Audio Control Center. Goodbye!"
            break
        }
        switch ($choice.ToUpper()) {
            "1" { Invoke-RollbackToApple; Pause-Menu }
            "2" { Invoke-SwitchToT2Audio; Pause-Menu }
            "3" { Save-Snapshot; Pause-Menu }
            "R" { Invoke-RestoreSnapshot; Pause-Menu }
            "4" { Set-SoundPreset "macos"; Start-Sleep -Milliseconds 700 }
            "5" { Set-SoundPreset "bass"; Start-Sleep -Milliseconds 700 }
            "6" { Set-SoundPreset "bright"; Start-Sleep -Milliseconds 700 }
            "7" { Set-SoundPreset "movie"; Start-Sleep -Milliseconds 700 }
            "8" { Set-SoundPreset "voice"; Start-Sleep -Milliseconds 700 }
            "9" { Set-SoundPreset "night"; Start-Sleep -Milliseconds 700 }
            "T" { Invoke-SoundTest; Pause-Menu }
            "M" { Invoke-MicTest; Pause-Menu }
            "D" { Invoke-Doctor; Pause-Menu }
            "V" { Reset-DriverVerifier; Pause-Menu }
            "G" { Reset-CrashGuard; Pause-Menu }
            "K" { New-ManagerShortcut; Pause-Menu }
            default {
                Write-Warn "Unknown option: '$choice'"
                Start-Sleep -Seconds 1
            }
        }
    }
}

# -------------------------------------------------------------
# Dispatch
# -------------------------------------------------------------
if (-not (Test-Admin)) {
    Write-Fail "Please run PowerShell as Administrator!"
    exit 1
}

switch ($Action) {
    "Menu"           { Show-MainMenu }
    "RollbackToApple"{ Invoke-RollbackToApple }
    "SwitchToT2Audio"{ Invoke-SwitchToT2Audio }
    "Preset"         { Set-SoundPreset $PresetName }
    "Doctor"         { Invoke-Doctor }
    "SoundTest"      { Invoke-SoundTest }
    "MicTest"        { Invoke-MicTest }
    "ResetVerifier"  { Reset-DriverVerifier }
    "ResetGuard"     { Reset-CrashGuard }
    "Backup"         { Save-Snapshot }
    "Restore"        { Invoke-RestoreSnapshot }
    "Shortcut"       { New-ManagerShortcut }
    "Status"         { Show-StatusSummary }
}
