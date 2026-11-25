# Rebuilds a test-signed package in packaging\ after the INF was changed.
# Needs the Windows Driver Kit (Inf2Cat.exe, signtool.exe) on this machine.
# Run from the repo folder in an elevated PowerShell:
#
#   .\tools\Build-TestPackage.ps1 -SysPath <path to a built T2AudioMiniport.sys>
#
# Steps: copy the .sys into packaging\, create or reuse a test code-signing
# certificate, sign the .sys, run Inf2Cat to make a new catalog for the INF,
# sign the catalog, then call Make-ReleasePackage.ps1 to build the zip.
# Not tested on hardware; check the output of each step.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SysPath,
    [string]$CertName = 'T2AudioPort Test Certificate'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$pkg  = Join-Path $root 'packaging'

function Find-KitTool([string]$Name, [string]$Arch) {
    $bin = 'C:\Program Files (x86)\Windows Kits\10\bin'
    if (-not (Test-Path $bin)) { throw "Windows Driver Kit not found in $bin. Install the WDK first." }
    $tool = Get-ChildItem -Path $bin -Recurse -Filter $Name -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match ('\\' + $Arch + '\\') } |
        Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $tool) { throw "$Name ($Arch) not found under $bin." }
    return $tool.FullName
}

if (-not (Test-Path -LiteralPath $SysPath)) { throw "No .sys at $SysPath" }
$inf2cat  = Find-KitTool 'Inf2Cat.exe' 'x86'
$signtool = Find-KitTool 'signtool.exe' 'x64'
Write-Host "Inf2Cat:  $inf2cat"
Write-Host "signtool: $signtool"

# 1. Copy the driver binary into the package folder.
Copy-Item -LiteralPath $SysPath -Destination (Join-Path $pkg 'T2AudioMiniport.sys') -Force

# 2. Reuse the test certificate if it already exists, otherwise create it.
$cert = Get-ChildItem Cert:\CurrentUser\My |
    Where-Object { $_.Subject -eq "CN=$CertName" } | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=$CertName" `
        -KeyUsage DigitalSignature -KeyAlgorithm RSA -KeyLength 2048 `
        -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(5)
    Write-Host "Created certificate $($cert.Thumbprint)"
} else {
    Write-Host "Reusing certificate $($cert.Thumbprint)"
}
$cerPath = Join-Path $env:TEMP 'T2AudioPort-Test.cer'
Export-Certificate -Cert $cert -FilePath $cerPath -Type CERT | Out-Null

# 3. Sign the .sys (the catalog hashes it, so this must come before Inf2Cat).
$sys = Join-Path $pkg 'T2AudioMiniport.sys'
& $signtool sign /fd SHA256 /sha1 $cert.Thumbprint $sys
if ($LASTEXITCODE -ne 0) { throw 'signtool failed on the .sys' }

# 4. Create a new catalog for the INF (the old one no longer matches it).
$cat = Join-Path $pkg 'T2AudioMiniport.cat'
Remove-Item -LiteralPath $cat -ErrorAction SilentlyContinue
& $inf2cat /driver:$pkg /os:10_X64 /verbose
if ($LASTEXITCODE -ne 0) { throw 'Inf2Cat failed; check packaging\T2AudioMiniport.inf' }

# 5. Sign the catalog.
& $signtool sign /fd SHA256 /sha1 $cert.Thumbprint $cat
if ($LASTEXITCODE -ne 0) { throw 'signtool failed on the catalog' }

# 6. Build the release folder and zip.
& (Join-Path $root 'tools\Make-ReleasePackage.ps1') -CertificatePath $cerPath
Write-Host 'Done. Next: Prepare (test mode), reboot, then Install from the dist folder.'
