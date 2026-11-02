<#
.SYNOPSIS
    T2AudioPort - 1-Click Driver Installer (MacBook Pro 16", 2019)
    Unified wrapper calling tools\T2Audio-Setup.ps1 -Action Install.
#>

[CmdletBinding()]
param(
    [string]$Preset = 'macos',
    [double]$Volume = 0.3,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$ScriptDir = $PSScriptRoot
$T2Setup = Join-Path $ScriptDir 'T2Audio-Setup.ps1'

if (-not (Test-Path -LiteralPath $T2Setup)) {
    Write-Host "[ERROR] Master setup script not found: $T2Setup" -ForegroundColor Red
    exit 1
}

$splat = [ordered]@{
    Action = 'Install'
    Preset = $Preset
    Volume = $Volume
}
if ($Force) { $splat['Force'] = $true }

& $T2Setup @splat
exit $LASTEXITCODE
