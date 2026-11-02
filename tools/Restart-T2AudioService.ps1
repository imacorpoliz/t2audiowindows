# Restart-T2AudioService.ps1 - Gracefully restarts AudioEndpointBuilder/Audiosrv and cycles T2 Audio PnP device
$ErrorActionPreference = 'Continue'
Write-Host "Stopping Audiosrv & AudioEndpointBuilder..."
Stop-Service Audiosrv -Force -ErrorAction SilentlyContinue
Stop-Service AudioEndpointBuilder -Force -ErrorAction SilentlyContinue

$dev = Get-PnpDevice | Where-Object { $_.InstanceId -like 'PCI\VEN_106B&DEV_1803*' } | Select-Object -First 1
if (-not $dev) { throw 'Apple T2 audio device not found' }
$id = $dev.InstanceId
Write-Host "Disabling PnP device..."
Disable-PnpDevice -InstanceId $id -Confirm:$false
Start-Sleep -Seconds 1

Write-Host "Enabling PnP device..."
Enable-PnpDevice -InstanceId $id -Confirm:$false
Start-Sleep -Seconds 2

Write-Host "Starting AudioEndpointBuilder & Audiosrv..."
Start-Service AudioEndpointBuilder
Start-Service Audiosrv
Start-Sleep -Seconds 2

Write-Host "PnP devices for Audio / Mic:"
Get-PnpDevice | Where-Object { $_.FriendlyName -like "*Audio*" -or $_.FriendlyName -like "*Mic*" } | Select-Object FriendlyName, Status, Problem, ConfigManagerErrorCode
