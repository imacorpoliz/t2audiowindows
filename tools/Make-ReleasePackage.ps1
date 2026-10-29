# Builds a release folder and zip from a signed package in packaging\.
#
#   .\tools\Make-ReleasePackage.ps1 [-CertificatePath <public .cer>] [-OutDir dist]
#
# Run after the driver was built and signed (GEMINI.md section 2). The result,
# dist\T2AudioPort-<version>\ and dist\T2AudioPort-<version>.zip, has the
# layout the installer expects (tools\ next to packaging\):
#
#   packaging\T2AudioMiniport.inf / .sys / t2audiominiport.cat
#   tools\T2Audio-Setup.ps1, tools\Restore-AppleAudioDriver.ps1
#   certificate\<name>.cer          (only with -CertificatePath; public part only)
#   README.md, THIRD_PARTY_LICENSES.md, LICENSE (if present), assets\banner.svg
#   docs\GUIDE.md, docs\INSTALL.md (README links keep working inside the zip)
#   SHA256SUMS.txt
#
# The script refuses to package an unsigned .sys/.cat, a .sys whose INF
# version is missing, or a certificate file that contains a private key.

[CmdletBinding()]
param(
    [string]$CertificatePath = '',
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $root 'dist' }
$pkg = Join-Path $root 'packaging'
$inf = Join-Path $pkg 'T2AudioMiniport.inf'
$sys = Join-Path $pkg 'T2AudioMiniport.sys'
$cat = Join-Path $pkg 't2audiominiport.cat'

foreach ($f in @($inf, $sys, $cat)) {
    if (-not (Test-Path -LiteralPath $f)) { throw "Missing $f - build and sign the package first (GEMINI.md section 2)." }
}
foreach ($f in @($sys, $cat)) {
    $sig = Get-AuthenticodeSignature -LiteralPath $f
    if (-not $sig.SignerCertificate) { throw "$f is not signed." }
    Write-Host ("signed: {0}  by {1}  ({2})" -f (Split-Path -Leaf $f), $sig.SignerCertificate.Subject, $sig.Status)
}
$m = Select-String -LiteralPath $inf -Pattern '^DriverVer\s*=\s*[^,]+,\s*([0-9.]+)' | Select-Object -First 1
if (-not $m) { throw "No DriverVer in $inf." }
$version = $m.Matches[0].Groups[1].Value

$name = "T2AudioPort-$version"
$dest = Join-Path $OutDir $name
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
New-Item -ItemType Directory -Force -Path (Join-Path $dest 'packaging'), (Join-Path $dest 'tools'),
    (Join-Path $dest 'docs'), (Join-Path $dest 'assets') | Out-Null

Copy-Item $inf, $sys, $cat -Destination (Join-Path $dest 'packaging')
Copy-Item (Join-Path $root 'tools\T2Audio-Setup.ps1'), (Join-Path $root 'tools\Restore-AppleAudioDriver.ps1') `
    -Destination (Join-Path $dest 'tools')
Copy-Item (Join-Path $root 'README.md'), (Join-Path $root 'THIRD_PARTY_LICENSES.md') -Destination $dest
Copy-Item (Join-Path $root 'docs\INSTALL.md'), (Join-Path $root 'docs\GUIDE.md') -Destination (Join-Path $dest 'docs')
Copy-Item (Join-Path $root 'assets\banner.svg') -Destination (Join-Path $dest 'assets')
$license = Join-Path $root 'LICENSE'
if (Test-Path $license) { Copy-Item $license -Destination $dest } else { Write-Warning 'No LICENSE file: add one before a public release (see README, Licence).' }

if ($CertificatePath) {
    $cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $CertificatePath
    if ($cert.HasPrivateKey) { throw "$CertificatePath contains a private key - export the public .cer only." }
    New-Item -ItemType Directory -Force -Path (Join-Path $dest 'certificate') | Out-Null
    Copy-Item $CertificatePath -Destination (Join-Path $dest 'certificate')
    Write-Host "certificate: $($cert.Subject) $($cert.Thumbprint)"
}

$sums = Get-ChildItem -Path $dest -Recurse -File | Sort-Object FullName | ForEach-Object {
    $rel = $_.FullName.Substring($dest.Length + 1).Replace('\', '/')
    '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $rel
}
Set-Content -Path (Join-Path $dest 'SHA256SUMS.txt') -Value $sums -Encoding ascii

$zip = Join-Path $OutDir "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $dest '*') -DestinationPath $zip
Write-Host "release: $dest"
Write-Host "zip:     $zip  ($([math]::Round((Get-Item $zip).Length / 1KB)) KB)"
