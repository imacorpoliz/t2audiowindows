# Measure-AudioCpu.ps1
# Accurately measures CPU, DPC time, and audiodg resource consumption
# across Idle, Active Playback, and Active Capture states.

param(
    [int]$SampleSeconds = 5
)

$ErrorActionPreference = 'Stop'

function Get-CpuSnapshot {
    param([int]$Seconds = 3)
    $samples = Get-Counter -Counter @(
        '\Processor(_Total)\% Processor Time',
        '\Processor(_Total)\% DPC Time',
        '\Processor(_Total)\% Interrupt Time'
    ) -SampleInterval 1 -MaxSamples $Seconds
    
    $totalCpu = ($samples.CounterSamples | Where-Object Path -like '*% Processor Time*' | Measure-Object -Property CookedValue -Average).Average
    $dpcCpu   = ($samples.CounterSamples | Where-Object Path -like '*% DPC Time*' | Measure-Object -Property CookedValue -Average).Average
    $intCpu   = ($samples.CounterSamples | Where-Object Path -like '*% Interrupt Time*' | Measure-Object -Property CookedValue -Average).Average

    $audiodgProc = Get-Process -Name 'audiodg' -ErrorAction SilentlyContinue
    $audiodgCpu = if ($audiodgProc) { $audiodgProc.CPU } else { 0 }

    return [PSCustomObject]@{
        TotalCpuPercent = [math]::Round($totalCpu, 2)
        DpcPercent      = [math]::Round($dpcCpu, 3)
        InterruptPercent= [math]::Round($intCpu, 3)
        AudiodgCpuSec   = $audiodgCpu
    }
}

Write-Host "=== Measuring Baseline Idle (No Audio) for $SampleSeconds s ===" -ForegroundColor Cyan
$idle = Get-CpuSnapshot -Seconds $SampleSeconds

Write-Host "Baseline: Total CPU: $($idle.TotalCpuPercent)%, DPC: $($idle.DpcPercent)%, Interrupt: $($idle.InterruptPercent)%" -ForegroundColor Gray

Write-Host "`n=== Starting Playback (Speakers + DSP) & Measuring for $SampleSeconds s ===" -ForegroundColor Cyan
$playScript = Join-Path $PSScriptRoot "Test-WasapiTone.ps1"
$playJob = Start-Job -ScriptBlock {
    param($script, $duration)
    powershell -ExecutionPolicy Bypass -File $script -Seconds ($duration + 2) -Amplitude 0.1
} -ArgumentList $playScript, $SampleSeconds

Start-Sleep -Seconds 1
$playing = Get-CpuSnapshot -Seconds $SampleSeconds
Wait-Job $playJob | Out-Null
Receive-Job $playJob | Out-Null
Remove-Job $playJob

Write-Host "Playback: Total CPU: $($playing.TotalCpuPercent)%, DPC: $($playing.DpcPercent)%, Interrupt: $($playing.InterruptPercent)%" -ForegroundColor Gray

Write-Host "`n=== Starting Parallel Playback + Microphone Capture for $SampleSeconds s ===" -ForegroundColor Cyan
$recScript = Join-Path $PSScriptRoot "Record-WasapiMic.ps1"
$tmpWav = Join-Path $env:TEMP "cpu_test_mic.wav"

$playJob2 = Start-Job -ScriptBlock {
    param($script, $duration)
    powershell -ExecutionPolicy Bypass -File $script -Seconds ($duration + 2) -Amplitude 0.1
} -ArgumentList $playScript, $SampleSeconds

$recJob = Start-Job -ScriptBlock {
    param($script, $outWav, $duration)
    powershell -ExecutionPolicy Bypass -File $script -OutFile $outWav -DurationSeconds ($duration + 1)
} -ArgumentList $recScript, $tmpWav, $SampleSeconds

Start-Sleep -Seconds 1
$both = Get-CpuSnapshot -Seconds $SampleSeconds
Wait-Job $playJob2, $recJob | Out-Null
Receive-Job $playJob2, $recJob | Out-Null
Remove-Job $playJob2, $recJob
if (Test-Path $tmpWav) { Remove-Item $tmpWav -Force }

Write-Host "Both:     Total CPU: $($both.TotalCpuPercent)%, DPC: $($both.DpcPercent)%, Interrupt: $($both.InterruptPercent)%" -ForegroundColor Gray

$deltaDpcPlay = [math]::Round($playing.DpcPercent - $idle.DpcPercent, 3)
$deltaDpcBoth = [math]::Round($both.DpcPercent - $idle.DpcPercent, 3)
$deltaCpuPlay = [math]::Round($playing.TotalCpuPercent - $idle.TotalCpuPercent, 2)
$deltaCpuBoth = [math]::Round($both.TotalCpuPercent - $idle.TotalCpuPercent, 2)

Write-Host "`n==========================================================" -ForegroundColor Green
Write-Host "                  CPU & DPC IMPACT SUMMARY                 " -ForegroundColor Green
Write-Host "==========================================================" -ForegroundColor Green
Write-Host ("Idle State:          CPU: {0,5}% | DPC: {1,5}% | Int: {2,5}%" -f $idle.TotalCpuPercent, $idle.DpcPercent, $idle.InterruptPercent)
Write-Host ("Playback Active:     CPU: {0,5}% | DPC: {1,5}% | Int: {2,5}%" -f $playing.TotalCpuPercent, $playing.DpcPercent, $playing.InterruptPercent)
Write-Host ("Playback + Mic:      CPU: {0,5}% | DPC: {1,5}% | Int: {2,5}%" -f $both.TotalCpuPercent, $both.DpcPercent, $both.InterruptPercent)
Write-Host "----------------------------------------------------------"
Write-Host ("Net Driver DPC Cost: Playback: +{0}% | Play+Mic: +{1}%" -f $deltaDpcPlay, $deltaDpcBoth) -ForegroundColor Yellow
Write-Host ("Net Total CPU Delta: Playback: {0:+0.00;-0.00;0.00}% | Play+Mic: {1:+0.00;-0.00;0.00}%" -f $deltaCpuPlay, $deltaCpuBoth) -ForegroundColor Yellow
Write-Host "=========================================================="
