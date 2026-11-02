<#
.SYNOPSIS
    T2AudioPort - 1-Click Rollback Script (MacBook Pro 16", 2019)
    Unified wrapper calling tools\T2Audio-Setup.ps1 -Action Uninstall.
#>

[CmdletBinding()]
param(
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
    Action = 'Uninstall'
}
if ($Force) { $splat['Force'] = $true }

& $T2Setup @splat
exit $LASTEXITCODE
