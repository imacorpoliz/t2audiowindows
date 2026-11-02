<#
.SYNOPSIS
    T2AudioPort - Set Default Audio Endpoint
    Supports both device GUIDs and friendly names ('Speakers', 'Headphones', 'Toggle').
#>

[CmdletBinding()]
param(
    [string]$DeviceId = '',
    [ValidateSet('Speakers', 'Headphones', 'Toggle')]
    [string]$To = 'Toggle'
)

$ErrorActionPreference = 'Stop'
$ScriptDir = $PSScriptRoot
$T2Setup = Join-Path $ScriptDir 'T2Audio-Setup.ps1'

if (-not (Test-Path -LiteralPath $T2Setup)) {
    Write-Host "[ERROR] Master setup script not found: $T2Setup" -ForegroundColor Red
    exit 1
}

if ($DeviceId) {
    $code = @'
using System;
using System.Runtime.InteropServices;

[Guid("F8679F50-850A-41CF-9C72-430F290290C8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IPolicyConfig {
    [PreserveSig] int GetMixFormat([MarshalAs(UnmanagedType.LPWStr)] string pszDeviceName, out IntPtr ppFormat);
    [PreserveSig] int GetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string pszDeviceName, [MarshalAs(UnmanagedType.Bool)] bool bDefault, out IntPtr ppFormat);
    [PreserveSig] int ResetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string pszDeviceName);
    [PreserveSig] int SetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string pszDeviceName, IntPtr pEndpointFormat, IntPtr mixFormat);
    [PreserveSig] int GetProcessingPeriod([MarshalAs(UnmanagedType.LPWStr)] string pszDeviceName, [MarshalAs(UnmanagedType.Bool)] bool bDefault, IntPtr pmftDefaultPeriod, IntPtr pmftMinimumPeriod);
    [PreserveSig] int SetProcessingPeriod([MarshalAs(UnmanagedType.LPWStr)] string pszDeviceName, IntPtr pmftPeriod);
    [PreserveSig] int GetShareMode([MarshalAs(UnmanagedType.LPWStr)] string pszDeviceName, IntPtr pMode);
    [PreserveSig] int SetShareMode([MarshalAs(UnmanagedType.LPWStr)] string pszDeviceName, IntPtr mode);
    [PreserveSig] int GetPropertyValue([MarshalAs(UnmanagedType.LPWStr)] string pszDeviceName, [MarshalAs(UnmanagedType.Bool)] bool bFxStore, IntPtr key, IntPtr pv);
    [PreserveSig] int SetPropertyValue([MarshalAs(UnmanagedType.LPWStr)] string pszDeviceName, [MarshalAs(UnmanagedType.Bool)] bool bFxStore, IntPtr key, IntPtr pv);
    [PreserveSig] int SetDefaultEndpoint([MarshalAs(UnmanagedType.LPWStr)] string pszDeviceName, int role);
    [PreserveSig] int SetEndpointVisibility([MarshalAs(UnmanagedType.LPWStr)] string pszDeviceName, [MarshalAs(UnmanagedType.Bool)] bool bVisible);
}

[ComImport, Guid("870AF99C-171D-4F9E-AF0D-E63DF40C2BC9")]
public class CPolicyConfigClient { }

public static class PolicyConfig {
    public static int SetDefault(string deviceId, int role) {
        var client = (IPolicyConfig)Activator.CreateInstance(typeof(CPolicyConfigClient));
        return client.SetDefaultEndpoint(deviceId, role);
    }
}
'@
    if (-not ('PolicyConfig' -as [type])) {
        Add-Type -TypeDefinition $code
    }
    $roles = @{ 'Console' = 0; 'Multimedia' = 1; 'Communications' = 2 }
    foreach ($name in $roles.Keys) {
        $hr = [PolicyConfig]::SetDefault($DeviceId, $roles[$name])
        if ($hr -eq 0) {
            Write-Host "Set default $name -> $DeviceId : OK" -ForegroundColor Green
        } else {
            Write-Host ("Set default $name -> $DeviceId : FAILED hr=0x{0:X8}" -f $hr) -ForegroundColor Red
        }
    }
} else {
    & $T2Setup -Action Switch -To $To
}
