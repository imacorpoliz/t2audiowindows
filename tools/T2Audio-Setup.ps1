# T2AudioPort - установщик и настройка звука (MacBookPro16,1).
#
# Меню (запуск без параметров) или параметры:
#   .\T2Audio-Setup.ps1                                  # интерактивное меню
#   .\T2Audio-Setup.ps1 -Action Install -Preset macos    # установить, постоянный режим
#   .\T2Audio-Setup.ps1 -Action Preset  -Preset bright   # сменить пресет звука
#   .\T2Audio-Setup.ps1 -Action Tune -BassDb 3 -ClarityDb 2  # тембр поверх пресета
#   .\T2Audio-Setup.ps1 -Action Balance -Volume 0.3      # выровнять баланс L/R
#   .\T2Audio-Setup.ps1 -Action Status
#   .\T2Audio-Setup.ps1 -Action ResetGuard               # сбросить защиту от падений
#   .\T2Audio-Setup.ps1 -Action Uninstall                # вернуть AppleAudio
#
# Постоянный режим: драйвер стартует при каждой загрузке Windows
# (Parameters\EnableProduction=1) под защитой от падений: если две сессии
# подряд оборвались без штатного завершения (BSOD, зависание, выключение
# кнопкой), драйвер сам перестаёт стартовать до сброса этим скриптом.
# В безопасном режиме драйвер не стартует никогда.
#
# Нужны права администратора, включённый testsigning и подписанный пакет
# в packaging\ (T2AudioMiniport.inf/.sys/.cat), как его собирает GEMINI.md §2.
# Файл сохранён в UTF-8 с BOM (иначе Windows PowerShell 5.1 искажает кириллицу).

[CmdletBinding()]
param(
    [ValidateSet('Menu', 'Install', 'Preset', 'Tune', 'Balance', 'Status', 'ResetGuard', 'Uninstall')]
    [string]$Action = 'Menu',

    [ValidateSet('macos', 'macos-precise', 'bright', 'loud', 'maxvolume', 'even', 'bass', 'voice', 'movie', 'night', 'warm', 'natural', 'raw')]
    [string]$Preset = 'macos',

    # -Action Tune: тембр в дБ (-12..+12) поверх текущего пресета; не указан = не менять.
    # Бас = полка 150 Гц, высокие = полка 6 кГц, разборчивость = пик 2,5 кГц.
    [ValidateRange(-12, 12)] [Nullable[int]]$BassDb,
    [ValidateRange(-12, 12)] [Nullable[int]]$TrebleDb,
    [ValidateRange(-12, 12)] [Nullable[int]]$ClarityDb,
    # Компрессор: 0 = быстрый пиковый (как раньше), 1 = точный как в macOS (RMS, мягкое колено).
    [ValidateRange(0, 1)] [Nullable[int]]$CompStyle,

    # Громкость точки T2 после установки (0..1); -1 = не трогать.
    [double]$Volume = 0.3,

    # Без вопросов-подтверждений.
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
# Пресеты звука. Значения - DWORD в Parameters; отсутствующее значение =
# значение по умолчанию в драйвере. Каждый пресет перечисляет только отличия
# от значений по умолчанию (= 'macos'); при установке пишутся все значения.
# Проверен на железе владельцем только 'macos' (2026-10-28, 1.0.11.0);
# остальные - варианты для подбора на слух. BassDb/TrebleDb/ClarityDb/
# CompStyle понимает драйвер начиная с 1.0.12.0 (старый их просто не читает).
# ---------------------------------------------------------------------------
$PresetValueNames = @('SpeakerDsp', 'DspGainDb', 'TweeterTrimDb', 'WooferLimitDb', 'DspAutoGain',
                      'BassDb', 'TrebleDb', 'ClarityDb', 'CompStyle')
# Значения драйвера по умолчанию (если в реестре значения нет).
$ParamDefaults = @{ SpeakerDsp = 3; DspGainDb = 6; TweeterTrimDb = 0; WooferLimitDb = 0; DspAutoGain = 0;
                    BassDb = 0; TrebleDb = 0; ClarityDb = 0; CompStyle = 0 }

function New-PresetValues([hashtable]$changes) {
    $v = $ParamDefaults.Clone()
    foreach ($k in $changes.Keys) { $v[$k] = $changes[$k] }
    return $v
}

$Presets = [ordered]@{
    'macos'         = @{ Title = 'Как на Mac (рекомендуется)';
                         Text  = 'Эквалайзер Apple для 6 динамиков, виртуальный бас, многополосный компрессор.';
                         Values = (New-PresetValues @{ }); Verified = $true }
    'macos-precise' = @{ Title = 'Как на Mac, точный компрессор';
                         Text  = 'То же, но компрессор считает уровень как macOS (RMS, мягкое колено): мягче на пиках, ровнее на басе.';
                         Values = (New-PresetValues @{ CompStyle = 1 }); Verified = $false }
    'bright'        = @{ Title = 'Ярче';
                         Text  = 'Как на Mac, но твитеры на 3 дБ громче: больше «воздуха» и деталей.';
                         Values = (New-PresetValues @{ TweeterTrimDb = 3 }); Verified = $false }
    'loud'          = @{ Title = 'Громче';
                         Text  = 'Сильнее компенсация громкости и защита вуферов от дребезга; лимитер держит пики.';
                         Values = (New-PresetValues @{ SpeakerDsp = 4; DspGainDb = 9; WooferLimitDb = 3 }); Verified = $false }
    'maxvolume'     = @{ Title = 'Максимальная громкость без гула';
                         Text  = 'Для громкой музыки с басом: вуферы ограничены на 6 дБ ниже, защита от дребезга.';
                         Values = (New-PresetValues @{ SpeakerDsp = 4; WooferLimitDb = 6 }); Verified = $false }
    'even'          = @{ Title = 'Ровная громкость';
                         Text  = 'Как на Mac плюс автоусиление по громкости (LUFS): тихие записи до +6 дБ, громкие до -12 дБ, медленно.';
                         Values = (New-PresetValues @{ DspAutoGain = 1 }); Verified = $false }
    'bass'          = @{ Title = 'Больше баса';
                         Text  = 'Бас +4 дБ (полка 150 Гц) с защитой вуферов от дребезга. Для музыки на средней громкости.';
                         Values = (New-PresetValues @{ SpeakerDsp = 4; WooferLimitDb = 2; BassDb = 4; CompStyle = 1 }); Verified = $false }
    'voice'         = @{ Title = 'Голос (подкасты, звонки)';
                         Text  = 'Разборчивость +4 дБ (2,5 кГц), бас -3 дБ, ровная громкость.';
                         Values = (New-PresetValues @{ BassDb = -3; ClarityDb = 4; DspAutoGain = 1 }); Verified = $false }
    'movie'         = @{ Title = 'Кино';
                         Text  = 'Ровная громкость, диалоги +2 дБ, бас +2 дБ, точный компрессор.';
                         Values = (New-PresetValues @{ BassDb = 2; ClarityDb = 2; DspAutoGain = 1; CompStyle = 1 }); Verified = $false }
    'night'         = @{ Title = 'Ночь (тихо и ровно)';
                         Text  = 'Меньше баса и пиков, ровная громкость: не будит соседей, речь остаётся понятной.';
                         Values = (New-PresetValues @{ SpeakerDsp = 4; DspGainDb = 3; WooferLimitDb = 6; DspAutoGain = 1; BassDb = -4; ClarityDb = 2; CompStyle = 1 }); Verified = $false }
    'warm'          = @{ Title = 'Мягкий';
                         Text  = 'Эквалайзер Mac и виртуальный бас, без компрессора: больше динамики, чуть тише.';
                         Values = (New-PresetValues @{ SpeakerDsp = 2 }); Verified = $false }
    'natural'       = @{ Title = 'Естественный';
                         Text  = 'Только эквалайзер и разделение на динамики Mac, без виртуального баса и компрессора.';
                         Values = (New-PresetValues @{ SpeakerDsp = 1 }); Verified = $false }
    'raw'           = @{ Title = 'Без обработки (для сравнения)';
                         Text  = 'Сырой путь: вуферы на полном диапазоне, твитеры через ФВЧ 3 кГц. Звучит плоско, баса мало.';
                         Values = (New-PresetValues @{ SpeakerDsp = 0 }); Verified = $false }
}

# ---------------------------------------------------------------------------
# Вывод
# ---------------------------------------------------------------------------
function Write-Banner {
    Write-Host ''
    Write-Host '  ╔══════════════════════════════════════════════════════════╗' -ForegroundColor Cyan
    Write-Host '  ║          T2AudioPort  ·  звук Apple T2 для Windows       ║' -ForegroundColor Cyan
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
    $a = Read-Host "  $prompt [д/н]"
    return ($a -match '^(д|да|y|yes)$')
}

# ---------------------------------------------------------------------------
# Системные проверки
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
# Громкость точки T2 (Windows хранит громкость и Mute для каждой точки;
# тестовые прогоны оставляли точку T2 заглушённой).
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
    foreach ($k in Get-ChildItem $root) {
        $st = (Get-ItemProperty -Path $k.PSPath -ErrorAction SilentlyContinue).DeviceState
        if ($st -ne 1) { continue }
        $props = Join-Path $k.PSPath 'Properties'
        if (-not (Test-Path $props)) { continue }
        $v = (Get-ItemProperty -Path $props -ErrorAction SilentlyContinue).$prop
        if ($v -and $v.StartsWith($HardwareId)) { return '{0.0.0.00000000}.' + $k.PSChildName }
    }
    return $null
}

function Set-T2Volume([double]$level) {
    if ($level -lt 0) { return }
    $ep = $null
    for ($i = 0; $i -lt 10 -and -not $ep; $i++) { $ep = Get-T2Endpoint; if (-not $ep) { Start-Sleep -Seconds 1 } }
    if (-not $ep) { Write-Warn 'Точка T2 ещё не появилась в Windows; громкость не изменена.'; return }
    try {
        if (-not ('T2Setup.Volume' -as [type])) { Add-Type -TypeDefinition $code }
        $r = [T2Setup.Volume]::Set($ep, [float]$level)
        Write-Ok "Динамики T2: Mute снят, громкость $([int]($level * 100)) %, баланс L/R выровнен ($r)"
    } catch {
        Write-Warn "Не удалось выставить громкость: $_"
    }
}

# ---------------------------------------------------------------------------
# Действия
# ---------------------------------------------------------------------------
function Show-Presets {
    Write-Section 'Пресеты звука'
    $i = 1
    foreach ($k in $Presets.Keys) {
        $p = $Presets[$k]
        $mark = if ($p.Verified) { ' ★ проверен' } else { '' }
        Write-Host ("   {0}. {1,-34}{2}" -f $i, $p.Title, $mark) -ForegroundColor White
        Write-Host ("      {0}" -f $p.Text) -ForegroundColor DarkGray
        $i++
    }
}

function Select-PresetInteractive {
    Show-Presets
    $keys = @($Presets.Keys)
    $a = Read-Host "  Номер пресета (Enter = 1)"
    if (-not $a) { return $keys[0] }
    $n = 0
    if ([int]::TryParse($a, [ref]$n) -and $n -ge 1 -and $n -le $keys.Count) { return $keys[$n - 1] }
    Write-Warn 'Нет такого номера; выбран «Как на Mac».'
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
    Write-Ok "Пресет «$($p.Title)»: $(($PresetValueNames | ForEach-Object { "$_=$($p.Values[$_])" }) -join ', ')"
}

# Текущее значение параметра с учётом значения по умолчанию (DWORD читается
# как Int32, поэтому -3 дБ возвращается как -3).
function Get-EffectiveParam($name) {
    $v = Get-Param $name
    if ($null -eq $v) { return [int]$ParamDefaults[$name] }
    if ($v -gt [int]::MaxValue) { return [int]($v - 0x100000000) }
    return [int]$v
}

function Format-Db([int]$v) { if ($v -gt 0) { "+$v" } else { "$v" } }

function Restart-T2Device {
    $d = Get-T2Device
    if (-not $d) { Write-Fail 'Устройство T2 не найдено.'; return $false }
    if ((Get-DeviceService $d) -ne $ServiceName) {
        Write-Info 'Драйвер T2AudioPort сейчас не активен - настройка применится после установки.'
        return $true
    }
    Write-Info 'Перезапуск устройства, чтобы драйвер прочитал настройки (звук пропадёт на пару секунд)...'
    $r = Invoke-Pnp @('/restart-device', $d.InstanceId)
    $d = Wait-DeviceService $ServiceName
    if ($d -and $d.Status -eq 'OK') { Write-Ok 'Устройство перезапущено.'; return $true }
    Write-Warn "Устройство после перезапуска: $($d.Status) (pnputil код $($r.Code)). Возможно, нужна перезагрузка."
    return $false
}

function Invoke-Status {
    Write-Section 'Состояние'
    $d = Get-T2Device
    if (-not $d) { Write-Fail 'Аудиоустройство Apple T2 (VEN_106B&DEV_1803) не найдено.'; return }
    $svc = Get-DeviceService $d
    $ver = Get-DeviceDriverVersion $d
    $drv = switch ($svc) { 'T2AudioMiniport' { 'T2AudioPort' } 'AppleAudio' { 'AppleAudio (драйвер Apple)' } default { "$svc" } }
    $color = if ($d.Status -eq 'OK') { 'Green' } else { 'Yellow' }
    Write-Host ("   Драйвер:        {0}  {1}" -f $drv, $ver) -ForegroundColor White
    Write-Host ("   Устройство:     {0}" -f $d.Status) -ForegroundColor $color
    $prod = Get-Param 'EnableProduction'
    Write-Host ("   Постоянный режим: {0}" -f $(if ($prod -eq 1) { 'включён' } else { 'выключен' })) -ForegroundColor White
    $cur = Get-CurrentPresetName
    $pn = if ($cur) { $Presets[$cur].Title } else { 'свои значения / по умолчанию' }
    Write-Host ("   Пресет:         {0}" -f $pn) -ForegroundColor White
    Write-Host ("   Тембр:          бас {0} дБ, высокие {1} дБ, разборчивость {2} дБ, компрессор {3}" -f `
        (Format-Db (Get-EffectiveParam 'BassDb')), (Format-Db (Get-EffectiveParam 'TrebleDb')), `
        (Format-Db (Get-EffectiveParam 'ClarityDb')), $(if ((Get-EffectiveParam 'CompStyle') -eq 1) { 'точный (как macOS)' } else { 'быстрый' })) -ForegroundColor White
    $strikes = Get-Param 'CrashGuardStrikes'; $armed = Get-Param 'CrashGuardArmed'
    if ($strikes -ge 2) {
        Write-Fail "Защита от падений СРАБОТАЛА ($strikes обрыва подряд): драйвер не стартует. Пункт «Сбросить защиту» после разбора причины."
    } elseif ($strikes -ge 1) {
        Write-Warn "Прошлая сессия завершилась нештатно (обрывов подряд: $strikes)."
    } else {
        Write-Host '   Защита от падений: в порядке' -ForegroundColor White
    }
    $ep = Get-T2Endpoint
    if ($ep) {
        try {
            if (-not ('T2Setup.Volume' -as [type])) { Add-Type -TypeDefinition $code }
            $vol = [T2Setup.Volume]::Get($ep)
            Write-Host ("   Громкость T2:   {0}" -f $vol) -ForegroundColor White
            if ($vol -match 'L=([\d.,]+) R=([\d.,]+)') {
                $l = [double]($Matches[1] -replace ',', '.'); $r = [double]($Matches[2] -replace ',', '.')
                if ([math]::Abs($l - $r) -gt 0.02) { Write-Warn 'Баланс L/R неравный (Параметры звука, Уровни, Баланс). Выровнять: .\T2Audio-Setup.ps1 -Action Balance' }
            }
        } catch { }
    }
    $ts = if (Test-TestSigning) { 'включён' } else { 'ВЫКЛЮЧЕН' }
    Write-Host ("   testsigning:    {0}" -f $ts) -ForegroundColor White
    if (Test-Path $TestSession) { Write-Warn 'Есть тестовая сессия агента (TestSession) - она удалится при перезагрузке.' }
}

function Invoke-Install {
    Write-Section 'Установка T2AudioPort'
    $model = (Get-CimInstance Win32_ComputerSystem -ErrorAction SilentlyContinue).Model
    if ($model -and $model -ne 'MacBookPro16,1') {
        Write-Warn "Модель $model, а DSP настроен для MacBookPro16,1."
        if (-not (Confirm-Action 'Всё равно продолжить?')) { return }
    }
    if (-not (Test-TestSigning)) {
        Write-Fail 'testsigning выключен: тестовую подпись драйвера Windows не примет.'
        Write-Info  'Включение: bcdedit /set testsigning on, затем перезагрузка (Secure Boot должен быть выключен).'
        return
    }
    foreach ($f in @($InfPath, $SysPath, $CatPath)) {
        if (-not (Test-Path -LiteralPath $f)) { Write-Fail "Нет файла пакета: $f (соберите и подпишите по GEMINI.md §2)"; return }
    }
    $sig = Get-AuthenticodeSignature -LiteralPath $SysPath
    if (-not $sig.SignerCertificate) { Write-Fail 'T2AudioMiniport.sys не подписан.'; return }
    $hash = (Get-FileHash -LiteralPath $SysPath -Algorithm SHA256).Hash
    $infVer = (Select-String -LiteralPath $InfPath -Pattern '^DriverVer\s*=\s*(.+)$').Matches | Select-Object -First 1
    Write-Ok "Пакет: $(if ($infVer) { $infVer.Groups[1].Value } else { '?' }), SHA256 $($hash.Substring(0, 16))…"

    $d = Get-T2Device
    if (-not $d) { Write-Fail 'Аудиоустройство Apple T2 не найдено.'; return }

    Write-Host ''
    Write-Warn 'Драйвер экспериментальный. После установки он будет стартовать при каждой загрузке.'
    Write-Info 'Если две сессии подряд оборвутся (BSOD, зависание), драйвер сам перестанет стартовать;'
    Write-Info 'вернуть Apple-драйвер можно этим же скриптом (пункт «Вернуть AppleAudio») или в безопасном режиме.'
    if (-not $Force) {
        $a = Read-Host "  Напишите УСТАНОВИТЬ для продолжения"
        if ($a -ne 'УСТАНОВИТЬ') { Write-Warn 'Отменено.'; return }
    }

    if (Test-Path $TestSession) { Remove-Item -Path $TestSession -Recurse -Force; Write-Info 'Тестовая сессия агента удалена.' }
    Unregister-ScheduledTask -TaskName 'T2Audio-AutoCapture-AfterBoot' -Confirm:$false -ErrorAction SilentlyContinue

    # Настройки до привязки: драйвер читает их при старте.
    Write-PresetValues $Preset
    Set-Param 'EnableProduction' 1
    Set-Param 'CrashGuardArmed' 0
    Set-Param 'CrashGuardStrikes' 0
    Write-Ok 'Постоянный режим включён, защита от падений сброшена.'

    # Отключение устройства перед привязкой избавляет от Code 14 (перезагрузки).
    Write-Info 'Привязка драйвера к устройству...'
    $null = Invoke-Pnp @('/disable-device', $d.InstanceId)
    $r = Invoke-Pnp @('/add-driver', $InfPath, '/install')
    $null = Invoke-Pnp @('/enable-device', $d.InstanceId)
    if ($r.Code -ne 0 -and $r.Code -ne 3010) {
        Write-Fail "pnputil /add-driver вернул $($r.Code)."
        Write-Info ($r.Out.Trim())
        return
    }
    $d = Wait-DeviceService $ServiceName 30
    if ($d -and $d.Status -eq 'OK' -and (Get-DeviceService $d) -eq $ServiceName) {
        Write-Ok 'T2AudioPort установлен и работает.'
        Set-T2Volume $Volume
        Write-Host ''
        Write-Info 'Выберите «Динамики» Apple T2 устройством вывода в параметрах звука, если Windows не сделала этого сама.'
        Write-Info 'Пресет можно сменить в любой момент: .\T2Audio-Setup.ps1 -Action Preset -Preset bright'
    } elseif ($r.Code -eq 3010) {
        Write-Warn 'Windows просит перезагрузку, чтобы завершить установку.'
    } else {
        Write-Fail "Устройство не запустилось: $($d.Status), драйвер $(Get-DeviceService $d)."
        Write-Info 'Проверьте testsigning и подпись; откат - пункт «Вернуть AppleAudio».'
    }
}

function Invoke-Preset {
    Write-Section 'Смена пресета'
    Write-PresetValues $Preset
    $null = Restart-T2Device
}

function Read-DbInteractive($label, $name) {
    $cur = Get-EffectiveParam $name
    $a = Read-Host ("  {0}, дБ от -12 до +12 (сейчас {1}; Enter = не менять)" -f $label, (Format-Db $cur))
    if (-not $a) { return $null }
    $n = 0
    if ([int]::TryParse($a.Trim().TrimStart('+'), [ref]$n) -and $n -ge -12 -and $n -le 12) { return $n }
    Write-Warn 'Нужно целое число от -12 до +12; не меняю.'
    return $null
}

# Тембр поверх текущего пресета: меняет только указанные значения.
function Invoke-Tune([switch]$Interactive) {
    Write-Section 'Тембр'
    $changes = [ordered]@{}
    if ($Interactive) {
        foreach ($pair in @(@('Бас (150 Гц)', 'BassDb'), @('Высокие (6 кГц)', 'TrebleDb'), @('Разборчивость (2,5 кГц)', 'ClarityDb'))) {
            $v = Read-DbInteractive $pair[0] $pair[1]
            if ($null -ne $v) { $changes[$pair[1]] = $v }
        }
        $c = Read-Host '  Компрессор: 0 = быстрый, 1 = точный как macOS (Enter = не менять)'
        if ($c -eq '0' -or $c -eq '1') { $changes['CompStyle'] = [int]$c }
    } else {
        if ($null -ne $BassDb)    { $changes['BassDb'] = $BassDb }
        if ($null -ne $TrebleDb)  { $changes['TrebleDb'] = $TrebleDb }
        if ($null -ne $ClarityDb) { $changes['ClarityDb'] = $ClarityDb }
        if ($null -ne $CompStyle) { $changes['CompStyle'] = $CompStyle }
    }
    if ($changes.Count -eq 0) {
        Write-Info 'Ничего не изменено. Пример: .\T2Audio-Setup.ps1 -Action Tune -BassDb 3 -TrebleDb -2'
        return
    }
    foreach ($k in $changes.Keys) { Set-Param $k $changes[$k] }
    Write-Ok "Тембр: $(($changes.Keys | ForEach-Object { "$_=$($changes[$_])" }) -join ', ')"
    $null = Restart-T2Device
}

function Invoke-ResetGuard {
    Write-Section 'Сброс защиты от падений'
    $s = Get-Param 'CrashGuardStrikes'
    Write-Info "Обрывов подряд сейчас: $(if ($null -eq $s) { 0 } else { $s })"
    if (-not (Confirm-Action 'Сбросить и разрешить драйверу стартовать?')) { return }
    Set-Param 'CrashGuardArmed' 0
    Set-Param 'CrashGuardStrikes' 0
    Write-Ok 'Сброшено.'
    $null = Restart-T2Device
}

function Invoke-Uninstall {
    Write-Section 'Возврат драйвера Apple (AppleAudio)'
    if (-not (Confirm-Action 'Удалить T2AudioPort и вернуть AppleAudio?')) { return }
    if (Test-Path $ParamsPath) { Set-Param 'EnableProduction' 0 }
    if (Test-Path $TestSession) { Remove-Item -Path $TestSession -Recurse -Force }
    $infs = @(Get-OemInfsForT2)
    if ($infs.Count -eq 0) { Write-Info 'Пакет T2AudioPort в хранилище драйверов не найден.' }
    foreach ($inf in $infs) {
        $r = Invoke-Pnp @('/delete-driver', $inf, '/uninstall', '/force')
        if ($r.Code -eq 0) { Write-Ok "Удалён $inf" } else { Write-Warn "pnputil /delete-driver ${inf}: код $($r.Code)" }
    }
    $null = Invoke-Pnp @('/scan-devices')
    $d = Wait-DeviceService 'AppleAudio' 30
    if ($d -and (Get-DeviceService $d) -eq 'AppleAudio' -and $d.Status -eq 'OK') {
        Write-Ok 'Устройство снова на AppleAudio.'
    } else {
        Write-Warn "Сейчас: $($d.Status), драйвер $(Get-DeviceService $d)."
        Write-Info 'Если AppleAudio не вернулся сам: перезагрузка или .\Restore-AppleAudioDriver.ps1 (скачивает драйвер из Boot Camp).'
    }
}

function Show-Menu {
    while ($true) {
        Write-Banner
        Invoke-Status
        Write-Section 'Что сделать?'
        Write-Host '   1. Установить / обновить T2AudioPort' -ForegroundColor White
        Write-Host '   2. Сменить пресет звука' -ForegroundColor White
        Write-Host '   3. Сбросить защиту от падений' -ForegroundColor White
        Write-Host '   4. Вернуть драйвер Apple (удалить T2AudioPort)' -ForegroundColor White
        Write-Host '   5. Подстроить тембр (бас / высокие / разборчивость)' -ForegroundColor White
        Write-Host '   0. Выход' -ForegroundColor White
        $c = Read-Host "`n  Выбор"
        switch ($c) {
            '1' { $script:Preset = Select-PresetInteractive; Invoke-Install }
            '2' { $script:Preset = Select-PresetInteractive; Invoke-Preset }
            '3' { Invoke-ResetGuard }
            '4' { Invoke-Uninstall }
            '5' { Invoke-Tune -Interactive }
            '0' { return }
            default { Write-Warn 'Нет такого пункта.' }
        }
        Read-Host "`n  Enter - в меню" | Out-Null
    }
}

# ---------------------------------------------------------------------------
if (-not (Test-Admin)) { Write-Fail 'Запустите PowerShell от имени администратора.'; exit 1 }
switch ($Action) {
    'Menu'       { Show-Menu }
    'Install'    { Write-Banner; Invoke-Install }
    'Preset'     { Write-Banner; Invoke-Preset }
    'Tune'       { Write-Banner; Invoke-Tune }
    'Balance'    { Write-Banner; Write-Section 'Баланс'; $v = if ($Volume -ge 0) { $Volume } else { 0.3 }; Set-T2Volume $v }
    'Status'     { Write-Banner; Invoke-Status }
    'ResetGuard' { Write-Banner; Invoke-ResetGuard }
    'Uninstall'  { Write-Banner; Invoke-Uninstall }
}
