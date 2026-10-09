# T2AudioPort - manual test agent.
#
# This is the ONLY supported way to make the T2AudioMiniport driver touch the
# hardware. It is deliberately NOT a scheduled task and must be started by hand.
#
# How the safety model works:
#   * The driver refuses to start unless it finds EnableTestMode=1 under the
#     VOLATILE key
#         HKLM\SYSTEM\CurrentControlSet\Services\T2AudioMiniport\TestSession
#   * A volatile key lives only in memory and is discarded by the kernel on
#     every reboot. So after a bugcheck or a normal restart the driver comes
#     back with no test session and refuses to start before mapping any BAR,
#     opening the BCE transport, or registering an audio endpoint.
#   * Device-memory writes (the copy DPC) need a SECOND opt-in, EnableMmioCopy=1,
#     which only the -Mmio switch sets. Without it the DPC never writes BAR1.
#   * Hardware I/O commands (BCE START_IO/STOP_IO from SetState) need a THIRD
#     opt-in, EnableBceIo=1, which only the -BceIo switch sets. Without it a
#     stage-4 run wires the speaker and holds the transport open but sends
#     nothing, isolating the send from the stream/transport setup.
#   * Whatever happens, the agent's finally block disables the device and
#     deletes the test session, so the machine is left in the safe state.
#
# Modes:
#   -Mode Test    (default) create session, enable device, capture, probe,
#                 then ALWAYS disable the device and drop the session.
#   -Mode Recover run this after a crash/reboot: drop any session and disable
#                 the device. Safe to run repeatedly.
#   -Mode Status  read-only report (device state, session, bound INF, task).
#
#   * The diagnostic stage (DiagStage) is also cumulative and defaults to 1
#     (RAM only: no BAR mapping, no BCE, system-memory buffer only). Escalate
#     one stage at a time so a fault can be attributed to a single step.
#
# Examples:
#   .\T2Audio-TestAgent.ps1 -Mode Status
#   .\T2Audio-TestAgent.ps1 -Mode Test -Stage 1   # RAM only, no device memory
#   .\T2Audio-TestAgent.ps1 -Mode Test -Stage 3   # + BAR + BCE discovery
#   .\T2Audio-TestAgent.ps1 -Mode Test -Stage 4   # speaker wired, NO I/O sent
#   .\T2Audio-TestAgent.ps1 -Mode Test -Stage 4 -BceIo   # + BCE START_IO/STOP_IO
#   .\T2Audio-TestAgent.ps1 -Mode Test -Stage 5 -Mmio   # + BAR1 writes (risky)
#   .\T2Audio-TestAgent.ps1 -Mode Recover

[CmdletBinding()]
param(
    [ValidateSet('Test', 'Recover', 'Status')]
    [string]$Mode = 'Test',

    # Diagnostic stage (DiagStage), cumulative:
    #   1 RAM only (no BAR) | 2 +BAR metadata | 3 +BCE discovery
    #   4 +hardware I/O     | 5 +MMIO copy (BAR writes)
    # Defaults to 1 (RAM only). Escalate one stage at a time.
    [ValidateRange(1, 5)]
    [int]$Stage = 1,

    # Allow the copy DPC to write device memory (EnableMmioCopy=1). Off by
    # default; only pass this once the buffer address has been confirmed.
    # Implies -Stage 5.
    [switch]$Mmio,

    # Allow SetState to issue BCE START_IO/STOP_IO (EnableBceIo=1). Off by
    # default: at stage 4 the speaker is wired and the transport is held open,
    # but no command is sent. Pass this only after the no-command run is clean.
    [switch]$BceIo,

    # Skip the kernel DbgView capture (faster, no logs).
    [switch]$NoCapture,

    # How long to capture kernel output during a test.
    [int]$CaptureSeconds = 60,

    # How long to wait after restarting the device before probing.
    [int]$SettleSeconds = 6,

    # Render endpoint id to probe. Auto-discovered for the T2 device if empty.
    [string]$DeviceId = '',

    # Skip the interactive confirmation prompt in Test mode.
    [switch]$Force,

    # SHA256 of the driver build you intend to test. Test mode refuses to enable
    # the device unless the installed T2AudioMiniport.sys matches this hash, so a
    # stale or bad build can never be activated by accident.
    [string]$ExpectedSysHash = '',

    # Override the build-hash safety check (NOT recommended).
    [switch]$AllowUnverified
)

$ErrorActionPreference = 'Stop'

$HardwareId       = 'PCI\VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01'
$KnownInstanceId  = 'PCI\VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01\4&3AC8FC3&0&03D8'
$ServiceKeyPath   = 'SYSTEM\CurrentControlSet\Services\T2AudioMiniport'
$TestSubKey       = 'TestSession'
$AutoTaskName     = 'T2Audio-AutoCapture-AfterBoot'

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$ProbeScript = Join-Path $ProjectRoot 'tools\Test-WasapiRender.ps1'
$CaptureRoot = Join-Path $ProjectRoot 'docs\logs'

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

function Test-Admin {
    return ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-T2InstanceId {
    try {
        $dev = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
            Where-Object { $_.InstanceId -like "$HardwareId*" } |
            Select-Object -First 1
        if ($dev) { return $dev.InstanceId }
    } catch { }
    return $KnownInstanceId
}

# Create the volatile test-session key. Returns $true on success.
function New-TestSession {
    param([bool]$AllowMmio, [bool]$AllowBceIo, [int]$DiagStage)

    $svc = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($ServiceKeyPath, $true)
    if ($null -eq $svc) {
        Write-Fail "Service key not found: HKLM\$ServiceKeyPath (driver not installed?)"
        return $false
    }
    try {
        # RegistryOptions::Volatile -> kernel drops the key on reboot.
        $key = $svc.CreateSubKey(
            $TestSubKey,
            [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
            [Microsoft.Win32.RegistryOptions]::Volatile)
        $mmioValue = 0
        if ($AllowMmio) { $mmioValue = 1 }
        $bceIoValue = 0
        if ($AllowBceIo) { $bceIoValue = 1 }
        $key.SetValue('EnableTestMode', 1, [Microsoft.Win32.RegistryValueKind]::DWord)
        $key.SetValue('DiagStage', $DiagStage, [Microsoft.Win32.RegistryValueKind]::DWord)
        $key.SetValue('EnableMmioCopy', $mmioValue, [Microsoft.Win32.RegistryValueKind]::DWord)
        $key.SetValue('EnableBceIo', $bceIoValue, [Microsoft.Win32.RegistryValueKind]::DWord)
        $key.Flush()
        $key.Close()
    } finally {
        $svc.Close()
    }
    return $true
}

function Remove-TestSession {
    $svc = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($ServiceKeyPath, $true)
    if ($null -eq $svc) { return $false }
    try {
        if ($svc.OpenSubKey($TestSubKey)) {
            $svc.DeleteSubKeyTree($TestSubKey, $false)
            Write-Ok 'Removed volatile test session'
        } else {
            Write-Info 'Test session already absent'
        }
        return $true
    } catch {
        Write-Warn "Could not remove test session: $_"
        return $false
    } finally {
        $svc.Close()
    }
}

function Test-TestSessionPresent {
    $svc = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($ServiceKeyPath, $false)
    if ($null -eq $svc) { return $false }
    try { return ($null -ne $svc.OpenSubKey($TestSubKey)) } finally { $svc.Close() }
}

function Remove-AutoTask {
    try {
        Unregister-ScheduledTask -TaskName $AutoTaskName -Confirm:$false -ErrorAction Stop
        Write-Warn "Removed scheduled task '$AutoTaskName'"
    } catch { }
}

# Run pnputil <action>-device in a background job with a hard timeout, so a
# driver wedged in PnP cannot hang the agent forever. Returns the exit code, or
# 124 on timeout. This is the ONLY way the agent changes device state.
function Set-DeviceStateBounded {
    param(
        [string]$InstanceId,
        [ValidateSet('enable', 'disable', 'restart')][string]$Action,
        [int]$TimeoutSeconds = 45
    )
    $flag = "/${Action}-device"
    $job = Start-Job -ScriptBlock {
        param($f, $id)
        $out = & pnputil.exe $f "$id" 2>&1
        [pscustomobject]@{ Code = $LASTEXITCODE; Output = @($out) }
    } -ArgumentList $flag, $InstanceId
    if (Wait-Job $job -Timeout $TimeoutSeconds) {
        $r = Receive-Job $job
        Remove-Job $job -Force -ErrorAction SilentlyContinue
        if ($r) {
            $r.Output | ForEach-Object { Write-Info $_ }
            return $r.Code
        }
        return 0
    }
    Stop-Job $job -ErrorAction SilentlyContinue
    Remove-Job $job -Force -ErrorAction SilentlyContinue
    Write-Warn "pnputil ${Action}-device timed out after ${TimeoutSeconds}s (driver likely stuck in PnP)."
    return 124
}

function Get-T2Endpoint {
    # The render endpoint's DEVPKEY_Device_InstanceId-adjacent property
    # ({a8b865dd...},8) holds the *hardware* ID (PCI\VEN_...\REV_01), not the
    # full instance ID, so match on either.
    param([string]$InstanceId)
    $root = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
    if (-not (Test-Path $root)) { return $null }
    $prop = '{a8b865dd-2e3d-4094-ad97-e593a70c75d6},8'
    foreach ($k in Get-ChildItem $root) {
        $propsPath = Join-Path $k.PSPath 'Properties'
        if (-not (Test-Path $propsPath)) { continue }
        $p = Get-ItemProperty -Path $propsPath -ErrorAction SilentlyContinue
        $v = $p.$prop
        if (-not $v) { continue }
        if ($v -eq $HardwareId -or $v.StartsWith($HardwareId) -or
            $v -eq $InstanceId -or $v.StartsWith($InstanceId)) {
            return '{0.0.0.00000000}.' + $k.PSChildName
        }
    }
    return $null
}

function Get-DbgViewCli {
    foreach ($p in @(
        'C:\Program Files\DebugView\DbgViewCLI.exe',
        'C:\Users\othysa\Desktop\mbp\DebugView\DbgViewCLI.exe')) {
        if (Test-Path -LiteralPath $p) { return $p }
    }
    return $null
}

# ---------------------------------------------------------------------------
# Status
# ---------------------------------------------------------------------------
function Invoke-Status {
    Write-Header 'T2Audio test agent - status'

    $instanceId = Get-T2InstanceId
    Write-Info "Instance ID : $instanceId"
    try {
        $dev = Get-PnpDevice -InstanceId $instanceId -ErrorAction Stop
        Write-Info "Status      : $($dev.Status)  ($($dev.Problem))"
        Write-Info "FriendlyName: $($dev.FriendlyName)"
        try {
            $inf = (Get-PnpDeviceProperty -InstanceId $instanceId -KeyName 'DEVPKEY_Device_DriverInfPath' -ErrorAction Stop).Data
            Write-Info "Bound INF   : $inf"
        } catch { }
    } catch {
        Write-Warn "Get-PnpDevice failed (Safe Mode?): $_"
    }

    if (Test-TestSessionPresent) {
        Write-Warn 'Test session: PRESENT (driver will start / write if MmioCopy=1)'
    } else {
        Write-Ok 'Test session: absent (driver refuses to start - safe state)'
    }

    try {
        $task = Get-ScheduledTask -TaskName $AutoTaskName -ErrorAction Stop
        Write-Warn "Scheduled task '$AutoTaskName' exists: $($task.State)"
    } catch {
        Write-Ok "Scheduled task '$AutoTaskName' not present"
    }
}

# ---------------------------------------------------------------------------
# Recover
# ---------------------------------------------------------------------------
function Invoke-Recover {
    Write-Header 'T2Audio test agent - recover'

    Remove-AutoTask
    $sessionRemoved = Remove-TestSession

    $instanceId = Get-T2InstanceId
    Write-Info "Instance ID: $instanceId"
    Write-Info 'Disabling device (bounded) to guarantee it cannot activate...'
    $disableCode = Set-DeviceStateBounded -InstanceId $instanceId -Action 'disable'
    if ($sessionRemoved -and $disableCode -ne 124) {
        Write-Ok 'Recovery complete - device disabled, no test session'
    } else {
        Write-Warn "Recovery incomplete: sessionRemoved=$sessionRemoved disableCode=$disableCode - re-run Recover."
    }
}

# ---------------------------------------------------------------------------
# Test
# ---------------------------------------------------------------------------
function Invoke-Test {
    Write-Header 'T2Audio test agent - test'

    if (-not (Test-Admin)) { Write-Fail 'Administrator privileges required.'; exit 1 }

    $instanceId = Get-T2InstanceId
    Write-Info "Instance ID : $instanceId"

    # Build guard: never enable the device unless the installed driver is the
    # build the caller vouches for. A leftover or bad build must not be started.
    $sysPath = 'C:\Windows\System32\drivers\T2AudioMiniport.sys'
    if (-not (Test-Path -LiteralPath $sysPath)) {
        Write-Fail "Driver file not present: $sysPath - nothing to test."
        Write-Fail 'Stage and bind a build first (Install-T2AudioDriver.ps1 -Bind).'
        exit 1
    }
    $actualHash = (Get-FileHash -LiteralPath $sysPath -Algorithm SHA256).Hash
    Write-Info "Installed driver SHA256: $actualHash"
    if ($ExpectedSysHash) {
        if ($actualHash -ne $ExpectedSysHash.ToUpper()) {
            Write-Fail 'Installed driver does not match -ExpectedSysHash - refusing to enable.'
            Write-Fail "  expected: $($ExpectedSysHash.ToUpper())"
            Write-Fail "  actual  : $actualHash"
            exit 1
        }
        Write-Ok 'Installed driver matches the expected verified build.'
    } elseif ($AllowUnverified) {
        Write-Warn 'Proceeding with an UNVERIFIED driver (-AllowUnverified).'
    } else {
        Write-Fail 'No -ExpectedSysHash given: refusing to enable an unverified driver.'
        Write-Info 'Pass -ExpectedSysHash <sha256> after installing the verified build,'
        Write-Info 'or -AllowUnverified to override (not recommended).'
        exit 1
    }

    $dev = Get-PnpDevice -InstanceId $instanceId -ErrorAction SilentlyContinue
    if ($dev -and $dev.Service -ne 'T2AudioMiniport') {
        Write-Warn "Device is bound to service '$($dev.Service)', not T2AudioMiniport."
        Write-Warn 'Enabling would exercise the wrong driver; stage+bind the build first.'
        if (-not $AllowUnverified) { exit 1 }
    }

    # -Mmio implies the MMIO stage.
    $effectiveStage = $Stage
    if ($Mmio -and $effectiveStage -lt 5) { $effectiveStage = 5 }

    # -BceIo implies at least the hardware-I/O stage.
    if ($BceIo -and $effectiveStage -lt 4) { $effectiveStage = 4 }

    # START_IO remains unsafe even with FileObject attached to our outgoing
    # request: the lower driver forwards a different IRP into KS and bugchecks.
    # Do not create a hardware-I/O test session while this path is blocked.
    if ($BceIo) {
        Write-Fail 'BCE START_IO is blocked after repeated ks.sys bugchecks; refusing -BceIo.'
        exit 1
    }

    $stageName = switch ($effectiveStage) {
        1 { 'RAM only (no BAR)' }
        2 { 'BAR metadata' }
        3 { 'BCE discovery' }
        4 { if ($BceIo) { 'hardware I/O (BCE START_IO/STOP_IO)' }
            else { 'speaker wired, no I/O (transport held open)' } }
        5 { 'MMIO copy (BAR writes)' }
    }
    Write-Info "Test session: EnableTestMode=1, DiagStage=$effectiveStage ($stageName), EnableMmioCopy=$(if ($Mmio) { 1 } else { 0 }), EnableBceIo=$(if ($BceIo) { 1 } else { 0 })"

    if ($effectiveStage -ge 5) {
        Write-Warn 'Stage 5: the DPC will write into BAR1. This is the risky path.'
    } elseif ($effectiveStage -ge 4 -and $BceIo) {
        Write-Warn 'Stage 4 + -BceIo: SetState will send BCE START_IO/STOP_IO.'
    } else {
        Write-Ok "Stage ${effectiveStage}: the driver will not write device memory."
    }

    if (-not $Force) {
        Write-Host ''
        Write-Host 'This will enable the T2 audio device and load the driver.' -ForegroundColor Yellow
        Write-Host 'If the driver is unstable the machine may bugcheck.' -ForegroundColor Yellow
        $answer = Read-Host "Type 'TEST' to continue"
        if ($answer -ne 'TEST') { Write-Warn 'Cancelled.'; exit 0 }
    }

    $timestamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    $captureDir = Join-Path $CaptureRoot "capture_agent_$timestamp"
    New-Item -ItemType Directory -Path $captureDir -Force | Out-Null
    Write-Info "Capture dir : $captureDir"

    # Make sure nothing auto-runs behind our back.
    Remove-AutoTask

    $dbg = $null
    $proc = $null
    try {
        if (-not (New-TestSession -AllowMmio ([bool]$Mmio) -AllowBceIo ([bool]$BceIo) -DiagStage $effectiveStage)) { exit 1 }

        if (-not $NoCapture) {
            $dbg = Get-DbgViewCli
            if ($dbg) {
                $raw = Join-Path $captureDir 'kernel_raw.csv'
                $cliArgs = @('--kernel', '--no-win32', '--duration', "$CaptureSeconds",
                             '--format', 'csv', '--log', $raw, '--no-banner',
                             '--clock-ms', '--accepteula')
                $proc = Start-Process -FilePath $dbg -ArgumentList $cliArgs -PassThru -NoNewWindow `
                    -RedirectStandardOutput (Join-Path $captureDir 'stdout.txt') `
                    -RedirectStandardError (Join-Path $captureDir 'stderr.txt')
                Write-Info "DbgViewCLI pid $($proc.Id)"
                Start-Sleep -Seconds 3
            } else {
                Write-Warn 'DbgViewCLI not found - skipping kernel capture'
            }
        }

        Write-Info 'Enabling + restarting device (bounded)...'
        $enableCode = Set-DeviceStateBounded -InstanceId $instanceId -Action 'enable'
        if ($enableCode -ne 0) {
            throw "enable-device failed (exit=$enableCode); refusing to probe."
        }
        $restartCode = Set-DeviceStateBounded -InstanceId $instanceId -Action 'restart'
        if ($restartCode -ne 0) {
            throw "restart-device failed (exit=$restartCode); refusing to probe."
        }
        Start-Sleep -Seconds $SettleSeconds

        $startedDevice = Get-PnpDevice -InstanceId $instanceId -ErrorAction Stop
        if ($startedDevice.Status -ne 'OK' -or $startedDevice.Service -ne 'T2AudioMiniport') {
            throw "Test device did not start (status=$($startedDevice.Status), problem=$($startedDevice.Problem), service=$($startedDevice.Service)); refusing to probe."
        }

        $endpoint = $DeviceId
        if (-not $endpoint) { $endpoint = Get-T2Endpoint -InstanceId $instanceId }
        if (-not $endpoint) {
            Write-Warn 'Could not discover T2 render endpoint; probe skipped.'
        } else {
            Write-Info "Endpoint    : $endpoint"
            try {
                $out = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $ProbeScript -DeviceId $endpoint 2>&1 | Out-String
                $out | Out-File (Join-Path $captureDir 'wasapi.txt') -Encoding utf8
                Write-Ok 'WASAPI probe complete'
            } catch {
                Write-Warn "Probe error: $_"
            }
        }
    } finally {
        # Drop the volatile test session FIRST. If the device disable below hangs
        # (a stuck driver can block PnP), the session must already be gone so the
        # next boot cannot auto-start the driver.
        Write-Info 'Cleanup: dropping the volatile test session...'
        $sessionRemoved = Remove-TestSession
        Write-Info 'Disabling device (bounded so a stuck PnP cannot hang the agent)...'
        $disableCode = Set-DeviceStateBounded -InstanceId $instanceId -Action 'disable'

        if ($proc) {
            $proc.WaitForExit(($CaptureSeconds + 30) * 1000) | Out-Null
            if (-not $proc.HasExited) { $proc.Kill() }
            $raw = Join-Path $captureDir 'kernel_raw.csv'
            if (Test-Path -LiteralPath $raw) {
                $t2 = Get-Content -LiteralPath $raw | Where-Object { $_ -match 'T2Audio:' }
                $t2 | Out-File (Join-Path $captureDir 't2audio_messages.txt') -Encoding utf8
                Write-Info "T2Audio lines: $($t2.Count)"
            }
        }
        if ($sessionRemoved -and $disableCode -ne 124) {
            Write-Ok 'Cleanup complete: test session removed; device disabled.'
        } else {
            Write-Warn "Cleanup INCOMPLETE: sessionRemoved=$sessionRemoved disableCode=$disableCode - run -Mode Recover before rebooting."
        }
    }
}

switch ($Mode) {
    'Status'  { Invoke-Status }
    'Recover' { Invoke-Recover }
    'Test'    { Invoke-Test }
}
