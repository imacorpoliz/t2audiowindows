<#
.SYNOPSIS
    T2 Audio Control Center & Manager (MacBook Pro 16", 2019)
    Unified companion to T2Audio-Setup.ps1.

.DESCRIPTION
    Provides interactive management, preset switching, feature toggles,
    diagnostics, volume balancing, and emergency rollback to AppleAudio.
    All operations are backed by tools\T2Audio-Setup.ps1.
#>

[CmdletBinding()]
param(
    [ValidateSet('Menu', 'Prepare', 'Install', 'Preset', 'Tune', 'Balance', 'Features', 'Restart',
                 'UpdateGuard', 'Status', 'Doctor', 'Switch', 'Shortcut', 'SoundTest', 'MicTest',
                 'Backup', 'Restore', 'Report', 'ResetGuard', 'Uninstall',
                 'RollbackToApple', 'SwitchToT2Audio')]
    [string]$Action = 'Menu',

    [string]$Preset = 'macos',
    [string]$PresetName = '',

    [ValidateRange(-12, 12)] [Nullable[int]]$BassDb,
    [ValidateRange(-12, 12)] [Nullable[int]]$TrebleDb,
    [ValidateRange(-12, 12)] [Nullable[int]]$ClarityDb,
    [ValidateRange(0, 1)] [Nullable[int]]$CompStyle,

    [ValidateRange(0, 1)] [Nullable[int]]$Headphones,
    [ValidateRange(0, 1)] [Nullable[int]]$Microphone,
    [ValidateRange(0, 1)] [Nullable[int]]$ResumeAfterSleep,
    [ValidateRange(0, 1)] [Nullable[int]]$ExtraFormats,
    [ValidateRange(0, 1)] [Nullable[int]]$HeadphoneCrossfeed,
    [ValidateRange(0, 1)] [Nullable[int]]$DspLoudness,
    [ValidateRange(0, 1)] [Nullable[int]]$AntiRattle,
    [ValidateRange(0, 1)] [Nullable[int]]$MicNoiseSuppression,
    [ValidateRange(0, 1)] [Nullable[int]]$HeadsetMic,
    [ValidateRange(0, 1)] [Nullable[int]]$MicCanceller,
    [ValidateRange(0, 1)] [Nullable[int]]$MicBeam,

    [ValidateSet('Speakers', 'Headphones', 'Toggle')]
    [string]$To = 'Toggle',

    [double]$Volume = 0.3,
    [string]$File = '',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

$ScriptDir     = $PSScriptRoot
$T2SetupScript = Join-Path $ScriptDir 'T2Audio-Setup.ps1'

if (-not (Test-Path -LiteralPath $T2SetupScript)) {
    Write-Host "[ERROR] Master setup script not found: $T2SetupScript" -ForegroundColor Red
    exit 1
}

# Map compatibility actions
$resolvedAction = $Action
if ($Action -eq 'RollbackToApple') {
    $resolvedAction = 'Uninstall'
} elseif ($Action -eq 'SwitchToT2Audio') {
    $resolvedAction = 'Install'
}

# Resolve preset name parameter
$effectivePreset = if ($PresetName) { $PresetName } else { $Preset }

# Build forwarding hashtable for clean PowerShell parameter splatting
$splat = [ordered]@{ Action = $resolvedAction }

# Install forwards the preset only when one was given, so an update keeps
# the current sound settings.
if ($resolvedAction -eq 'Preset' -or
    ($resolvedAction -eq 'Install' -and ($PSBoundParameters.ContainsKey('Preset') -or $PresetName))) {
    $splat['Preset'] = $effectivePreset
}
if ($resolvedAction -in @('Install', 'Balance')) {
    # Only an explicit -Volume: an update keeps the user's volume.
    if ($PSBoundParameters.ContainsKey('Volume')) { $splat['Volume'] = $Volume }
}
if ($null -ne $BassDb)             { $splat['BassDb'] = $BassDb }
if ($null -ne $TrebleDb)           { $splat['TrebleDb'] = $TrebleDb }
if ($null -ne $ClarityDb)          { $splat['ClarityDb'] = $ClarityDb }
if ($null -ne $CompStyle)          { $splat['CompStyle'] = $CompStyle }
if ($null -ne $Headphones)         { $splat['Headphones'] = $Headphones }
if ($null -ne $Microphone)         { $splat['Microphone'] = $Microphone }
if ($null -ne $ResumeAfterSleep)   { $splat['ResumeAfterSleep'] = $ResumeAfterSleep }
if ($null -ne $ExtraFormats)       { $splat['ExtraFormats'] = $ExtraFormats }
if ($null -ne $HeadphoneCrossfeed) { $splat['HeadphoneCrossfeed'] = $HeadphoneCrossfeed }
if ($null -ne $DspLoudness)        { $splat['DspLoudness'] = $DspLoudness }
if ($null -ne $AntiRattle)         { $splat['AntiRattle'] = $AntiRattle }
if ($null -ne $MicNoiseSuppression) { $splat['MicNoiseSuppression'] = $MicNoiseSuppression }
if ($null -ne $HeadsetMic)         { $splat['HeadsetMic'] = $HeadsetMic }
if ($null -ne $MicCanceller)       { $splat['MicCanceller'] = $MicCanceller }
if ($null -ne $MicBeam)            { $splat['MicBeam'] = $MicBeam }
if ($resolvedAction -eq 'Switch')  { $splat['To'] = $To }
if ($File)                         { $splat['File'] = $File }
if ($Force)                        { $splat['Force'] = $true }

& $T2SetupScript @splat
exit $LASTEXITCODE
