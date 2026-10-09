# T2AudioPort - stage the driver package (SAFE by default).
#
# By default this script ONLY adds the driver package to the driver store
# (pnputil /add-driver without /install). It does NOT bind the driver to the
# device, does NOT stop audio, does NOT disable AppleAudio, and does NOT reboot.
# Staging is reversible and cannot activate the driver.
#
# Activating the driver on the device is a separate, deliberate step:
#   * -Bind  runs 'pnputil /add-driver <inf> /install' and rescans devices.
#   * Even after -Bind, the driver still will NOT touch the hardware on its own:
#     it refuses to start unless a VOLATILE test session exists. Hardware tests
#     are started by hand with tools\T2Audio-TestAgent.ps1.
#
# Run as Administrator.

[CmdletBinding()]
param(
    # Also bind the package to the device (pnputil /add-driver /install + rescan).
    # Still does not reboot and does not touch AppleAudio.
    [switch]$Bind,

    # Skip the interactive confirmation prompt.
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

function Write-Header($text) {
    Write-Host ''
    Write-Host '==================================================================' -ForegroundColor Cyan
    Write-Host "  $text" -ForegroundColor Cyan
    Write-Host '==================================================================' -ForegroundColor Cyan
}
function Write-Ok($m)   { Write-Host "      [OK] $m" -ForegroundColor Green }
function Write-Info($m) { Write-Host "      $m" -ForegroundColor Gray }
function Write-Warn($m) { Write-Host "      [WARN] $m" -ForegroundColor Yellow }
function Write-Fail($m) { Write-Host "      [ERROR] $m" -ForegroundColor Red }

Write-Header 'T2AudioPort - stage driver package'

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Fail 'Administrator privileges required.'; exit 1 }

$projectRoot = Split-Path -Parent $PSScriptRoot
$packageDir = Join-Path $projectRoot 'packaging'
$infPath = Join-Path $packageDir 'T2AudioMiniport.inf'
$sysPath = Join-Path $packageDir 'T2AudioMiniport.sys'

if (-not (Test-Path -LiteralPath $infPath)) { Write-Fail "INF not found: $infPath"; exit 1 }
if (-not (Test-Path -LiteralPath $sysPath)) { Write-Fail "SYS not found: $sysPath"; exit 1 }

Write-Ok 'Package files found'
Write-Info "INF: $infPath"
Write-Info "SYS: $sysPath ($((Get-Item -LiteralPath $sysPath).Length) bytes)"

# Report signature state (informational only - a test build may be unsigned).
try {
    $sig = Get-AuthenticodeSignature -LiteralPath $sysPath
    Write-Info "Signature: $($sig.Status)"
} catch {
    Write-Warn "Could not read signature: $_"
}

Write-Host ''
if ($Bind) {
    Write-Warn 'This will BIND the package to the T2 device (pnputil /add-driver /install).'
    Write-Warn 'The driver will still refuse to start until the test agent creates a session.'
} else {
    Write-Ok 'Stage-only mode: the package is added to the store but not bound to the device.'
}

if (-not $Force) {
    $answer = Read-Host 'Type STAGE (or BIND when -Bind is set) to continue'
    $expected = if ($Bind) { 'BIND' } else { 'STAGE' }
    if ($answer -ne $expected) { Write-Warn 'Cancelled.'; exit 0 }
}

Write-Host ''
Write-Info 'Adding driver package to the driver store...'
$args = @('/add-driver', "$infPath")
if ($Bind) { $args += '/install' }
$out = & pnputil.exe @args 2>&1
$code = $LASTEXITCODE
$out | ForEach-Object { Write-Info $_ }

if ($code -eq 0 -or $code -eq 3010) {
    Write-Ok "Package added (code $code)"
} else {
    Write-Fail "pnputil returned code $code"
    exit 1
}

if ($Bind) {
    Write-Info 'Rescanning devices...'
    & pnputil.exe /scan-devices 2>&1 | ForEach-Object { Write-Info $_ }
    Write-Ok 'Bind requested. Driver still inert until the test agent runs.'
} else {
    Write-Ok 'Staged. The device is untouched; audio is unaffected.'
}

Write-Host ''
Write-Host 'Next steps:' -ForegroundColor Yellow
Write-Host '  * Hardware tests are manual only: .\T2Audio-TestAgent.ps1 -Mode Test' -ForegroundColor White
Write-Host '  * Check state any time:          .\T2Audio-TestAgent.ps1 -Mode Status' -ForegroundColor White
Write-Host '  * Recover after a crash:         .\T2Audio-TestAgent.ps1 -Mode Recover' -ForegroundColor White
