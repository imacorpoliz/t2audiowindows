# T2AudioPort - DISABLED post-reboot auto-capture.
#
# This script used to run automatically (via the scheduled task
# 'T2Audio-AutoCapture-AfterBoot') and, without asking, restarted the T2 audio
# device with ForceSystemBuffer toggled and the MMIO path live. That is exactly
# the behaviour that kept crashing the machine unattended, so it is now a no-op.
#
# All hardware activation is done by hand through tools\T2Audio-TestAgent.ps1,
# which creates a VOLATILE test session that a reboot erases. There is no
# automatic path to start the driver any more.
#
# This file is intentionally harmless: if the task is ever re-registered, it
# does nothing.

$ErrorActionPreference = 'Continue'

Write-Host 'Auto-Capture-AfterBoot is disabled.' -ForegroundColor Yellow
Write-Host 'Hardware tests must be run manually: tools\T2Audio-TestAgent.ps1' -ForegroundColor Yellow

# Defensive: if this ever runs as the scheduled task, remove the task so it
# cannot fire again.
try {
    Unregister-ScheduledTask -TaskName 'T2Audio-AutoCapture-AfterBoot' -Confirm:$false -ErrorAction Stop
    Write-Host 'Removed stale scheduled task T2Audio-AutoCapture-AfterBoot.' -ForegroundColor Yellow
} catch { }

exit 0
