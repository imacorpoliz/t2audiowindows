#Requires -RunAsAdministrator
<#
.SYNOPSIS
    T2 Audio Manager & Rollback Utility
    MacBook Pro 16" (2019, Apple T2) - Windows 10/11 x64
#>

param(
    [ValidateSet('Menu', 'RollbackToApple', 'SwitchToT2Audio', 'Preset', 'Doctor', 'SoundTest', 'MicTest', 'ResetVerifier', 'ResetGuard', 'Backup', 'Shortcut', 'Status')]
    [string]$Action = 'Menu',
    [string]$PresetName = 'macos',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

# Paths
$ScriptDir    = $PSScriptRoot
$ProjectRoot  = if (Test-Path (Join-Path $ScriptDir 'T2AudioPort')) { $ScriptDir } else { Split-Path -Parent $ScriptDir }
$T2PortDir    = Join-Path $ProjectRoot 'T2AudioPort'
$AppleDir     = Join-Path $ProjectRoot 'MBP-Audio-driver'
$AppleInf     = Join-Path $AppleDir 'appleaudio.inf'
$T2SetupScript= Join-Path $T2PortDir 'tools\T2Audio-Setup.ps1'
$BackupDir    = Join-Path $ProjectRoot 'backups'
$HwIdPattern  = 'VEN_106B&DEV_1803'

if (-not (Test-Path $BackupDir)) { New-Item -ItemType Directory -Force -Path $BackupDir | Out-Null }

function Write-Header($text) {
    Write-Host ''
    Write-Host ('=' * 66) -ForegroundColor Cyan
    Write-Host "   $text" -ForegroundColor Cyan
    Write-Host ('=' * 66) -ForegroundColor Cyan
}
function Write-Ok($m)   { Write-Host "   [OK] $m" -ForegroundColor Green }
function Write-Info($m) { Write-Host "   $m" -ForegroundColor Gray }
function Write-Warn($m) { Write-Host "   [WARN] $m" -ForegroundColor Yellow }
function Write-Fail($m) { Write-Host "   [ERROR] $m" -ForegroundColor Red }

function Test-Admin {
    return ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-T2AudioDevice {
    return Get-PnpDevice | Where-Object { $_.InstanceId -like "*$HwIdPattern*" } | Select-Object -First 1
}

function Get-CurrentDriverType {
    $dev = Get-T2AudioDevice
    if (-not $dev) { return 'None' }
    $svc = (Get-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)" -ErrorAction SilentlyContinue).Service
    if ($svc -like '*T2Audio*') { return 'T2AudioPort' }
    if ($svc -like '*AppleAudio*') { return 'AppleAudio' }
    return $svc
}

# -------------------------------------------------------------
# Snapshot & Backup
# -------------------------------------------------------------
function Save-Snapshot {
    $ts = (Get-Date).ToString('yyyyMMdd-HHmmss')
    $file = Join-Path $BackupDir "snapshot-$ts.json"
    $params = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\T2AudioMiniport\Parameters' -ErrorAction SilentlyContinue
    
    $data = [PSCustomObject]@{
        Timestamp   = $ts
        DriverType  = (Get-CurrentDriverType)
        Parameters  = $params
    }
    $data | ConvertTo-Json -Depth 5 | Out-File $file -Encoding utf8
    Write-Ok "Backup snapshot saved: $(Split-Path -Leaf $file)"
    return $file
}

# -------------------------------------------------------------
# 1-Click Rollback to AppleAudio
# -------------------------------------------------------------
function Invoke-RollbackToApple {
    Write-Header "Rollback to Apple Official Driver (AppleAudio.sys)"
    Save-Snapshot | Out-Null

    if (-not (Test-Path $AppleInf)) {
        Write-Fail "Original Apple driver package not found at: $AppleInf"
        return
    }

    Write-Info "Installing official AppleAudio driver package..."
    $null = pnputil /add-driver $AppleInf /install
    
    # Disable T2AudioMiniport production mode so it does not conflict
    Set-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Services\T2AudioMiniport\Parameters' -Name 'EnableProduction' -Value 0 -ErrorAction SilentlyContinue

    Start-Sleep -Seconds 2
    $null = pnputil /scan-devices
    Start-Sleep -Seconds 1

    $cur = Get-CurrentDriverType
    $dev = Get-T2AudioDevice
    if ($cur -eq 'AppleAudio' -and $dev.Status -eq 'OK') {
        Write-Ok "Device successfully restored to AppleAudio (Status: OK)."
    } else {
        Write-Warn "Driver switched. Current driver: $cur, Device Status: $($dev.Status). A reboot may be needed."
    }
}

# -------------------------------------------------------------
# 1-Click Switch / Restore to T2AudioPort
# -------------------------------------------------------------
function Invoke-SwitchToT2Audio {
    Write-Header "Switching to T2AudioPort 1.0.13.8 (Custom DSP + Headphone Jack)"
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
    if ($cur -eq 'T2AudioPort' -and $dev.Status -eq 'OK') {
        Write-Ok "T2AudioPort 1.0.13.8 is active, verified, and running (Status: OK)!"
    } else {
        Write-Warn "Current driver: $cur, Device Status: $($dev.Status)"
    }
}

# -------------------------------------------------------------
# Quick Preset Switcher
# -------------------------------------------------------------
function Set-SoundPreset([string]$name) {
    Write-Header "Applying Sound Preset: $name"
    if (Test-Path $T2SetupScript) {
        powershell -ExecutionPolicy Bypass -File $T2SetupScript -Action Preset -Preset $name
        Write-Ok "Sound preset '$name' applied successfully!"
    } else {
        Write-Fail "T2Audio-Setup.ps1 not found."
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
    $desktop = [Environment]::GetFolderPath('Desktop')
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
    $params = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\T2AudioMiniport\Parameters' -ErrorAction SilentlyContinue

    Write-Host ""
    Write-Host "   ====================================================" -ForegroundColor DarkCyan
    Write-Host "            T2 AUDIO CONTROL & STATUS MONITOR          " -ForegroundColor Cyan
    Write-Host "   ====================================================" -ForegroundColor DarkCyan
    Write-Host ("   Driver in use:      {0}" -f $drv) -ForegroundColor $(if ($drv -eq 'T2AudioPort') { 'Green' } else { 'Yellow' })
    Write-Host ("   Hardware Status:    {0}" -f ($dev.Status)) -ForegroundColor Green
    
    if ($drv -eq 'T2AudioPort') {
        $preset = if ($params.SpeakerDsp -eq 3) { 'macos' } elseif ($params.SpeakerDsp -eq 4) { 'bass/custom' } else { 'default' }
        $jack = if ($params.JackEvents -eq 1) { 'ENABLED (Auto-Switch)' } else { 'DISABLED' }
        $mic = if ($params.MicInput -eq 1) { 'ENABLED (3-Mic Array)' } else { 'DISABLED' }
        $strikes = if ($params.CrashGuardStrikes) { $params.CrashGuardStrikes } else { 0 }

        Write-Host ("   Active Preset:      {0}" -f $preset) -ForegroundColor White
        Write-Host ("   Headphone Auto-Jack:{0}" -f $jack) -ForegroundColor White
        Write-Host ("   Microphone Input:   {0}" -f $mic) -ForegroundColor White
        Write-Host ("   Crash Guard:        {0} strikes" -f $strikes) -ForegroundColor $(if ($strikes -eq 0) { 'Green' } else { 'Red' })
    }

    $verf = verifier /querysettings 2>&1 | Out-String
    $verfStatus = if ($verf -match 'T2AudioMiniport.sys') { 'ARMED for T2Audio' } else { 'Inactive' }
    Write-Host ("   Driver Verifier:    {0}" -f $verfStatus) -ForegroundColor $(if ($verfStatus -match 'ARMED') { 'Yellow' } else { 'Gray' })
    Write-Host "   ====================================================" -ForegroundColor DarkCyan
}

# -------------------------------------------------------------
# Interactive Menu
# -------------------------------------------------------------
function Show-MainMenu {
    Clear-Host
    Show-StatusSummary

    $menu = @'

   ROLLBACK & DRIVER SWITCHING:
     [1] Rollback to official AppleAudio.sys (Откат на драйвер Apple)
     [2] Switch / Restore T2AudioPort 1.0.13.8 (Включить T2AudioPort)
     [3] Save manual settings snapshot (Сохранить слепок настроек)

   SOUND PRESETS & TONE:
     [4] Preset 'macos' (Рекомендованный звук macOS: баланс и чистота)
     [5] Preset 'bass' (Глубокий плотный бас)
     [6] Preset 'bright' (Кристальные высокие частоты)
     [7] Preset 'movie' (Кинотеатральный объемный звук)
     [8] Preset 'voice' (Четкая речь для звонков и видео)
     [9] Preset 'night' (Ночной режим без громких всплесков)

   HARDWARE TESTS & DIAGNOSTICS:
     [T] Sound Test (Проверка стереодинамиков: левый / правый / оба)
     [M] Mic Test (Запись 5 сек речи и воспроизведение)
     [D] Doctor (Полная проверка работоспособности системы)

   SAFETY & RECOVERY:
     [V] Reset Driver Verifier (Снять верификатор Microsoft)
     [G] Reset Crash Guard (Сбросить счетчик сбоев)
     [K] Create Desktop Shortcut (Создать ярлык управления на рабочем столе)

     [0] Exit (Выход)

'@
    Write-Host $menu
    $choice = Read-Host "   Выберите действие (0-9, T, M, D, V, G, K)"
    switch ($choice.ToUpper()) {
        '1' { Invoke-RollbackToApple }
        '2' { Invoke-SwitchToT2Audio }
        '3' { Save-Snapshot }
        '4' { Set-SoundPreset 'macos' }
        '5' { Set-SoundPreset 'bass' }
        '6' { Set-SoundPreset 'bright' }
        '7' { Set-SoundPreset 'movie' }
        '8' { Set-SoundPreset 'voice' }
        '9' { Set-SoundPreset 'night' }
        'T' { Invoke-SoundTest }
        'M' { Invoke-MicTest }
        'D' { Invoke-Doctor }
        'V' { Reset-DriverVerifier }
        'G' { Reset-CrashGuard }
        'K' { New-ManagerShortcut }
        '0' { return }
        default { Write-Warn "Неизвестный выбор." }
    }
}

# -------------------------------------------------------------
# Dispatch
# -------------------------------------------------------------
if (-not (Test-Admin)) {
    Write-Fail "Запустите PowerShell от имени администратора!"
    exit 1
}

switch ($Action) {
    'Menu'           { Show-MainMenu }
    'RollbackToApple'{ Invoke-RollbackToApple }
    'SwitchToT2Audio'{ Invoke-SwitchToT2Audio }
    'Preset'         { Set-SoundPreset $PresetName }
    'Doctor'         { Invoke-Doctor }
    'SoundTest'      { Invoke-SoundTest }
    'MicTest'        { Invoke-MicTest }
    'ResetVerifier'  { Reset-DriverVerifier }
    'ResetGuard'     { Reset-CrashGuard }
    'Backup'         { Save-Snapshot }
    'Shortcut'       { New-ManagerShortcut }
    'Status'         { Show-StatusSummary }
}
