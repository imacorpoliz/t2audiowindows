# T2AudioPort - Restore Original Apple Audio Driver
# Fetches the ORIGINAL Apple T2 audio driver (AppleAudio.sys + INF) from Apple's
# Boot Camp package using Brigadier, then restores it in place, replacing the
# custom T2AudioMiniport driver.
#
# Brigadier: https://github.com/timsutton/brigadier
#
# DANGER: This modifies system audio drivers. Run as Administrator.
#         A reboot is normally required to finish the restore.

[CmdletBinding()]
param(
    # Mac model to fetch Boot Camp drivers for. Auto-detected if omitted.
    [string]$Model = "",

    # Working directory for Brigadier and the extracted Boot Camp files.
    # Defaults to <project>\local_artifacts\brigadier (git-ignored).
    [string]$WorkDir = "",

    # Skip Brigadier entirely and restore from this already-extracted folder
    # (must contain AppleAudio.sys and its INF).
    [string]$DriverSourceDir = "",

    # Path to a prebuilt brigadier.exe (from GitHub releases). If omitted, the
    # script clones the brigadier source and runs it with Python.
    [string]$BrigadierExe = "",

    # Use files already extracted under WorkDir; do not download again.
    [switch]$SkipDownload,

    # Keep the downloaded/extracted Boot Camp files after the restore.
    [switch]$KeepDownload,

    # Uninstall the custom T2AudioMiniport driver package. Default: on.
    [bool]$UninstallCustom = $true,

    # Do not prompt to reboot at the end.
    [switch]$NoReboot,

    # Skip the interactive confirmation prompt.
    [switch]$Force
)

$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
function Write-Header($text) {
    Write-Host ""
    Write-Host "==================================================================" -ForegroundColor Cyan
    Write-Host "  $text" -ForegroundColor Cyan
    Write-Host "==================================================================" -ForegroundColor Cyan
}
function Write-Step($n, $t) { Write-Host "[$n] $t" -ForegroundColor Cyan }
function Write-Ok($m)       { Write-Host "      [OK] $m" -ForegroundColor Green }
function Write-Info($m)     { Write-Host "      $m" -ForegroundColor Gray }
function Write-Warn($m)     { Write-Host "      [WARN] $m" -ForegroundColor Yellow }
function Write-Fail($m)     { Write-Host "      [ERROR] $m" -ForegroundColor Red }

function Test-Admin {
    return ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Find the base AppleAudio driver (the INF that installs AppleAudio.sys) and the
# .sys binary inside an extracted Boot Camp tree.
function Find-AppleAudioDriver {
    param([string]$Root)

    $sys = Get-ChildItem -Path $Root -Recurse -File -Filter "AppleAudio.sys" -ErrorAction SilentlyContinue |
        Sort-Object Length -Descending | Select-Object -First 1
    if (-not $sys) { return $null }

    # Prefer an INF in the same directory that actually references AppleAudio.sys.
    $inf = Get-ChildItem -Path $sys.Directory.FullName -File -Filter "*.inf" -ErrorAction SilentlyContinue |
        Where-Object { (Get-Content -LiteralPath $_.FullName -Raw -ErrorAction SilentlyContinue) -match "AppleAudio\.sys" } |
        Select-Object -First 1

    # Fall back to any base AppleAudio INF (exclude the Dolby extension INF).
    if (-not $inf) {
        $inf = Get-ChildItem -Path $Root -Recurse -File -Filter "AppleAudio*.inf" -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -notmatch "_\d{7,}" } |
            Where-Object { (Get-Content -LiteralPath $_.FullName -Raw -ErrorAction SilentlyContinue) -match "AppleAudio\.sys" } |
            Select-Object -First 1
    }

    return [pscustomobject]@{
        Sys = $sys.FullName
        SysSize = $sys.Length
        Inf = if ($inf) { $inf.FullName } else { $null }
        Dir = $sys.Directory.FullName
    }
}

function Get-CustomDriverPublishedNames {
    $names = @()
    Get-ChildItem -Path "C:\Windows\INF\oem*.inf" -File -ErrorAction SilentlyContinue | ForEach-Object {
        $text = Get-Content -LiteralPath $_.FullName -Raw -ErrorAction SilentlyContinue
        if ($text -match "T2AudioMiniport" -or $text -match "t2audiominiport" -or $text -match "T2AudioPort") {
            $names += $_.Name
        }
    }
    return $names
}

function Install-Inf {
    param([string]$InfPath)
    Write-Info "pnputil /add-driver `"$InfPath`" /install"
    $out = & pnputil.exe /add-driver "$InfPath" /install 2>&1
    $code = $LASTEXITCODE
    $out | ForEach-Object { Write-Info $_ }
    if ($code -eq 0 -or $code -eq 3010) {
        Write-Ok "Installed: $(Split-Path -Leaf $InfPath) (code $code)"
        return $true
    }
    Write-Warn "pnputil returned code $code for $(Split-Path -Leaf $InfPath)"
    return $false
}

# ---------------------------------------------------------------------------
# Start
# ---------------------------------------------------------------------------
Write-Header "Restore Original Apple Audio Driver (via Brigadier)"

if (-not (Test-Admin)) {
    Write-Fail "Administrator privileges required."
    exit 1
}
Write-Ok "Running as Administrator"

$projectRoot = Split-Path -Parent $PSScriptRoot

# --- Resolve model --------------------------------------------------------
if (-not $Model) {
    $Model = (Get-CimInstance Win32_ComputerSystem).Model
    if (-not $Model) { $Model = "MacBookPro16,1" }
}
Write-Info "Model: $Model"

# --- Resolve work dir -----------------------------------------------------
if (-not $WorkDir) {
    $WorkDir = Join-Path $projectRoot "local_artifacts\brigadier"
}
if (-not (Test-Path -LiteralPath $WorkDir)) {
    New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null
}
$extractDir = Join-Path $WorkDir "extracted"
Write-Info "Work directory: $WorkDir"

# --- Locate or fetch the original driver ----------------------------------
$driver = $null

if ($DriverSourceDir) {
    Write-Step 1 "Using local driver source: $DriverSourceDir"
    if (-not (Test-Path -LiteralPath $DriverSourceDir)) {
        Write-Fail "DriverSourceDir not found: $DriverSourceDir"
        exit 1
    }
    $driver = Find-AppleAudioDriver -Root $DriverSourceDir
} else {
    Write-Step 1 "Obtaining original Apple drivers with Brigadier"

    if ($SkipDownload) {
        Write-Info "SkipDownload set - searching existing extracted files"
        if (Test-Path -LiteralPath $extractDir) {
            $driver = Find-AppleAudioDriver -Root $extractDir
        }
        if (-not $driver) {
            Write-Fail "SkipDownload set but no extracted AppleAudio.sys found under $extractDir"
            Write-Fail "Run once without -SkipDownload, or pass -DriverSourceDir <folder>."
            exit 1
        }
    }

    if (-not $driver) {
        # Run Brigadier (prebuilt exe if given, otherwise source checkout + Python).
        if ($BrigadierExe) {
            if (-not (Test-Path -LiteralPath $BrigadierExe)) {
                Write-Fail "BrigadierExe not found: $BrigadierExe"
                exit 1
            }
            if (-not (Test-Path -LiteralPath $extractDir)) { New-Item -ItemType Directory -Path $extractDir -Force | Out-Null }
            Write-Info "Running: brigadier.exe -m $Model -o `"$extractDir`""
            Write-Warn "Brigadier downloads a large Boot Camp ESD (hundreds of MB - >1 GB)."
            & $BrigadierExe -m $Model -o $extractDir
            if ($LASTEXITCODE -ne 0) { Write-Warn "brigadier.exe exit code: $LASTEXITCODE" }
        } else {
            # Ensure Python
            $python = $null
            foreach ($cand in @("py", "python", "python3")) {
                $cmd = Get-Command $cand -ErrorAction SilentlyContinue
                if ($cmd) {
                    try {
                        & $cmd.Source --version *> $null
                        if ($LASTEXITCODE -eq 0) { $python = $cmd.Source; break }
                    } catch { }
                }
            }
            if (-not $python) {
                Write-Fail "Python not found. Install Python or pass -BrigadierExe <path to brigadier.exe>."
                exit 1
            }
            Write-Info "Python: $python"

            # Ensure brigadier source
            $repoDir = Join-Path $WorkDir "brigadier"
            $script = Join-Path $repoDir "brigadier"
            if (-not (Test-Path -LiteralPath $script)) {
                $git = Get-Command git -ErrorAction SilentlyContinue
                if (-not $git) {
                    Write-Fail "git not found and no local brigadier checkout. Install git or pass -BrigadierExe."
                    exit 1
                }
                Write-Info "Cloning brigadier into $repoDir ..."
                & git clone --depth 1 "https://github.com/timsutton/brigadier.git" "$repoDir"
                if ($LASTEXITCODE -ne 0) { Write-Fail "git clone failed."; exit 1 }
            }

            if (-not (Test-Path -LiteralPath $extractDir)) { New-Item -ItemType Directory -Path $extractDir -Force | Out-Null }
            Write-Warn "Brigadier downloads a large Boot Camp ESD (hundreds of MB - >1 GB)."
            Push-Location $repoDir
            try {
                $brigArgs = @($script, "-m", $Model, "-o", $extractDir)
                if ($KeepDownload) { $brigArgs += "-k" }
                Write-Info "Running: $python brigadier -m $Model -o `"$extractDir`""
                & $python @brigArgs
                if ($LASTEXITCODE -ne 0) { Write-Warn "brigadier exit code: $LASTEXITCODE" }
            } finally {
                Pop-Location
            }
        }

        $driver = Find-AppleAudioDriver -Root $extractDir
    }
}

if (-not $driver) {
    Write-Fail "Could not locate AppleAudio.sys in the downloaded/extracted files."
    Write-Fail "Check the Boot Camp package contents, or pass -DriverSourceDir <folder>."
    exit 1
}

Write-Ok "Found AppleAudio.sys: $($driver.Sys) ($($driver.SysSize) bytes)"
if ($driver.Inf) {
    Write-Ok "Found base INF: $($driver.Inf)"
} else {
    Write-Warn "No INF referencing AppleAudio.sys was found next to the binary."
}

# --- Confirmation ---------------------------------------------------------
Write-Host ""
Write-Host "This script will:" -ForegroundColor Yellow
Write-Host "  1. Back up the current AppleAudio driver files" -ForegroundColor White
if ($UninstallCustom) {
    Write-Host "  2. Uninstall the custom T2AudioMiniport driver package" -ForegroundColor White
}
Write-Host "  3. Stage/install the original AppleAudio driver package" -ForegroundColor White
Write-Host "  4. Place AppleAudio.sys into C:\Windows\System32\drivers" -ForegroundColor White
Write-Host "  5. Rescan devices so PnP rebinds the original driver" -ForegroundColor White
Write-Host ""

if (-not $Force) {
    $confirm = Read-Host "Type 'RESTORE' to continue"
    if ($confirm -ne "RESTORE") {
        Write-Warn "Restore cancelled."
        exit 0
    }
}

# --- Backup current driver files -----------------------------------------
Write-Step 2 "Backing up current AppleAudio files"
$backupDir = Join-Path $projectRoot ("local_backups\restore_{0}" -f (Get-Date -Format "yyyyMMdd_HHmmss"))
New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
Get-ChildItem -Path "C:\Windows\System32\drivers" -Filter "AppleAudio.sys*" -File -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $backupDir -Force
    Write-Info "Backed up: $($_.Name)"
}
Write-Ok "Backups in: $backupDir"

# --- Stop audio service ---------------------------------------------------
Write-Step 3 "Stopping Windows Audio service"
try {
    Stop-Service -Name audiosrv -Force -ErrorAction Stop
    Write-Ok "audiosrv stopped"
} catch {
    Write-Warn "Could not stop audiosrv: $_"
}

# --- Uninstall custom driver ---------------------------------------------
if ($UninstallCustom) {
    Write-Step 4 "Uninstalling custom T2AudioMiniport driver package"
    $oemInfs = Get-CustomDriverPublishedNames
    if ($oemInfs.Count -eq 0) {
        Write-Info "No T2AudioMiniport packages found (already removed?)"
    } else {
        foreach ($oemInf in $oemInfs) {
            Write-Info "pnputil /delete-driver $oemInf /uninstall /force"
            & pnputil.exe /delete-driver $oemInf /uninstall /force 2>&1 | ForEach-Object { Write-Info $_ }
            if ($LASTEXITCODE -eq 0) { Write-Ok "Removed $oemInf" } else { Write-Warn "Failed to remove $oemInf (code $LASTEXITCODE)" }
        }
    }
} else {
    Write-Step 4 "Skipping custom driver uninstall (-UninstallCustom:`$false)"
}

# --- Install original AppleAudio package ----------------------------------
Write-Step 5 "Installing original AppleAudio driver package"
if ($driver.Inf) {
    Install-Inf -InfPath $driver.Inf | Out-Null

    # Install any extension INFs found alongside (e.g. the Dolby APO extension).
    $extInfs = Get-ChildItem -Path $driver.Dir -File -Filter "AppleAudio*.inf" -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -ne $driver.Inf } |
        Where-Object { (Get-Content -LiteralPath $_.FullName -Raw -ErrorAction SilentlyContinue) -match "Class\s*=\s*Extension" }
    foreach ($ext in $extInfs) {
        Install-Inf -InfPath $ext.FullName | Out-Null
    }
} else {
    Write-Warn "No INF available - cannot stage the package. Only the .sys will be copied."
}

# --- Place the .sys binary ------------------------------------------------
Write-Step 6 "Placing AppleAudio.sys"
$targetSys = "C:\Windows\System32\drivers\AppleAudio.sys"
try {
    Copy-Item -LiteralPath $driver.Sys -Destination $targetSys -Force
    $size = (Get-Item -LiteralPath $targetSys).Length
    Write-Ok "Restored $targetSys ($size bytes)"
} catch {
    # PnP may have already placed (and loaded) the file during the INF install,
    # so it can be locked. Accept it if the on-disk file matches the source.
    $existing = Get-Item -LiteralPath $targetSys -ErrorAction SilentlyContinue
    if ($existing -and $existing.Length -eq $driver.SysSize) {
        Write-Ok "AppleAudio.sys already in place and matches source ($($existing.Length) bytes)"
    } else {
        Write-Fail "Could not write $targetSys : $_"
        exit 1
    }
}

# Remove stale renamed copies so PnP binds the fresh file.
foreach ($stale in @("$targetSys.disabled", "$targetSys.before_rollback")) {
    if (Test-Path -LiteralPath $stale) {
        try { Remove-Item -LiteralPath $stale -Force; Write-Info "Removed stale $stale" } catch { Write-Warn "Could not remove $stale" }
    }
}

# --- Fix service start type if a disabled service exists ------------------
$svc = Get-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Services\AppleAudio" -ErrorAction SilentlyContinue
if ($svc) {
    if ($svc.Start -eq 4) {
        try {
            & sc.exe config AppleAudio start= demand | Out-Null
            Write-Ok "Re-enabled AppleAudio service (Start=demand)"
        } catch {
            Write-Warn "Could not reconfigure AppleAudio service: $_"
        }
    } else {
        Write-Info "AppleAudio service present (Start=$($svc.Start))"
    }
} else {
    Write-Info "AppleAudio service not present yet (PnP will create it on bind)"
}

# --- Rescan devices -------------------------------------------------------
Write-Step 7 "Rescanning devices (pnputil /scan-devices)"
& pnputil.exe /scan-devices 2>&1 | ForEach-Object { Write-Info $_ }
Write-Ok "Device scan complete"

# --- Restart audio service ------------------------------------------------
Write-Step 8 "Starting Windows Audio service"
try {
    Start-Service -Name audiosrv -ErrorAction Stop
    Write-Ok "audiosrv started"
} catch {
    Write-Warn "Could not start audiosrv (a reboot may be required): $_"
}

# --- Cleanup --------------------------------------------------------------
if (-not $KeepDownload -and -not $SkipDownload -and -not $DriverSourceDir) {
    Write-Step 9 "Cleaning up downloaded Boot Camp files"
    try {
        Remove-Item -LiteralPath $extractDir -Recurse -Force -ErrorAction SilentlyContinue
        Write-Ok "Removed $extractDir"
    } catch {
        Write-Warn "Could not remove $extractDir : $_"
    }
}

# --- Done -----------------------------------------------------------------
Write-Header "Restore Complete"
Write-Host "Next steps:" -ForegroundColor Yellow
Write-Host "  1. Reboot to finish rebinding the original AppleAudio driver" -ForegroundColor White
Write-Host "  2. After reboot, verify sound and check Device Manager for 'Apple Audio Device'" -ForegroundColor White
Write-Host ""

if (-not $NoReboot) {
    $answer = Read-Host "Reboot now? (y/N)"
    if ($answer -match "^(y|yes)$") {
        Write-Host "Rebooting in 10 seconds..." -ForegroundColor Cyan
        shutdown.exe /r /t 10 /c "T2AudioPort - restoring original AppleAudio driver"
    } else {
        Write-Info "Reboot skipped. Remember to reboot manually."
    }
}
