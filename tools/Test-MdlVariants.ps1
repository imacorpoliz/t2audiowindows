# T2AudioPort - MMIO MDL variant experiment harness.
#
# For each MdlVariant value: set the registry override, restart the device,
# discover the active T2 render endpoint, capture kernel debug output, run the
# WASAPI probe, and print the HRESULTs plus the relevant driver log lines.
# No reboot required; the device restart reloads the driver and re-reads
# HKLM\SYSTEM\CurrentControlSet\Services\T2AudioMiniport\Parameters.

param(
    [int[]]$Variants = @(0, 1, 2, 3, 4),
    [string]$InstanceId = 'PCI\VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01\4&3ac8fc3&0&03D8',
    [string]$HwId = 'PCI\VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01'
)

$ErrorActionPreference = 'Stop'

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$DbgView = @(
    $env:T2AUDIO_DBGVIEW,
    (Join-Path (Split-Path -Parent $ProjectRoot) 'DebugView\DbgViewCLI.exe'),
    'C:\Program Files\DebugView\DbgViewCLI.exe'
) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (-not $DbgView) { throw 'DbgViewCLI.exe not found (set $env:T2AUDIO_DBGVIEW)' }
$Probe = Join-Path $PSScriptRoot 'Test-WasapiRender.ps1'
$regPath = 'HKLM:\SYSTEM\CurrentControlSet\Services\T2AudioMiniport\Parameters'
if (-not (Test-Path $regPath)) { New-Item -Path $regPath -Force | Out-Null }

$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$outDir = Join-Path $ProjectRoot "docs\logs\mdlvariants_$stamp"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
Write-Output "output: $outDir"

function Get-T2Endpoint {
    param([string]$Hw)
    $root = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
    foreach ($k in Get-ChildItem $root) {
        $props = Join-Path $k.PSPath 'Properties'
        $hw = (Get-ItemProperty -Path $props -ErrorAction SilentlyContinue).'{a8b865dd-2e3d-4094-ad97-e593a70c75d6},8'
        if ($hw -eq $Hw) {
            $state = (Get-ItemProperty -Path $k.PSPath -ErrorAction SilentlyContinue).DeviceState
            if ($state -eq 1) { return $k.PSChildName }
        }
    }
    return $null
}

foreach ($v in $Variants) {
    Set-ItemProperty -Path $regPath -Name 'MdlVariant' -Value $v -Type DWord
    Write-Output "=== Variant $v : restarting device ==="
    pnputil /restart-device "$InstanceId" | Out-Null
    Start-Sleep -Seconds 3

    $guid = Get-T2Endpoint -Hw $HwId
    if (-not $guid) { Write-Output "  no active T2 endpoint found"; continue }
    $deviceId = "{0.0.0.00000000}.$guid"
    $kernelLog = Join-Path $outDir "variant${v}_kernel.csv"
    $wasapiLog = Join-Path $outDir "variant${v}_wasapi.txt"

    $proc = Start-Process -FilePath $DbgView -PassThru -WindowStyle Hidden -ArgumentList @(
        '--kernel', '--no-win32', '--duration', '12', '--format', 'csv',
        '--log', $kernelLog, '--no-banner', '--clock-ms', '--accepteula')
    Start-Sleep -Seconds 2

    & powershell -NoProfile -ExecutionPolicy Bypass -File $Probe -DeviceId $deviceId *>&1 |
        Tee-Object -FilePath $wasapiLog | Out-Null
    Wait-Process -Id $proc.Id -Timeout 30 -ErrorAction SilentlyContinue

    Write-Output "  endpoint: $guid"
    Get-Content $wasapiLog | Select-String -Pattern 'Initialize|GetBufferSize|Start hr|Stop hr|MixFormat'
    if (Test-Path $kernelLog) {
        Get-Content $kernelLog |
            Select-String -Pattern 'CreateSpeakerMdl|AllocateAudioBuffer|SetState|Initialize|StartIo|StopIo' |
            Select-Object -Last 25
    }
    Write-Output ""
}
