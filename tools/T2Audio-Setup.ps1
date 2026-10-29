# T2AudioPort - installer and sound settings (MacBookPro16,1).
#
# Menu (run without parameters) or parameters:
#   .\T2Audio-Setup.ps1                                  # interactive menu
#   .\T2Audio-Setup.ps1 -Action Install -Preset macos    # install, persistent mode
#   .\T2Audio-Setup.ps1 -Action Preset  -Preset bright   # change the sound preset
#   .\T2Audio-Setup.ps1 -Action Tune -BassDb 3 -ClarityDb 2  # tone on top of the preset
#   .\T2Audio-Setup.ps1 -Action Balance -Volume 0.3      # even out the L/R balance
#   .\T2Audio-Setup.ps1 -Action Features -Headphones 1 -Microphone 1   # optional features
#   .\T2Audio-Setup.ps1 -Action UpdateGuard -Block 1     # keep Windows Update from restoring AppleAudio
#   .\T2Audio-Setup.ps1 -Action Status
#   .\T2Audio-Setup.ps1 -Action ResetGuard               # reset the crash guard
#   .\T2Audio-Setup.ps1 -Action Uninstall                # go back to AppleAudio
#
# Persistent mode: the driver starts on every Windows boot
# (Parameters\EnableProduction=1) under a crash guard: if two sessions in a
# row end without a clean shutdown (BSOD, hang, power button), the driver
# stops starting until this script resets the guard. It never starts in
# Safe Mode.
#
# Needs administrator rights, testsigning on and a signed package in
# packaging\ (T2AudioMiniport.inf/.sys/.cat), built as in GEMINI.md section 2.
# Saved as UTF-8 with BOM (Windows PowerShell 5.1 needs it for the symbols).

[CmdletBinding()]
param(
    [ValidateSet('Menu', 'Install', 'Preset', 'Tune', 'Balance', 'Features', 'UpdateGuard', 'Status', 'ResetGuard', 'Uninstall')]
    [string]$Action = 'Menu',

    [ValidateSet('macos', 'macos-precise', 'bright', 'loud', 'maxvolume', 'even', 'bass', 'voice', 'movie', 'night', 'warm', 'natural', 'raw')]
    [string]$Preset = 'macos',

    # -Action Tune: tone in dB (-12..+12) on top of the current preset; omitted = unchanged.
    # Bass = shelf at 150 Hz, treble = shelf at 6 kHz, clarity = peak at 2.5 kHz.
    [ValidateRange(-12, 12)] [Nullable[int]]$BassDb,
    [ValidateRange(-12, 12)] [Nullable[int]]$TrebleDb,
    [ValidateRange(-12, 12)] [Nullable[int]]$ClarityDb,
    # Compressor: 0 = fast peak (default), 1 = like macOS (RMS, soft knee).
    [ValidateRange(0, 1)] [Nullable[int]]$CompStyle,

    # -Action Features: optional features (1 = on, 0 = off; omitted = unchanged).
    [ValidateRange(0, 1)] [Nullable[int]]$Headphones,      # headphone output (+ jack tracking)
    [ValidateRange(0, 1)] [Nullable[int]]$Microphone,      # built-in microphones
    [ValidateRange(0, 1)] [Nullable[int]]$ResumeAfterSleep,# sound comes back after sleep by itself
    [ValidateRange(0, 1)] [Nullable[int]]$ExtraFormats,    # 16/24-bit for exclusive-mode apps

    # -Action UpdateGuard: 1 = stop Windows Update from installing drivers
    # (so it cannot bring AppleAudio back), 0 = restore the previous settings.
    [ValidateRange(0, 1)] [Nullable[int]]$Block,

    # T2 speaker volume after install (0..1); -1 = leave unchanged.
    [double]$Volume = 0.3,

    # No confirmation prompts.
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

$HardwareId   = 'PCI\VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01'
$ServiceName  = 'T2AudioMiniport'
$ParamsPath   = "HKLM:\SYSTEM\CurrentControlSet\Services\$ServiceName\Parameters"
$TestSession  = "HKLM:\SYSTEM\CurrentControlSet\Services\$ServiceName\TestSession"
$ProjectRoot  = Split-Path -Parent $PSScriptRoot
$PackageDir   = Join-Path $ProjectRoot 'packaging'
$InfPath      = Join-Path $PackageDir 'T2AudioMiniport.inf'
$SysPath      = Join-Path $PackageDir 'T2AudioMiniport.sys'
$CatPath      = Join-Path $PackageDir 't2audiominiport.cat'

# ---------------------------------------------------------------------------
# Sound presets. Values are DWORDs in Parameters; a missing value = the
# driver default. Each preset lists only its differences from the defaults
# (= 'macos'); installing writes all values. Only 'macos' is approved by the
# owner by ear (2026-10-28); the others are starting points for listening.
# BassDb/TrebleDb/ClarityDb/CompStyle need driver 1.0.12.0 or later.
# ---------------------------------------------------------------------------
$PresetValueNames = @('SpeakerDsp', 'DspGainDb', 'TweeterTrimDb', 'WooferLimitDb', 'DspAutoGain',
                      'BassDb', 'TrebleDb', 'ClarityDb', 'CompStyle')
# Driver defaults (used when the registry has no value).
$ParamDefaults = @{ SpeakerDsp = 3; DspGainDb = 6; TweeterTrimDb = 0; WooferLimitDb = 0; DspAutoGain = 0;
                    BassDb = 0; TrebleDb = 0; ClarityDb = 0; CompStyle = 0 }

function New-PresetValues([hashtable]$changes) {
    $v = $ParamDefaults.Clone()
    foreach ($k in $changes.Keys) { $v[$k] = $changes[$k] }
    return $v
}

$Presets = [ordered]@{
    'macos'         = @{ Title = 'Like macOS (recommended)';
                         Text  = 'Apple EQ for the 6 speakers, virtual bass, multiband compressor.';
                         Values = (New-PresetValues @{ }); Verified = $true }
    'macos-precise' = @{ Title = 'Like macOS, precise compressor';
                         Text  = 'Same, but the compressor measures like macOS (RMS, soft knee).';
                         Values = (New-PresetValues @{ CompStyle = 1 }); Verified = $false }
    'bright'        = @{ Title = 'Brighter';
                         Text  = 'Like macOS with the tweeters 3 dB louder: more air and detail.';
                         Values = (New-PresetValues @{ TweeterTrimDb = 3 }); Verified = $false }
    'loud'          = @{ Title = 'Louder';
                         Text  = 'More make-up gain and woofer rattle protection; the limiter holds the peaks.';
                         Values = (New-PresetValues @{ SpeakerDsp = 4; DspGainDb = 9; WooferLimitDb = 3 }); Verified = $false }
    'maxvolume'     = @{ Title = 'Maximum volume without hum';
                         Text  = 'For loud music with bass: woofers limited 6 dB lower, rattle protection.';
                         Values = (New-PresetValues @{ SpeakerDsp = 4; WooferLimitDb = 6 }); Verified = $false }
    'even'          = @{ Title = 'Even loudness';
                         Text  = 'Like macOS plus slow loudness autogain (LUFS): quiet tracks up to +6 dB, loud ones down to -12 dB.';
                         Values = (New-PresetValues @{ DspAutoGain = 1 }); Verified = $false }
    'bass'          = @{ Title = 'More bass';
                         Text  = 'Bass +4 dB (150 Hz shelf) with rattle protection; may rattle on kicks at high volume.';
                         Values = (New-PresetValues @{ SpeakerDsp = 4; WooferLimitDb = 2; BassDb = 4; CompStyle = 1 }); Verified = $false }
    'voice'         = @{ Title = 'Voice (podcasts, calls)';
                         Text  = 'Clarity +4 dB (2.5 kHz), bass -3 dB, even loudness.';
                         Values = (New-PresetValues @{ BassDb = -3; ClarityDb = 4; DspAutoGain = 1 }); Verified = $false }
    'movie'         = @{ Title = 'Movie';
                         Text  = 'Even loudness, dialogue +2 dB, bass +2 dB, precise compressor.';
                         Values = (New-PresetValues @{ BassDb = 2; ClarityDb = 2; DspAutoGain = 1; CompStyle = 1 }); Verified = $false }
    'night'         = @{ Title = 'Night (quiet and even)';
                         Text  = 'Less bass and fewer peaks, even loudness; speech stays clear.';
                         Values = (New-PresetValues @{ SpeakerDsp = 4; DspGainDb = 3; WooferLimitDb = 6; DspAutoGain = 1; BassDb = -4; ClarityDb = 2; CompStyle = 1 }); Verified = $false }
    'warm'          = @{ Title = 'Warm';
                         Text  = 'Mac EQ and virtual bass, no compressor: more dynamics, a little quieter.';
                         Values = (New-PresetValues @{ SpeakerDsp = 2 }); Verified = $false }
    'natural'       = @{ Title = 'Natural';
                         Text  = 'Only the Mac EQ and crossover, no virtual bass or compressor.';
                         Values = (New-PresetValues @{ SpeakerDsp = 1 }); Verified = $false }
    'raw'           = @{ Title = 'No processing (for comparison)';
                         Text  = 'Plain path: full-range woofers, tweeters high-passed at 3 kHz. Sounds flat.';
                         Values = (New-PresetValues @{ SpeakerDsp = 0 }); Verified = $false }
}

# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------
function Write-Banner {
    Write-Host ''
    Write-Host '  ╔══════════════════════════════════════════════════════════╗' -ForegroundColor Cyan
    Write-Host '  ║        T2AudioPort  ·  Apple T2 audio for Windows        ║' -ForegroundColor Cyan
    Write-Host '  ║                  MacBook Pro 16" (2019)                  ║' -ForegroundColor DarkCyan
    Write-Host '  ╚══════════════════════════════════════════════════════════╝' -ForegroundColor Cyan
}
function Write-Section($t) { Write-Host ''; Write-Host "  ── $t " -ForegroundColor Cyan }
function Write-Ok($m)   { Write-Host "   ✓ $m" -ForegroundColor Green }
function Write-Info($m) { Write-Host "     $m" -ForegroundColor Gray }
function Write-Warn($m) { Write-Host "   ! $m" -ForegroundColor Yellow }
function Write-Fail($m) { Write-Host "   ✗ $m" -ForegroundColor Red }

function Confirm-Action($prompt) {
    if ($Force) { return $true }
    $a = Read-Host "  $prompt [y/n]"
    return ($a -match '^(y|yes)$')
}

# ---------------------------------------------------------------------------
# System checks
# ---------------------------------------------------------------------------
function Test-Admin {
    ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-T2Device {
    Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -like "$HardwareId*" } | Select-Object -First 1
}

function Get-DeviceService($dev) {
    if (-not $dev) { return $null }
    try { (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction Stop).Data } catch { $null }
}

function Get-DeviceDriverVersion($dev) {
    if (-not $dev) { return $null }
    try { (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName 'DEVPKEY_Device_DriverVersion' -ErrorAction Stop).Data } catch { $null }
}

function Test-TestSigning {
    $out = & bcdedit.exe /enum '{current}' 2>$null | Out-String
    return ($out -match '(?im)^\s*testsigning\s+Yes')
}

function Get-OemInfsForT2 {
    Get-ChildItem -Path "$env:WINDIR\INF\oem*.inf" -File -ErrorAction SilentlyContinue | Where-Object {
        Select-String -LiteralPath $_.FullName -Pattern 'T2AudioMiniport' -SimpleMatch -Quiet
    } | ForEach-Object { $_.Name }
}

function Get-Param($name) {
    try { (Get-ItemProperty -Path $ParamsPath -Name $name -ErrorAction Stop).$name } catch { $null }
}

function Set-Param($name, [int]$value) {
    if (-not (Test-Path $ParamsPath)) { New-Item -Path $ParamsPath -Force | Out-Null }
    New-ItemProperty -Path $ParamsPath -Name $name -Value $value -PropertyType DWord -Force | Out-Null
}

function Invoke-Pnp([string[]]$arguments) {
    $out = & pnputil.exe @arguments 2>&1 | Out-String
    return @{ Code = $LASTEXITCODE; Out = $out }
}

function Wait-DeviceService($expected, [int]$seconds = 20) {
    $deadline = (Get-Date).AddSeconds($seconds)
    do {
        Start-Sleep -Milliseconds 700
        $d = Get-T2Device
        if ($d -and $d.Status -eq 'OK' -and (Get-DeviceService $d) -eq $expected) { return $d }
    } while ((Get-Date) -lt $deadline)
    return (Get-T2Device)
}

# ---------------------------------------------------------------------------
# T2 speaker endpoint volume (Windows keeps volume and mute per endpoint;
# test runs used to leave the T2 endpoint muted).
# ---------------------------------------------------------------------------
$code = @'
using System;
using System.Runtime.InteropServices;

namespace T2Setup {
[Flags] public enum CLSCTX : uint { ALL = 0x17 }
[ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] public class MMDeviceEnumerator { }
[Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IMMDeviceEnumerator {
    [PreserveSig] int EnumAudioEndpoints(int dataFlow, uint stateMask, out IntPtr devices);
    [PreserveSig] int GetDefaultAudioEndpoint(int dataFlow, int role, out IntPtr endpoint);
    [PreserveSig] int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id, out IMMDevice device);
}
[Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IMMDevice {
    [PreserveSig] int Activate(ref Guid iid, CLSCTX clsCtx, IntPtr activationParams, [MarshalAs(UnmanagedType.IUnknown)] out object iface);
}
[Guid("5CDF2C82-841E-4546-9722-0CF74078229A"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IAudioEndpointVolume {
    [PreserveSig] int RegisterControlChangeNotify(IntPtr notify);
    [PreserveSig] int UnregisterControlChangeNotify(IntPtr notify);
    [PreserveSig] int GetChannelCount(out uint count);
    [PreserveSig] int SetMasterVolumeLevel(float levelDb, ref Guid context);
    [PreserveSig] int SetMasterVolumeLevelScalar(float level, ref Guid context);
    [PreserveSig] int GetMasterVolumeLevel(out float levelDb);
    [PreserveSig] int GetMasterVolumeLevelScalar(out float level);
    [PreserveSig] int SetChannelVolumeLevel(uint channel, float levelDb, ref Guid context);
    [PreserveSig] int SetChannelVolumeLevelScalar(uint channel, float level, ref Guid context);
    [PreserveSig] int GetChannelVolumeLevel(uint channel, out float levelDb);
    [PreserveSig] int GetChannelVolumeLevelScalar(uint channel, out float level);
    [PreserveSig] int SetMute([MarshalAs(UnmanagedType.Bool)] bool mute, ref Guid context);
    [PreserveSig] int GetMute([MarshalAs(UnmanagedType.Bool)] out bool mute);
}
public static class Volume {
    // Per-channel levels ("L=0.30 R=0.12"): Windows keeps a balance per
    // endpoint (Sound settings, Levels, Balance) across driver reinstalls.
    static string Channels(IAudioEndpointVolume v) {
        uint n; if (v.GetChannelCount(out n) != 0) return "channels=?";
        var parts = new System.Collections.Generic.List<string>();
        for (uint c = 0; c < n; c++) {
            float l; v.GetChannelVolumeLevelScalar(c, out l);
            parts.Add((n == 2 ? (c == 0 ? "L" : "R") : ("ch" + c)) + "=" + l.ToString("0.00"));
        }
        return string.Join(" ", parts.ToArray());
    }
    static IAudioEndpointVolume Open(string id) {
        var en = (IMMDeviceEnumerator)Activator.CreateInstance(typeof(MMDeviceEnumerator));
        IMMDevice dev;
        if (en.GetDevice(id, out dev) != 0 || dev == null) return null;
        Guid iid = new Guid("5CDF2C82-841E-4546-9722-0CF74078229A");
        object o;
        if (dev.Activate(ref iid, CLSCTX.ALL, IntPtr.Zero, out o) != 0 || o == null) return null;
        return (IAudioEndpointVolume)o;
    }
    public static string Get(string id) {
        var v = Open(id);
        if (v == null) return "no endpoint volume";
        bool m; float l;
        v.GetMute(out m);
        v.GetMasterVolumeLevelScalar(out l);
        return "mute=" + m + " volume=" + l.ToString("0.00") + " " + Channels(v);
    }
    // Un-mute the endpoint, set its volume and an even L/R balance;
    // returns the channel levels before and after.
    public static string Set(string id, float level) {
        var bal = Open(id);
        string before = bal == null ? "?" : Channels(bal);
        var en = (IMMDeviceEnumerator)Activator.CreateInstance(typeof(MMDeviceEnumerator));
        IMMDevice dev;
        if (en.GetDevice(id, out dev) != 0 || dev == null) return "device not found";
        Guid iid = new Guid("5CDF2C82-841E-4546-9722-0CF74078229A");
        Guid ctx = Guid.Empty;
        object o;
        if (dev.Activate(ref iid, CLSCTX.ALL, IntPtr.Zero, out o) != 0 || o == null) return "no endpoint volume";
        var v = (IAudioEndpointVolume)o;
        v.SetMasterVolumeLevelScalar(level, ref ctx);
        uint n;
        if (v.GetChannelCount(out n) == 0) {
            for (uint c = 0; c < n; c++) v.SetChannelVolumeLevelScalar(c, level, ref ctx);
        }
        v.SetMute(false, ref ctx);
        bool m; float l;
        v.GetMute(out m);
        v.GetMasterVolumeLevelScalar(out l);
        return "mute=" + m + " volume=" + l.ToString("0.00") + "; balance before: " + before + ", now: " + Channels(v);
    }
}
}
'@

function Get-T2Endpoint {
    $root = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
    if (-not (Test-Path $root)) { return $null }
    $prop = '{a8b865dd-2e3d-4094-ad97-e593a70c75d6},8'
    $descProp = '{a45c254e-df1c-4efd-8020-67d146a850e0},2'
    $first = $null
    # The driver can expose two T2 outputs (speakers and headphones): prefer
    # the speakers, fall back to the first T2 endpoint.
    foreach ($k in Get-ChildItem $root) {
        $st = (Get-ItemProperty -Path $k.PSPath -ErrorAction SilentlyContinue).DeviceState
        if ($st -ne 1) { continue }
        $props = Join-Path $k.PSPath 'Properties'
        if (-not (Test-Path $props)) { continue }
        $all = Get-ItemProperty -Path $props -ErrorAction SilentlyContinue
        $v = $all.$prop
        if (-not ($v -and $v.StartsWith($HardwareId))) { continue }
        $id = '{0.0.0.00000000}.' + $k.PSChildName
        if ("$($all.$descProp)" -match 'Speaker') { return $id }
        if (-not $first) { $first = $id }
    }
    return $first
}

function Set-T2Volume([double]$level) {
    if ($level -lt 0) { return }
    $ep = $null
    for ($i = 0; $i -lt 10 -and -not $ep; $i++) { $ep = Get-T2Endpoint; if (-not $ep) { Start-Sleep -Seconds 1 } }
    if (-not $ep) { Write-Warn 'The T2 speaker endpoint has not appeared in Windows yet; volume unchanged.'; return }
    try {
        if (-not ('T2Setup.Volume' -as [type])) { Add-Type -TypeDefinition $code }
        $r = [T2Setup.Volume]::Set($ep, [float]$level)
        Write-Ok "T2 speakers: unmuted, volume $([int]($level * 100)) %, L/R balance evened ($r)"
    } catch {
        Write-Warn "Could not set the volume: $_"
    }
}

# ---------------------------------------------------------------------------
# Actions
# ---------------------------------------------------------------------------
function Show-Presets {
    Write-Section 'Sound presets'
    $i = 1
    foreach ($k in $Presets.Keys) {
        $p = $Presets[$k]
        $mark = if ($p.Verified) { ' ★ approved' } else { '' }
        Write-Host ("   {0}. {1,-34}{2}" -f $i, $p.Title, $mark) -ForegroundColor White
        Write-Host ("      {0}" -f $p.Text) -ForegroundColor DarkGray
        $i++
    }
}

function Select-PresetInteractive {
    Show-Presets
    $keys = @($Presets.Keys)
    $a = Read-Host "  Preset number (Enter = 1)"
    if (-not $a) { return $keys[0] }
    $n = 0
    if ([int]::TryParse($a, [ref]$n) -and $n -ge 1 -and $n -le $keys.Count) { return $keys[$n - 1] }
    Write-Warn 'No such number; using "Like macOS".'
    return $keys[0]
}

function Get-CurrentPresetName {
    foreach ($k in $Presets.Keys) {
        $match = $true
        foreach ($n in $PresetValueNames) {
            if ((Get-EffectiveParam $n) -ne [int]$Presets[$k].Values[$n]) { $match = $false; break }
        }
        if ($match) { return $k }
    }
    return $null
}

function Write-PresetValues($name) {
    $p = $Presets[$name]
    foreach ($n in $PresetValueNames) { Set-Param $n $p.Values[$n] }
    Write-Ok "Preset '$($p.Title)': $(($PresetValueNames | ForEach-Object { "$_=$($p.Values[$_])" }) -join ', ')"
}

# Current value of a parameter, or its default (a DWORD reads as Int32 in
# PowerShell 7 and as UInt32 elsewhere; both give -3 for -3 dB).
function Get-EffectiveParam($name) {
    $v = Get-Param $name
    if ($null -eq $v) { return [int]$ParamDefaults[$name] }
    if ($v -gt [int]::MaxValue) { return [int]($v - 0x100000000) }
    return [int]$v
}

function Format-Db([int]$v) { if ($v -gt 0) { "+$v" } else { "$v" } }

function Restart-T2Device {
    $d = Get-T2Device
    if (-not $d) { Write-Fail 'T2 device not found.'; return $false }
    if ((Get-DeviceService $d) -ne $ServiceName) {
        Write-Info 'T2AudioPort is not active right now - the setting applies after installation.'
        return $true
    }
    Write-Info 'Restarting the device so the driver reads the settings (sound stops for a few seconds)...'
    $r = Invoke-Pnp @('/restart-device', $d.InstanceId)
    $d = Wait-DeviceService $ServiceName
    if ($d -and $d.Status -eq 'OK') { Write-Ok 'Device restarted.'; return $true }
    Write-Warn "Device after restart: $($d.Status) (pnputil code $($r.Code)). A reboot may be needed."
    return $false
}

function Invoke-Status {
    Write-Section 'Status'
    $d = Get-T2Device
    if (-not $d) { Write-Fail 'Apple T2 audio device (VEN_106B&DEV_1803) not found.'; return }
    $svc = Get-DeviceService $d
    $ver = Get-DeviceDriverVersion $d
    $drv = switch ($svc) { 'T2AudioMiniport' { 'T2AudioPort' } 'AppleAudio' { 'AppleAudio (Apple driver)' } default { "$svc" } }
    $color = if ($d.Status -eq 'OK') { 'Green' } else { 'Yellow' }
    Write-Host ("   Driver:         {0}  {1}" -f $drv, $ver) -ForegroundColor White
    Write-Host ("   Device:         {0}" -f $d.Status) -ForegroundColor $color
    $prod = Get-Param 'EnableProduction'
    if ($svc -eq 'AppleAudio' -and $prod -eq 1) {
        Write-Warn 'The device runs AppleAudio although T2AudioPort is set to persistent mode:'
        Write-Info 'Windows (probably Windows Update) replaced the driver. Run -Action Install again;'
        Write-Info '-Action UpdateGuard -Block 1 stops Windows Update from installing drivers.'
    }
    Write-Host ("   Persistent mode: {0}" -f $(if ($prod -eq 1) { 'on' } else { 'off' })) -ForegroundColor White
    $cur = Get-CurrentPresetName
    $pn = if ($cur) { $Presets[$cur].Title } else { 'custom / defaults' }
    Write-Host ("   Preset:         {0}" -f $pn) -ForegroundColor White
    Write-Host ("   Tone:           bass {0} dB, treble {1} dB, clarity {2} dB, compressor {3}" -f `
        (Format-Db (Get-EffectiveParam 'BassDb')), (Format-Db (Get-EffectiveParam 'TrebleDb')), `
        (Format-Db (Get-EffectiveParam 'ClarityDb')), $(if ((Get-EffectiveParam 'CompStyle') -eq 1) { 'precise (like macOS)' } else { 'fast' })) -ForegroundColor White
    $strikes = Get-Param 'CrashGuardStrikes'; $armed = Get-Param 'CrashGuardArmed'
    if ($strikes -ge 2) {
        Write-Fail "Crash guard TRIPPED ($strikes unclean ends in a row): the driver does not start. Use 'Reset crash guard' once the cause is known."
    } elseif ($strikes -ge 1) {
        Write-Warn "The last session did not end cleanly (unclean ends in a row: $strikes)."
    } else {
        Write-Host '   Crash guard:    OK' -ForegroundColor White
    }
    $ep = Get-T2Endpoint
    if ($ep) {
        try {
            if (-not ('T2Setup.Volume' -as [type])) { Add-Type -TypeDefinition $code }
            $vol = [T2Setup.Volume]::Get($ep)
            Write-Host ("   T2 volume:      {0}" -f $vol) -ForegroundColor White
            if ($vol -match 'L=([\d.,]+) R=([\d.,]+)') {
                $l = [double]($Matches[1] -replace ',', '.'); $r = [double]($Matches[2] -replace ',', '.')
                if ([math]::Abs($l - $r) -gt 0.02) { Write-Warn 'L/R balance is uneven (Sound settings, Levels, Balance). Fix: .\T2Audio-Setup.ps1 -Action Balance' }
            }
        } catch { }
    }
    Show-Features
    $ts = if (Test-TestSigning) { 'on' } else { 'OFF' }
    Write-Host ("   testsigning:    {0}" -f $ts) -ForegroundColor White
    if (Test-Path $TestSession) { Write-Warn 'A test-agent session (TestSession) exists - it is removed at reboot.' }
}

function Invoke-Install {
    Write-Section 'Installing T2AudioPort'
    $model = (Get-CimInstance Win32_ComputerSystem -ErrorAction SilentlyContinue).Model
    if ($model -and $model -ne 'MacBookPro16,1') {
        Write-Warn "This is a $model; the sound processing is tuned for MacBookPro16,1."
        if (-not (Confirm-Action 'Continue anyway?')) { return }
    }
    if (-not (Test-TestSigning)) {
        Write-Fail 'testsigning is off: Windows will not accept the test-signed driver.'
        Write-Info  'Enable it: bcdedit /set testsigning on, then reboot (Secure Boot must be off).'
        return
    }
    foreach ($f in @($InfPath, $SysPath, $CatPath)) {
        if (-not (Test-Path -LiteralPath $f)) { Write-Fail "Package file missing: $f (build and sign it as in GEMINI.md section 2)"; return }
    }
    $sig = Get-AuthenticodeSignature -LiteralPath $SysPath
    if (-not $sig.SignerCertificate) { Write-Fail 'T2AudioMiniport.sys is not signed.'; return }
    if (-not (Install-PackageCertificate $sig.SignerCertificate)) { return }
    $hash = (Get-FileHash -LiteralPath $SysPath -Algorithm SHA256).Hash
    $infVer = (Select-String -LiteralPath $InfPath -Pattern '^DriverVer\s*=\s*(.+)$').Matches | Select-Object -First 1
    Write-Ok "Package: $(if ($infVer) { $infVer.Groups[1].Value } else { '?' }), SHA256 $($hash.Substring(0, 16))…"

    $d = Get-T2Device
    if (-not $d) { Write-Fail 'Apple T2 audio device not found.'; return }

    Write-Host ''
    Write-Warn 'This driver is experimental. Once installed it starts on every boot.'
    Write-Info 'If two sessions in a row end uncleanly (BSOD, hang), it stops starting by itself;'
    Write-Info 'the Apple driver comes back with this script ("Restore AppleAudio") or from Safe Mode.'
    if (-not $Force) {
        $a = Read-Host "  Type INSTALL to continue"
        if ($a -ne 'INSTALL') { Write-Warn 'Cancelled.'; return }
    }

    if (Test-Path $TestSession) { Remove-Item -Path $TestSession -Recurse -Force; Write-Info 'Test-agent session removed.' }
    Unregister-ScheduledTask -TaskName 'T2Audio-AutoCapture-AfterBoot' -Confirm:$false -ErrorAction SilentlyContinue

    # Settings before binding: the driver reads them at start.
    Write-PresetValues $Preset
    Set-Param 'EnableProduction' 1
    Set-Param 'CrashGuardArmed' 0
    Set-Param 'CrashGuardStrikes' 0
    Write-Ok 'Persistent mode on, crash guard reset.'

    # Disabling the device before binding avoids Code 14 (reboot required).
    Write-Info 'Binding the driver to the device...'
    $null = Invoke-Pnp @('/disable-device', $d.InstanceId)
    $r = Invoke-Pnp @('/add-driver', $InfPath, '/install')
    $null = Invoke-Pnp @('/enable-device', $d.InstanceId)
    if ($r.Code -ne 0 -and $r.Code -ne 3010) {
        Write-Fail "pnputil /add-driver returned $($r.Code)."
        Write-Info ($r.Out.Trim())
        return
    }
    $d = Wait-DeviceService $ServiceName 30
    if ($d -and $d.Status -eq 'OK' -and (Get-DeviceService $d) -eq $ServiceName) {
        Write-Ok 'T2AudioPort is installed and running.'
        Set-T2Volume $Volume
        Write-Host ''
        Write-Info 'Pick the Apple T2 "Speakers" as the output in Sound settings if Windows did not do it by itself.'
        Write-Info 'Change the preset any time: .\T2Audio-Setup.ps1 -Action Preset -Preset bright'
    } elseif ($r.Code -eq 3010) {
        Write-Warn 'Windows asks for a reboot to finish the installation.'
    } else {
        Write-Fail "The device did not start: $($d.Status), driver $(Get-DeviceService $d)."
        Write-Info 'Check testsigning and the signature; to roll back use "Restore AppleAudio".'
    }
}

# A release package may ship the public test certificate in certificate\*.cer.
# Windows only loads the test-signed driver when that certificate is trusted
# (Root + TrustedPublisher of the local machine). Returns $false to stop.
function Install-PackageCertificate($signer) {
    $trusted = Get-ChildItem Cert:\LocalMachine\TrustedPublisher -ErrorAction SilentlyContinue |
        Where-Object { $_.Thumbprint -eq $signer.Thumbprint }
    if ($trusted) { return $true }
    $cer = Get-ChildItem -Path (Join-Path $ProjectRoot 'certificate') -Filter *.cer -ErrorAction SilentlyContinue |
        Where-Object {
            (New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $_.FullName).Thumbprint -eq $signer.Thumbprint
        } | Select-Object -First 1
    if (-not $cer) {
        Write-Warn "The signing certificate ($($signer.Subject)) is not in TrustedPublisher; Windows may refuse the driver."
        return $true
    }
    Write-Info "The package is signed with $($signer.Subject) ($($signer.Thumbprint))."
    if (-not (Confirm-Action 'Trust this test certificate on this computer (Root + TrustedPublisher)?')) { return $false }
    Import-Certificate -FilePath $cer.FullName -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $cer.FullName -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
    Write-Ok 'Certificate trusted.'
    return $true
}

function Invoke-Preset {
    Write-Section 'Changing the preset'
    Write-PresetValues $Preset
    $null = Restart-T2Device
}

function Read-DbInteractive($label, $name) {
    $cur = Get-EffectiveParam $name
    $a = Read-Host ("  {0}, dB from -12 to +12 (now {1}; Enter = unchanged)" -f $label, (Format-Db $cur))
    if (-not $a) { return $null }
    $n = 0
    if ([int]::TryParse($a.Trim().TrimStart('+'), [ref]$n) -and $n -ge -12 -and $n -le 12) { return $n }
    Write-Warn 'Needs a whole number from -12 to +12; unchanged.'
    return $null
}

# Tone on top of the current preset: changes only the given values.
function Invoke-Tune([switch]$Interactive) {
    Write-Section 'Tone'
    $changes = [ordered]@{}
    if ($Interactive) {
        foreach ($pair in @(@('Bass (150 Hz)', 'BassDb'), @('Treble (6 kHz)', 'TrebleDb'), @('Clarity (2.5 kHz)', 'ClarityDb'))) {
            $v = Read-DbInteractive $pair[0] $pair[1]
            if ($null -ne $v) { $changes[$pair[1]] = $v }
        }
        $c = Read-Host '  Compressor: 0 = fast, 1 = precise like macOS (Enter = unchanged)'
        if ($c -eq '0' -or $c -eq '1') { $changes['CompStyle'] = [int]$c }
    } else {
        if ($null -ne $BassDb)    { $changes['BassDb'] = $BassDb }
        if ($null -ne $TrebleDb)  { $changes['TrebleDb'] = $TrebleDb }
        if ($null -ne $ClarityDb) { $changes['ClarityDb'] = $ClarityDb }
        if ($null -ne $CompStyle) { $changes['CompStyle'] = $CompStyle }
    }
    if ($changes.Count -eq 0) {
        Write-Info 'Nothing changed. Example: .\T2Audio-Setup.ps1 -Action Tune -BassDb 3 -TrebleDb -2'
        return
    }
    foreach ($k in $changes.Keys) { Set-Param $k $changes[$k] }
    Write-Ok "Tone: $(($changes.Keys | ForEach-Object { "$_=$($changes[$_])" }) -join ', ')"
    $null = Restart-T2Device
}

function Invoke-ResetGuard {
    Write-Section 'Resetting the crash guard'
    $s = Get-Param 'CrashGuardStrikes'
    Write-Info "Unclean ends in a row now: $(if ($null -eq $s) { 0 } else { $s })"
    if (-not (Confirm-Action 'Reset and let the driver start?')) { return }
    Set-Param 'CrashGuardArmed' 0
    Set-Param 'CrashGuardStrikes' 0
    Write-Ok 'Reset.'
    $null = Restart-T2Device
}

function Invoke-Uninstall {
    Write-Section 'Restoring the Apple driver (AppleAudio)'
    if (-not (Confirm-Action 'Remove T2AudioPort and restore AppleAudio?')) { return }
    if (Test-Path $ParamsPath) { Set-Param 'EnableProduction' 0 }
    if (Get-UpdateGuardOn) { Write-Info 'The Windows Update driver guard is still on: -Action UpdateGuard -Block 0 turns it off.' }
    if (Test-Path $TestSession) { Remove-Item -Path $TestSession -Recurse -Force }
    $infs = @(Get-OemInfsForT2)
    if ($infs.Count -eq 0) { Write-Info 'No T2AudioPort package in the driver store.' }
    foreach ($inf in $infs) {
        $r = Invoke-Pnp @('/delete-driver', $inf, '/uninstall', '/force')
        if ($r.Code -eq 0) { Write-Ok "Removed $inf" } else { Write-Warn "pnputil /delete-driver ${inf}: code $($r.Code)" }
    }
    $null = Invoke-Pnp @('/scan-devices')
    $d = Wait-DeviceService 'AppleAudio' 30
    if ($d -and (Get-DeviceService $d) -eq 'AppleAudio' -and $d.Status -eq 'OK') {
        Write-Ok 'The device is back on AppleAudio.'
    } else {
        Write-Warn "Now: $($d.Status), driver $(Get-DeviceService $d)."
        Write-Info 'If AppleAudio did not come back by itself: reboot, or .\Restore-AppleAudioDriver.ps1 (downloads the Boot Camp driver).'
    }
}

# Optional driver features (1.0.13.x; all off by default, all verified on
# hardware 2026-10-29). Headphones = HeadphoneOutput + JackEvents (jack
# tracking) + HeadphoneAutoMute (speakers silent while headphones are in).
$FeatureParams = [ordered]@{
    Headphones       = @('HeadphoneOutput', 'JackEvents', 'HeadphoneAutoMute')
    Microphone       = @('MicInput', 'MicStartIo')   # recording verified on 1.0.13.4
    ResumeAfterSleep = @('ResumeAfterSleep')
    ExtraFormats     = @('ExtraFormats')
}
$FeatureTitles = @{
    Headphones = 'Headphones (3.5 mm output)'; Microphone = 'Built-in microphones'
    ResumeAfterSleep = 'Sound back after sleep by itself'; ExtraFormats = '16/24-bit formats'
}

function Show-Features {
    foreach ($f in $FeatureParams.Keys) {
        $on = ((Get-Param $FeatureParams[$f][0]) -eq 1)
        Write-Host ("   {0,-46} {1}" -f $FeatureTitles[$f], $(if ($on) { 'on' } else { 'off' })) -ForegroundColor White
    }
}

function Invoke-Features([switch]$Interactive) {
    Write-Section 'Optional features'
    $changes = [ordered]@{}
    if ($Interactive) {
        foreach ($f in $FeatureParams.Keys) {
            $cur = ((Get-Param $FeatureParams[$f][0]) -eq 1)
            $a = Read-Host ("  {0} (now {1}): 1 = on, 0 = off, Enter = unchanged" -f $FeatureTitles[$f], $(if ($cur) { 'on' } else { 'off' }))
            if ($a -eq '0' -or $a -eq '1') { $changes[$f] = [int]$a }
        }
    } else {
        foreach ($f in $FeatureParams.Keys) {
            $v = Get-Variable -Name $f -Scope Script -ValueOnly -ErrorAction SilentlyContinue
            if ($null -ne $v) { $changes[$f] = [int]$v }
        }
    }
    if ($changes.Count -eq 0) {
        Show-Features
        Write-Info 'Nothing changed. Example: .\T2Audio-Setup.ps1 -Action Features -Headphones 1 -Microphone 1'
        return
    }
    foreach ($f in $changes.Keys) {
        foreach ($n in $FeatureParams[$f]) { Set-Param $n $changes[$f] }
        Write-Ok ("{0}: {1}" -f $FeatureTitles[$f], $(if ($changes[$f] -eq 1) { 'on' } else { 'off' }))
    }
    Write-Warn 'If something goes wrong: .\T2Audio-Setup.ps1 -Action Features -Headphones 0 -Microphone 0 -ResumeAfterSleep 0 -ExtraFormats 0'
    $null = Restart-T2Device
}

# Windows Update can reinstall Apple's Microsoft-signed AppleAudio over the
# test-signed T2AudioPort. Opt-in guard: the documented Windows settings
# "do not include drivers with Windows quality updates"
# (Policies\...\WindowsUpdate\ExcludeWUDriversInQualityUpdate=1) and "do not
# search Windows Update for drivers" (DriverSearching\SearchOrderConfig=0).
# They apply to ALL devices. The previous values are saved under the
# driver's Parameters key and restored by -Block 0.
$UpdateGuardKeys = @(
    @{ Path = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate'; Name = 'ExcludeWUDriversInQualityUpdate'; On = 1; Save = 'SavedExcludeWUDrivers' },
    @{ Path = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\DriverSearching'; Name = 'SearchOrderConfig'; On = 0; Save = 'SavedSearchOrderConfig' }
)

function Get-UpdateGuardOn {
    $k = $UpdateGuardKeys[0]
    try { return ((Get-ItemProperty -Path $k.Path -Name $k.Name -ErrorAction Stop).($k.Name) -eq 1) } catch { return $false }
}

function Invoke-UpdateGuard {
    Write-Section 'Windows Update driver guard'
    if ($null -eq $Block) {
        Write-Host ("   Guard: {0}" -f $(if (Get-UpdateGuardOn) { 'on (Windows Update installs no drivers)' } else { 'off' })) -ForegroundColor White
        Write-Info 'Example: .\T2Audio-Setup.ps1 -Action UpdateGuard -Block 1   (0 restores the previous settings)'
        return
    }
    if ($Block -eq 1) {
        Write-Warn 'This stops Windows Update from installing drivers for ALL devices, not just audio.'
        if (-not (Confirm-Action 'Turn the guard on?')) { return }
        foreach ($k in $UpdateGuardKeys) {
            $old = $null
            try { $old = (Get-ItemProperty -Path $k.Path -Name $k.Name -ErrorAction Stop).($k.Name) } catch { }
            if ($null -eq (Get-Param $k.Save)) { Set-Param $k.Save $(if ($null -eq $old) { -1 } else { [int]$old }) }
            if (-not (Test-Path $k.Path)) { New-Item -Path $k.Path -Force | Out-Null }
            New-ItemProperty -Path $k.Path -Name $k.Name -Value $k.On -PropertyType DWord -Force | Out-Null
        }
        Write-Ok 'Guard on. Update drivers for other devices by hand while it is on.'
    } else {
        foreach ($k in $UpdateGuardKeys) {
            $saved = Get-Param $k.Save
            if ($null -eq $saved -or $saved -eq -1 -or $saved -eq 4294967295) {
                Remove-ItemProperty -Path $k.Path -Name $k.Name -ErrorAction SilentlyContinue
            } else {
                New-ItemProperty -Path $k.Path -Name $k.Name -Value ([int]$saved) -PropertyType DWord -Force | Out-Null
            }
            Remove-ItemProperty -Path $ParamsPath -Name $k.Save -ErrorAction SilentlyContinue
        }
        Write-Ok 'Guard off, previous settings restored.'
    }
}

function Show-Menu {
    while ($true) {
        Write-Banner
        Invoke-Status
        Write-Section 'What to do?'
        Write-Host '   1. Install / update T2AudioPort' -ForegroundColor White
        Write-Host '   2. Change the sound preset' -ForegroundColor White
        Write-Host '   3. Reset the crash guard' -ForegroundColor White
        Write-Host '   4. Restore AppleAudio (remove T2AudioPort)' -ForegroundColor White
        Write-Host '   5. Adjust the tone (bass / treble / clarity)' -ForegroundColor White
        Write-Host '   6. Optional features (headphones, microphones, sleep, formats)' -ForegroundColor White
        Write-Host '   7. Windows Update driver guard' -ForegroundColor White
        Write-Host '   0. Exit' -ForegroundColor White
        $c = Read-Host "`n  Choice"
        switch ($c) {
            '1' { $script:Preset = Select-PresetInteractive; Invoke-Install }
            '2' { $script:Preset = Select-PresetInteractive; Invoke-Preset }
            '3' { Invoke-ResetGuard }
            '4' { Invoke-Uninstall }
            '5' { Invoke-Tune -Interactive }
            '6' { Invoke-Features -Interactive }
            '7' {
                $a = Read-Host '  1 = guard on, 0 = guard off, Enter = show'
                if ($a -eq '0' -or $a -eq '1') { $script:Block = [int]$a }
                Invoke-UpdateGuard
                $script:Block = $null
            }
            '0' { return }
            default { Write-Warn 'No such item.' }
        }
        Read-Host "`n  Enter - back to the menu" | Out-Null
    }
}

# ---------------------------------------------------------------------------
if (-not (Test-Admin)) { Write-Fail 'Run PowerShell as administrator.'; exit 1 }
switch ($Action) {
    'Menu'       { Show-Menu }
    'Install'    { Write-Banner; Invoke-Install }
    'Preset'     { Write-Banner; Invoke-Preset }
    'Tune'       { Write-Banner; Invoke-Tune }
    'Features'   { Write-Banner; Invoke-Features }
    'UpdateGuard' { Write-Banner; Invoke-UpdateGuard }
    'Balance'    { Write-Banner; Write-Section 'Balance'; $v = if ($Volume -ge 0) { $Volume } else { 0.3 }; Set-T2Volume $v }
    'Status'     { Write-Banner; Invoke-Status }
    'ResetGuard' { Write-Banner; Invoke-ResetGuard }
    'Uninstall'  { Write-Banner; Invoke-Uninstall }
}
