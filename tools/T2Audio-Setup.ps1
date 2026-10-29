# T2AudioPort - installer and sound settings (MacBookPro16,1).
#
# Menu (run without parameters) or parameters:
#   .\T2Audio-Setup.ps1                                  # interactive menu
#   .\T2Audio-Setup.ps1 -Action Prepare                  # turn on test mode (once, then reboot)
#   .\T2Audio-Setup.ps1 -Action Install -Preset macos    # install, persistent mode
#   .\T2Audio-Setup.ps1 -Action Preset  -Preset bright   # change the sound preset
#   .\T2Audio-Setup.ps1 -Action Tune -BassDb 3 -ClarityDb 2  # tone on top of the preset
#   .\T2Audio-Setup.ps1 -Action Balance -Volume 0.3      # even out the L/R balance
#   .\T2Audio-Setup.ps1 -Action Features -Headphones 1 -Microphone 1   # optional features
#   .\T2Audio-Setup.ps1 -Action UpdateGuard -Block 1     # keep Windows Update from restoring AppleAudio
#   .\T2Audio-Setup.ps1 -Action Status
#   .\T2Audio-Setup.ps1 -Action Doctor                   # check everything, with fixes
#   .\T2Audio-Setup.ps1 -Action Switch -To Headphones    # default output (no admin needed)
#   .\T2Audio-Setup.ps1 -Action Shortcut                 # desktop shortcut Ctrl+Alt+H: speakers <-> headphones
#   .\T2Audio-Setup.ps1 -Action SoundTest                # left / right / both test tones
#   .\T2Audio-Setup.ps1 -Action MicTest                  # record 5 s, show the level, play it back
#   .\T2Audio-Setup.ps1 -Action Backup  [-File x.json]   # save your settings
#   .\T2Audio-Setup.ps1 -Action Restore [-File x.json]   # load them again
#   .\T2Audio-Setup.ps1 -Action Report                   # zip for a bug report (on the desktop)
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
    [ValidateSet('Menu', 'Prepare', 'Install', 'Preset', 'Tune', 'Balance', 'Features', 'UpdateGuard', 'Status',
                 'Doctor', 'Switch', 'Shortcut', 'SoundTest', 'MicTest', 'Backup', 'Restore', 'Report',
                 'ResetGuard', 'Uninstall')]
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

    # -Action Switch: which T2 output becomes the Windows default.
    [ValidateSet('Speakers', 'Headphones', 'Toggle')]
    [string]$To = 'Toggle',

    # -Action Backup / Restore: the settings file (default: Desktop\T2AudioPort-settings.json).
    [string]$File = '',

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
    # Legal notice (GPL-3.0 section 7b attribution term of the KAIT2EN profile).
    Write-Host '   GPL-3.0-or-later. Speaker/mic DSP from the KAIT2EN 16_1 profile (C) André Eikmeyer;' -ForegroundColor DarkGray
    Write-Host '   virtual bass after bankstown (C) James Calligeros, MIT. See THIRD_PARTY_LICENSES.md.' -ForegroundColor DarkGray
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
using System.IO;
using System.Text;
using System.Threading;
using System.Runtime.InteropServices;

namespace T2Setup {
[Flags] public enum CLSCTX : uint { ALL = 0x17 }
[ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] public class MMDeviceEnumerator { }
[Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IMMDeviceEnumerator {
    [PreserveSig] int EnumAudioEndpoints(int dataFlow, uint stateMask, out IntPtr devices);
    [PreserveSig] int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice endpoint);
    [PreserveSig] int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id, out IMMDevice device);
}
[Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IMMDevice {
    [PreserveSig] int Activate(ref Guid iid, CLSCTX clsCtx, IntPtr activationParams, [MarshalAs(UnmanagedType.IUnknown)] out object iface);
    [PreserveSig] int OpenPropertyStore(int access, out IntPtr store);
    [PreserveSig] int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
    [PreserveSig] int GetState(out int state);
}
// Undocumented but stable since Vista: what Sound settings itself uses to set
// the default device (also in tools/Set-DefaultAudioDevice.ps1).
[Guid("F8679F50-850A-41CF-9C72-430F290290C8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IPolicyConfig {
    [PreserveSig] int GetMixFormat([MarshalAs(UnmanagedType.LPWStr)] string device, out IntPtr format);
    [PreserveSig] int GetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string device, [MarshalAs(UnmanagedType.Bool)] bool isDefault, out IntPtr format);
    [PreserveSig] int ResetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string device);
    [PreserveSig] int SetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string device, IntPtr endpointFormat, IntPtr mixFormat);
    [PreserveSig] int GetProcessingPeriod([MarshalAs(UnmanagedType.LPWStr)] string device, [MarshalAs(UnmanagedType.Bool)] bool isDefault, IntPtr defaultPeriod, IntPtr minimumPeriod);
    [PreserveSig] int SetProcessingPeriod([MarshalAs(UnmanagedType.LPWStr)] string device, IntPtr period);
    [PreserveSig] int GetShareMode([MarshalAs(UnmanagedType.LPWStr)] string device, IntPtr mode);
    [PreserveSig] int SetShareMode([MarshalAs(UnmanagedType.LPWStr)] string device, IntPtr mode);
    [PreserveSig] int GetPropertyValue([MarshalAs(UnmanagedType.LPWStr)] string device, [MarshalAs(UnmanagedType.Bool)] bool fxStore, IntPtr key, IntPtr value);
    [PreserveSig] int SetPropertyValue([MarshalAs(UnmanagedType.LPWStr)] string device, [MarshalAs(UnmanagedType.Bool)] bool fxStore, IntPtr key, IntPtr value);
    [PreserveSig] int SetDefaultEndpoint([MarshalAs(UnmanagedType.LPWStr)] string device, int role);
    [PreserveSig] int SetEndpointVisibility([MarshalAs(UnmanagedType.LPWStr)] string device, [MarshalAs(UnmanagedType.Bool)] bool visible);
}
[ComImport, Guid("870AF99C-171D-4F9E-AF0D-E63DF40C2BC9")] public class CPolicyConfigClient { }

public static class Endpoints {
    // flow 0 = render, 1 = capture; role 0 = console, 1 = multimedia, 2 = communications.
    public static string DefaultId(int flow, int role) {
        var en = (IMMDeviceEnumerator)Activator.CreateInstance(typeof(MMDeviceEnumerator));
        IMMDevice d;
        if (en.GetDefaultAudioEndpoint(flow, role, out d) != 0 || d == null) return null;
        string id;
        return d.GetId(out id) == 0 ? id : null;
    }
    // Make the endpoint the default for all three roles; returns the first failing HRESULT or 0.
    public static int SetDefault(string id) {
        var p = (IPolicyConfig)Activator.CreateInstance(typeof(CPolicyConfigClient));
        for (int role = 0; role < 3; role++) {
            int hr = p.SetDefaultEndpoint(id, role);
            if (hr != 0) return hr;
        }
        return 0;
    }
}

public static class Wav {
    static void Header(BinaryWriter w, int dataBytes, int channels) {
        w.Write(Encoding.ASCII.GetBytes("RIFF")); w.Write(36 + dataBytes);
        w.Write(Encoding.ASCII.GetBytes("WAVEfmt ")); w.Write(16); w.Write((short)1);
        w.Write((short)channels); w.Write(48000); w.Write(48000 * 2 * channels);
        w.Write((short)(2 * channels)); w.Write((short)16);
        w.Write(Encoding.ASCII.GetBytes("data")); w.Write(dataBytes);
    }
    // 48 kHz 16-bit stereo tone; mask 1 = left, 2 = right, 3 = both; 20 ms fades.
    public static byte[] Tone(double freq, double seconds, int mask, double amplitude) {
        int frames = (int)(48000 * seconds), fade = 960;
        var ms = new MemoryStream();
        var w = new BinaryWriter(ms);
        Header(w, frames * 4, 2);
        for (int i = 0; i < frames; i++) {
            double env = Math.Min(1.0, Math.Min(i, frames - 1 - i) / (double)fade);
            short v = (short)(Math.Sin(2 * Math.PI * freq * i / 48000.0) * amplitude * env * 32767);
            w.Write((mask & 1) != 0 ? v : (short)0);
            w.Write((mask & 2) != 0 ? v : (short)0);
        }
        w.Flush();
        return ms.ToArray();
    }
    // "peak=-12.3 dBFS rms=-34.5 dBFS 5.0 s" for a 16-bit PCM file.
    public static string Analyze(string path) {
        byte[] b = File.ReadAllBytes(path);
        int pos = 12, channels = 1, rate = 48000;
        while (pos + 8 <= b.Length) {
            string id = Encoding.ASCII.GetString(b, pos, 4);
            int len = BitConverter.ToInt32(b, pos + 4);
            if (id == "fmt ") { channels = BitConverter.ToInt16(b, pos + 10); rate = BitConverter.ToInt32(b, pos + 12); }
            if (id == "data") {
                int end = Math.Min(b.Length, pos + 8 + len);
                double peak = 0, sum = 0; long n = 0;
                for (int i = pos + 8; i + 1 < end; i += 2) {
                    double v = BitConverter.ToInt16(b, i) / 32768.0;
                    peak = Math.Max(peak, Math.Abs(v)); sum += v * v; n++;
                }
                if (n == 0) return "empty recording";
                Func<double, string> db = x => x <= 0 ? "-inf" : (20 * Math.Log10(x)).ToString("0.0");
                return "peak=" + db(peak) + " dBFS rms=" + db(Math.Sqrt(sum / n)) + " dBFS " +
                       (n / (double)channels / rate).ToString("0.0") + " s";
            }
            pos += 8 + len + (len & 1);
        }
        return "no audio data";
    }
}

// Records from the default input device through winmm (MCI): simple and
// present on every Windows; 48 kHz 16-bit mono.
public static class Mci {
    [DllImport("winmm.dll", CharSet = CharSet.Unicode)]
    static extern int mciSendString(string command, StringBuilder ret, int retLength, IntPtr callback);
    static string Run(string cmd) {
        var sb = new StringBuilder(256);
        int e = mciSendString(cmd, sb, sb.Capacity, IntPtr.Zero);
        return e == 0 ? null : ("MCI error " + e + " on: " + cmd);
    }
    public static string Record(string path, int seconds) {
        Run("close t2rec");
        string err = Run("open new type waveaudio alias t2rec");
        if (err != null) return err;
        Run("set t2rec bitspersample 16 channels 1 samplespersec 48000 bytespersec 96000 alignment 2");
        err = Run("record t2rec");
        if (err == null) { Thread.Sleep(seconds * 1000); err = Run("stop t2rec"); }
        if (err == null) err = Run("save t2rec \"" + path + "\"");
        Run("close t2rec");
        return err;
    }
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
        Write-Info  'Run .\T2Audio-Setup.ps1 -Action Prepare (turns test mode on), reboot, then install.'
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

# Changing the boot configuration makes BitLocker ask for the recovery key at
# the next boot unless protection is suspended for one reboot.
function Suspend-BitLockerForBootChange {
    $bl = $null
    try { $bl = Get-BitLockerVolume -MountPoint $env:SystemDrive -ErrorAction Stop } catch { return $true }
    if (-not $bl -or $bl.ProtectionStatus -ne 'On') { return $true }
    Write-Warn "BitLocker protects $($env:SystemDrive): changing the boot settings would ask for the recovery key at the next boot."
    if (-not (Confirm-Action 'Suspend BitLocker for one reboot (it resumes by itself)?')) {
        Write-Info 'Not changed. Have the recovery key ready, or suspend BitLocker yourself first.'
        return $false
    }
    Suspend-BitLocker -MountPoint $env:SystemDrive -RebootCount 1 | Out-Null
    Write-Ok 'BitLocker suspended for one reboot.'
    return $true
}

function Get-SecureBootState {
    try { if (Confirm-SecureBootUEFI -ErrorAction Stop) { return 'on' } else { return 'off' } } catch { return 'unknown' }
}

# Test mode once per computer: the driver is test-signed (no Microsoft
# signature, owner's decision 2026-10-29).
function Invoke-Prepare {
    Write-Section 'Preparing Windows for a test-signed driver'
    $model = (Get-CimInstance Win32_ComputerSystem -ErrorAction SilentlyContinue).Model
    Write-Info "Model: $model"
    $sb = Get-SecureBootState
    if ($sb -eq 'on') {
        Write-Fail 'Secure Boot is on: Windows refuses test mode while it is.'
        Write-Info 'Turn it off on the Mac: restart into macOS Recovery (Cmd+R), Utilities > Startup Security Utility,'
        Write-Info 'Secure Boot: "No Security". Then come back to Windows and run -Action Prepare again.'
        return
    }
    Write-Ok "Secure Boot: $sb"
    if (Test-TestSigning) { Write-Ok 'Test mode is already on. Next: -Action Install.'; return }
    Write-Info 'Test mode lets Windows load drivers signed with a test certificate; Windows shows'
    Write-Info '"Test Mode" in a desktop corner while it is on. -Action Uninstall can turn it off again.'
    if (-not (Confirm-Action 'Turn test mode on?')) { return }
    if (-not (Suspend-BitLockerForBootChange)) { return }
    $out = & bcdedit.exe /set testsigning on 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { Write-Fail "bcdedit failed: $($out.Trim())"; return }
    Write-Ok 'Test mode on. Reboot Windows, then run: .\T2Audio-Setup.ps1 -Action Install -Preset macos'
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
        if (Test-TestSigning) {
            Write-Info 'Test mode is still on. Turn it off only if no other test-signed driver needs it.'
            if ((-not $Force) -and (Confirm-Action 'Turn test mode off (takes effect after a reboot)?') -and
                (Suspend-BitLockerForBootChange)) {
                $out = & bcdedit.exe /set testsigning off 2>&1 | Out-String
                if ($LASTEXITCODE -eq 0) { Write-Ok 'Test mode off after the next reboot.' } else { Write-Warn "bcdedit failed: $($out.Trim())" }
            }
        }
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


# ---------------------------------------------------------------------------
# Endpoints, switching, tests, backup, diagnostics
# ---------------------------------------------------------------------------
function Add-T2Code { if (-not ('T2Setup.Volume' -as [type])) { Add-Type -TypeDefinition $code } }

# All T2 endpoints Windows knows: Id, Flow (Render/Capture), Kind (Speakers /
# Headphones / Microphone / other), Name, State (Active, Unplugged, Disabled).
function Get-T2Endpoints {
    $hwProp = '{a8b865dd-2e3d-4094-ad97-e593a70c75d6},8'
    $descProp = '{a45c254e-df1c-4efd-8020-67d146a850e0},2'
    $nameProp = '{a45c254e-df1c-4efd-8020-67d146a850e0},14'
    $states = @{ 1 = 'Active'; 2 = 'Disabled'; 4 = 'Not present'; 8 = 'Unplugged' }
    foreach ($flow in 'Render', 'Capture') {
        $root = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\$flow"
        if (-not (Test-Path $root)) { continue }
        foreach ($k in Get-ChildItem $root) {
            $props = Join-Path $k.PSPath 'Properties'
            if (-not (Test-Path $props)) { continue }
            $all = Get-ItemProperty -Path $props -ErrorAction SilentlyContinue
            $hw = $all.$hwProp
            if (-not ($hw -and $hw.StartsWith($HardwareId))) { continue }
            $desc = "$($all.$descProp)"
            $st = (Get-ItemProperty -Path $k.PSPath -ErrorAction SilentlyContinue).DeviceState
            $kind = if ($flow -eq 'Capture') { 'Microphone' } elseif ($desc -match 'Headphone') { 'Headphones' } elseif ($desc -match 'Speaker') { 'Speakers' } else { $desc }
            [pscustomobject]@{
                Id    = $(if ($flow -eq 'Render') { '{0.0.0.00000000}.' } else { '{0.0.1.00000000}.' }) + $k.PSChildName
                Flow  = $flow
                Kind  = $kind
                Name  = "$($all.$nameProp)"
                State = $(if ($states.ContainsKey([int]$st)) { $states[[int]$st] } else { "state $st" })
            }
        }
    }
}

# Manual speakers <-> headphones switch until the jack is detected (ROADMAP 4.5).
function Invoke-Switch {
    Add-T2Code
    $eps = @(Get-T2Endpoints | Where-Object { $_.Flow -eq 'Render' -and $_.State -eq 'Active' })
    $spk = $eps | Where-Object Kind -eq 'Speakers' | Select-Object -First 1
    $hp = $eps | Where-Object Kind -eq 'Headphones' | Select-Object -First 1
    if (-not $spk) { Write-Fail 'The T2 speaker output is not active.'; return }
    if (-not $hp -and $To -ne 'Speakers') { Write-Fail 'No T2 headphone output: -Action Features -Headphones 1 first.'; return }
    $target = switch ($To) {
        'Speakers'   { $spk }
        'Headphones' { $hp }
        default      { if ([T2Setup.Endpoints]::DefaultId(0, 1) -eq $hp.Id) { $spk } else { $hp } }
    }
    $hr = [T2Setup.Endpoints]::SetDefault($target.Id)
    if ($hr -eq 0) { Write-Ok "Default output: $($target.Kind) ($($target.Name))" } else { Write-Fail ("Could not switch: 0x{0:X8}" -f $hr) }
}

# Desktop shortcut with a hotkey for Invoke-Switch (runs without admin rights).
function New-SwitchShortcut {
    $lnk = Join-Path ([Environment]::GetFolderPath('Desktop')) 'T2 Speakers - Headphones.lnk'
    $shell = New-Object -ComObject WScript.Shell
    $sc = $shell.CreateShortcut($lnk)
    $sc.TargetPath = Join-Path $PSHOME 'powershell.exe'
    $sc.Arguments = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$PSCommandPath`" -Action Switch -To Toggle"
    $sc.WorkingDirectory = $PSScriptRoot
    $sc.IconLocation = "$env:SystemRoot\System32\mmres.dll,0"
    $sc.Hotkey = 'Ctrl+Alt+H'
    $sc.Description = 'Switch the default output between the T2 speakers and headphones'
    $sc.Save()
    Write-Ok "Shortcut created: $lnk (hotkey Ctrl+Alt+H)"
}

function Invoke-SoundTest {
    Add-T2Code
    Write-Section 'Sound test'
    $def = [T2Setup.Endpoints]::DefaultId(0, 1)
    $t2 = @(Get-T2Endpoints | Where-Object { $_.Flow -eq 'Render' -and $_.State -eq 'Active' })
    $cur = $t2 | Where-Object Id -eq $def | Select-Object -First 1
    if (-not $cur) {
        Write-Warn 'The default output is not a T2 device.'
        $spk = $t2 | Where-Object Kind -eq 'Speakers' | Select-Object -First 1
        if (-not $spk -or -not (Confirm-Action 'Make the T2 speakers the default output for the test?')) { return }
        $null = [T2Setup.Endpoints]::SetDefault($spk.Id); $cur = $spk
    }
    Write-Info "Playing on: $($cur.Kind) ($($cur.Name)). Quiet tones, about -20 dBFS."
    foreach ($t in @(@('left', 1, 440), @('right', 2, 440), @('both', 3, 660))) {
        Write-Host ("   ♪ {0}" -f $t[0]) -ForegroundColor White
        $ms = New-Object System.IO.MemoryStream (, [T2Setup.Wav]::Tone($t[2], 1.2, $t[1], 0.1))
        (New-Object System.Media.SoundPlayer $ms).PlaySync()
        Start-Sleep -Milliseconds 300
    }
    Write-Ok 'Done. Left only on the left, right only on the right? If not: -Action Balance, then -Action Report.'
}

function Invoke-MicTest {
    Add-T2Code
    Write-Section 'Microphone test'
    $mic = Get-T2Endpoints | Where-Object { $_.Flow -eq 'Capture' -and $_.State -eq 'Active' } | Select-Object -First 1
    if (-not $mic) { Write-Fail 'No active T2 microphone: -Action Features -Microphone 1 first.'; return }
    if ([T2Setup.Endpoints]::DefaultId(1, 1) -ne $mic.Id) {
        if (-not (Confirm-Action "Make '$($mic.Name)' the default microphone for the test?")) { return }
        $null = [T2Setup.Endpoints]::SetDefault($mic.Id)
    }
    $wav = Join-Path $env:TEMP 'T2AudioPort-mictest.wav'
    Write-Info 'Recording 5 seconds - say something at a normal distance...'
    $err = [T2Setup.Mci]::Record($wav, 5)
    if ($err) { Write-Fail $err; return }
    $a = [T2Setup.Wav]::Analyze($wav)
    Write-Ok "Recorded: $a"
    if ($a -match 'peak=(-?[\d.]+|-inf)') {
        $peak = if ($Matches[1] -eq '-inf') { -999 } else { [double]$Matches[1] }
        if ($peak -lt -45) { Write-Warn 'Very quiet or silent: is "Microphone Array (Apple T2 …)" selected and not muted (Sound settings > Input)?' }
        elseif ($peak -gt -1) { Write-Warn 'Clipping: speak further away or lower the input level.' }
    }
    Write-Info 'Playing it back...'
    (New-Object System.Media.SoundPlayer $wav).PlaySync()
    Write-Info "File: $wav"
}

# Settings worth keeping: everything under Parameters except the crash guard,
# persistent-mode switch and saved Windows Update values.
function Get-SettingsFile { if ($File) { $File } else { Join-Path ([Environment]::GetFolderPath('Desktop')) 'T2AudioPort-settings.json' } }

function Invoke-Backup {
    Write-Section 'Backup settings'
    if (-not (Test-Path $ParamsPath)) { Write-Fail 'No settings yet.'; return }
    $item = Get-Item $ParamsPath
    $data = [ordered]@{ Saved = (Get-Date).ToString('s'); Driver = (Get-DeviceDriverVersion (Get-T2Device)); Values = [ordered]@{} }
    foreach ($n in ($item.GetValueNames() | Sort-Object)) {
        if ($n -match '^(CrashGuard|Saved|EnableProduction$)') { continue }
        $data.Values[$n] = [int](Get-EffectiveParam $n)
    }
    $path = Get-SettingsFile
    $data | ConvertTo-Json -Depth 4 | Set-Content -Path $path -Encoding UTF8
    Write-Ok "Saved $($data.Values.Count) values to $path"
}

function Invoke-Restore {
    Write-Section 'Restore settings'
    $path = Get-SettingsFile
    if (-not (Test-Path $path)) { Write-Fail "No file $path"; return }
    $data = Get-Content -Raw -Path $path | ConvertFrom-Json
    $names = @($data.Values.PSObject.Properties.Name)
    Write-Info "$($names.Count) values from $path (saved $($data.Saved), driver $($data.Driver))"
    if (-not (Confirm-Action 'Apply them?')) { return }
    foreach ($n in $names) {
        if ($n -match '^(CrashGuard|Saved|EnableProduction$)') { continue }
        Set-Param $n ([int]$data.Values.$n)
    }
    Write-Ok 'Applied.'
    $null = Restart-T2Device
}

# One screen that checks everything and says how to fix what is wrong.
function Invoke-Doctor {
    Add-T2Code
    Write-Section 'Doctor'
    $issues = 0
    function Bad($m, $fix) { Write-Fail $m; if ($fix) { Write-Info "fix: $fix" }; $script:doctorIssues++ }
    $script:doctorIssues = 0
    $model = (Get-CimInstance Win32_ComputerSystem -ErrorAction SilentlyContinue).Model
    if ($model -eq 'MacBookPro16,1') { Write-Ok "Model $model" } else { Bad "Model '$model' (the driver is tuned for MacBookPro16,1)" 'do not install on other Macs' }
    $os = Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue
    Write-Ok "Windows $($os.Caption) build $($os.BuildNumber)"
    $sb = Get-SecureBootState
    if ($sb -eq 'on') { Bad 'Secure Boot is on' 'macOS Recovery > Startup Security Utility > No Security' } else { Write-Ok "Secure Boot: $sb" }
    if (Test-TestSigning) { Write-Ok 'Test mode: on' } else { Bad 'Test mode is off' '-Action Prepare, then reboot' }
    try {
        $bl = Get-BitLockerVolume -MountPoint $env:SystemDrive -ErrorAction Stop
        Write-Ok "BitLocker on ${env:SystemDrive}: $($bl.ProtectionStatus) (keep the recovery key at hand before boot changes)"
    } catch { Write-Ok 'BitLocker: not in use' }
    $d = Get-T2Device
    if (-not $d) { Bad 'Apple T2 audio device not found' 'is this a T2 Mac with Boot Camp drivers?'; return }
    $svc = Get-DeviceService $d
    if ($svc -eq $ServiceName -and $d.Status -eq 'OK') { Write-Ok "Driver T2AudioPort $(Get-DeviceDriverVersion $d) running" }
    elseif ($svc -eq 'AppleAudio') { Bad 'The device runs AppleAudio' '-Action Install' }
    else { Bad "Device: $($d.Status), driver $svc" '-Action Report and look at the log' }
    if (Test-Path -LiteralPath $SysPath) {
        $sig = Get-AuthenticodeSignature -LiteralPath $SysPath
        if ($sig.SignerCertificate) {
            $trusted = Get-ChildItem Cert:\LocalMachine\TrustedPublisher -ErrorAction SilentlyContinue | Where-Object Thumbprint -eq $sig.SignerCertificate.Thumbprint
            if ($trusted) { Write-Ok 'Package certificate trusted' } else { Bad 'Package certificate not trusted' '-Action Install offers to trust it' }
        }
    }
    $strikes = Get-Param 'CrashGuardStrikes'
    if ($strikes -ge 2) { Bad "Crash guard tripped ($strikes)" 'find the cause (-Action Report), then -Action ResetGuard' }
    elseif ($strikes -ge 1) { Write-Warn 'One unclean end recorded (power loss, BSOD or hang)' } else { Write-Ok 'Crash guard: OK' }
    $eps = @(Get-T2Endpoints)
    foreach ($kind in 'Speakers', 'Headphones', 'Microphone') {
        $e = $eps | Where-Object Kind -eq $kind | Select-Object -First 1
        $feature = @{ Headphones = 'HeadphoneOutput'; Microphone = 'MicInput' }[$kind]
        $wanted = (-not $feature) -or ((Get-Param $feature) -eq 1)
        if ($e -and $e.State -eq 'Active') { Write-Ok "$kind endpoint: $($e.Name)" }
        elseif ($wanted) { Bad "$kind endpoint missing or $($e.State)" 'restart the device (-Action Preset -Preset macos) or reboot' }
        else { Write-Info "$kind endpoint: off (-Action Features)" }
    }
    $defOut = [T2Setup.Endpoints]::DefaultId(0, 1)
    $defIn = [T2Setup.Endpoints]::DefaultId(1, 1)
    $out = $eps | Where-Object Id -eq $defOut | Select-Object -First 1
    if ($out) { Write-Ok "Default output: $($out.Kind)" } else { Write-Warn 'The default output is not a T2 device (-Action Switch -To Speakers)' }
    $in = $eps | Where-Object Id -eq $defIn | Select-Object -First 1
    if ($in) { Write-Ok 'Default input: T2 microphone' } elseif ((Get-Param 'MicInput') -eq 1) { Write-Warn 'The default input is not the T2 microphone' }
    $spk = $eps | Where-Object { $_.Kind -eq 'Speakers' -and $_.State -eq 'Active' } | Select-Object -First 1
    if ($spk) {
        $v = [T2Setup.Volume]::Get($spk.Id)
        if ($v -match 'mute=True') { Bad 'The T2 speakers are muted' '-Action Balance -Volume 0.3' }
        elseif ($v -match 'L=([\d.,]+) R=([\d.,]+)' -and [math]::Abs([double]($Matches[1] -replace ',', '.') - [double]($Matches[2] -replace ',', '.')) -gt 0.02) {
            Bad 'Uneven L/R balance' '-Action Balance -Volume 0.3'
        } else { Write-Ok "Speaker volume: $v" }
    }
    $since = (Get-Date).AddDays(-7)
    $kp = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Microsoft-Windows-Kernel-Power'; Id = 41; StartTime = $since } -ErrorAction SilentlyContinue)
    if ($kp.Count) { Write-Warn "$($kp.Count) unexpected shutdown(s) in the last 7 days (Kernel-Power 41)" } else { Write-Ok 'No unexpected shutdowns in the last 7 days' }
    $dumps = @(Get-ChildItem "$env:SystemRoot\Minidump\*.dmp" -ErrorAction SilentlyContinue | Where-Object LastWriteTime -gt $since)
    if ($dumps.Count) { Write-Warn "$($dumps.Count) crash dump(s) in the last 7 days in $env:SystemRoot\Minidump (-Action Report lists them)" }
    if (Get-UpdateGuardOn) { Write-Info 'Windows Update driver guard: on' }
    if (Test-Path $TestSession) { Write-Warn 'A test-agent session (TestSession) is present - removed at reboot.' }
    Write-Host ''
    if ($script:doctorIssues -eq 0) { Write-Ok 'Everything looks fine.' } else { Write-Warn "$($script:doctorIssues) problem(s) - see the fixes above." }
}

# A zip with what a bug report needs: Doctor, status, settings, device and
# driver lists, recent T2/power events, the list of crash dumps (not the dumps).
function Invoke-Report {
    Write-Section 'Bug report'
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $dir = Join-Path $env:TEMP "T2AudioPort-report-$stamp"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Start-Transcript -Path (Join-Path $dir 'doctor-and-status.txt') | Out-Null
    try { Invoke-Doctor; Invoke-Status } finally { Stop-Transcript | Out-Null }
    & reg.exe export ($ParamsPath -replace '^HKLM:', 'HKLM') (Join-Path $dir 'parameters.reg') /y 2>&1 | Out-Null
    $d = Get-T2Device
    if ($d) { (Invoke-Pnp @('/enum-devices', '/instanceid', $d.InstanceId, '/drivers')).Out | Set-Content (Join-Path $dir 'device.txt') }
    Get-T2Endpoints | Format-Table -AutoSize | Out-String -Width 200 | Set-Content (Join-Path $dir 'endpoints.txt')
    $since = (Get-Date).AddDays(-7)
    Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $since } -ErrorAction SilentlyContinue |
        Where-Object { $_.ProviderName -eq 'Microsoft-Windows-Kernel-Power' -or $_.Message -match 'T2Audio|AppleAudio' } |
        Select-Object -First 200 TimeCreated, ProviderName, Id, LevelDisplayName, Message |
        Format-List | Out-String -Width 300 | Set-Content (Join-Path $dir 'events.txt')
    Get-ChildItem "$env:SystemRoot\Minidump\*.dmp" -ErrorAction SilentlyContinue |
        Select-Object Name, Length, LastWriteTime | Format-Table | Out-String | Set-Content (Join-Path $dir 'minidumps.txt')
    $setupapi = "$env:SystemRoot\INF\setupapi.dev.log"
    if (Test-Path $setupapi) {
        Select-String -Path $setupapi -Pattern 'T2AudioMiniport|VEN_106B&DEV_1803' -Context 0, 2 |
            Select-Object -Last 60 | Out-String -Width 300 | Set-Content (Join-Path $dir 'setupapi.txt')
    }
    $zip = Join-Path ([Environment]::GetFolderPath('Desktop')) "T2AudioPort-report-$stamp.zip"
    Compress-Archive -Path (Join-Path $dir '*') -DestinationPath $zip -Force
    Remove-Item -Recurse -Force $dir
    Write-Ok "Report: $zip"
    Write-Info 'It contains your computer name and device list, no personal files and no crash dumps.'
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
        Write-Section 'Setup'
        Write-Host '   1. Prepare Windows (test mode, once)      2. Install / update      3. Restore AppleAudio' -ForegroundColor White
        Write-Section 'Sound'
        Write-Host '   4. Preset      5. Tone      6. Optional features (headphones, microphone, sleep, formats)' -ForegroundColor White
        Write-Host '   7. Switch speakers <-> headphones      8. Sound test      9. Microphone test' -ForegroundColor White
        Write-Section 'Maintenance'
        Write-Host '   D. Doctor      R. Bug report      B. Backup settings      L. Load settings' -ForegroundColor White
        Write-Host '   K. Switch shortcut (Ctrl+Alt+H)      G. Windows Update guard      C. Reset crash guard' -ForegroundColor White
        Write-Host '   0. Exit' -ForegroundColor White
        $c = Read-Host "`n  Choice"
        switch ($c) {
            '1' { Invoke-Prepare }
            '2' { $script:Preset = Select-PresetInteractive; Invoke-Install }
            '3' { Invoke-Uninstall }
            '4' { $script:Preset = Select-PresetInteractive; Invoke-Preset }
            '5' { Invoke-Tune -Interactive }
            '6' { Invoke-Features -Interactive }
            '7' { $script:To = 'Toggle'; Invoke-Switch }
            '8' { Invoke-SoundTest }
            '9' { Invoke-MicTest }
            'd' { Invoke-Doctor }
            'r' { Invoke-Report }
            'b' { Invoke-Backup }
            'l' { Invoke-Restore }
            'k' { New-SwitchShortcut }
            'g' {
                $a = Read-Host '  1 = guard on, 0 = guard off, Enter = show'
                if ($a -eq '0' -or $a -eq '1') { $script:Block = [int]$a }
                Invoke-UpdateGuard
                $script:Block = $null
            }
            'c' { Invoke-ResetGuard }
            '0' { return }
            default { Write-Warn 'No such item.' }
        }
        Read-Host "`n  Enter - back to the menu" | Out-Null
    }
}

# ---------------------------------------------------------------------------
# Switching outputs and the tests only touch the user's audio settings; all
# other actions change drivers or HKLM and need an elevated PowerShell.
if ($Action -notin @('Switch', 'SoundTest', 'MicTest', 'Shortcut') -and -not (Test-Admin)) {
    Write-Fail 'Run PowerShell as administrator.'; exit 1
}
switch ($Action) {
    'Menu'       { Show-Menu }
    'Prepare'    { Write-Banner; Invoke-Prepare }
    'Install'    { Write-Banner; Invoke-Install }
    'Preset'     { Write-Banner; Invoke-Preset }
    'Tune'       { Write-Banner; Invoke-Tune }
    'Features'   { Write-Banner; Invoke-Features }
    'UpdateGuard' { Write-Banner; Invoke-UpdateGuard }
    'Balance'    { Write-Banner; Write-Section 'Balance'; $v = if ($Volume -ge 0) { $Volume } else { 0.3 }; Set-T2Volume $v }
    'Status'     { Write-Banner; Invoke-Status }
    'Doctor'     { Write-Banner; Invoke-Doctor }
    'Switch'     { Invoke-Switch }
    'Shortcut'   { New-SwitchShortcut }
    'SoundTest'  { Write-Banner; Invoke-SoundTest }
    'MicTest'    { Write-Banner; Invoke-MicTest }
    'Backup'     { Write-Banner; Invoke-Backup }
    'Restore'    { Write-Banner; Invoke-Restore }
    'Report'     { Write-Banner; Invoke-Report }
    'ResetGuard' { Write-Banner; Invoke-ResetGuard }
    'Uninstall'  { Write-Banner; Invoke-Uninstall }
}
