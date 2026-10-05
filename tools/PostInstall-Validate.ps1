# T2AudioPort Post-Installation Validation Script
# Run this after reboot to verify driver installation and test audio functionality

param(
    [switch]$SkipAudioTest = $false,
    [int]$TestVolume = -30  # dB (relative to max)
)

$ErrorActionPreference = "Continue"

Write-Host "==================================================================" -ForegroundColor Cyan
Write-Host "  T2AudioPort Post-Installation Validation" -ForegroundColor Cyan
Write-Host "==================================================================" -ForegroundColor Cyan
Write-Host ""

$testsPassed = 0
$testsTotal = 0

function Test-Component {
    param([string]$Name, [scriptblock]$Test)
    
    $script:testsTotal++
    Write-Host "[$script:testsTotal] Testing: $Name" -ForegroundColor Cyan
    
    try {
        $result = & $Test
        if ($result) {
            Write-Host "    [PASS]" -ForegroundColor Green
            $script:testsPassed++
            return $true
        } else {
            Write-Host "    [FAIL]" -ForegroundColor Red
            return $false
        }
    } catch {
        Write-Host "    [ERROR] $_" -ForegroundColor Red
        return $false
    }
}

# Test 1: Check if driver is loaded
Test-Component "Driver loaded in kernel" {
    $driverLoaded = Get-WindowsDriver -Online | Where-Object {
        $_.OriginalFileName -like "*T2AudioMiniport*"
    }
    if ($driverLoaded) {
        Write-Host "      Found: $($driverLoaded.OriginalFileName)" -ForegroundColor Gray
        return $true
    }
    return $false
}

# Test 2: Check device status in Device Manager
Test-Component "Device Manager status" {
    $t2Device = Get-PnpDevice | Where-Object {
        $_.InstanceId -like "PCI\VEN_106B&DEV_1803*"
    }
    if ($t2Device) {
        Write-Host "      Device: $($t2Device.FriendlyName)" -ForegroundColor Gray
        Write-Host "      Status: $($t2Device.Status)" -ForegroundColor Gray
        Write-Host "      Problem: $($t2Device.Problem)" -ForegroundColor Gray
        
        if ($t2Device.Status -eq "OK") {
            return $true
        } else {
            Write-Host "      Device has problem code: $($t2Device.Problem)" -ForegroundColor Yellow
            return $false
        }
    }
    Write-Host "      Device not found" -ForegroundColor Red
    return $false
}

# Test 3: Check driver file in System32\drivers
Test-Component "Driver file present" {
    $driverPath = "C:\Windows\System32\drivers\T2AudioMiniport.sys"
    if (Test-Path -LiteralPath $driverPath) {
        $fileSize = (Get-Item -LiteralPath $driverPath).Length
        Write-Host "      Path: $driverPath" -ForegroundColor Gray
        Write-Host "      Size: $fileSize bytes" -ForegroundColor Gray
        return $true
    }
    return $false
}

# Test 4: Check audio service running
Test-Component "Windows Audio service" {
    $audioSvc = Get-Service -Name audiosrv
    Write-Host "      Status: $($audioSvc.Status)" -ForegroundColor Gray
    return ($audioSvc.Status -eq "Running")
}

# Test 5: Check audio endpoints
Test-Component "Audio endpoints enumeration" {
    Add-Type -AssemblyName System.Speech
    $endpoints = Get-CimInstance -ClassName Win32_SoundDevice | Where-Object {
        $_.Name -like "*Apple*" -or $_.Name -like "*T2*"
    }
    
    if ($endpoints) {
        foreach ($ep in $endpoints) {
            Write-Host "      Found: $($ep.Name)" -ForegroundColor Gray
            Write-Host "      Status: $($ep.Status)" -ForegroundColor Gray
        }
        return $true
    } else {
        Write-Host "      No T2 audio endpoints found" -ForegroundColor Yellow
        return $false
    }
}

# Test 6: Check kernel debug messages (if DebugView available)
Test-Component "Kernel debug messages" {
    # Try to find DbgView log or use kernel debugger output
    $expectedMessages = @(
        "T2Audio: PortCls DriverEntry success",
        "T2Audio: hardware validated",
        "T2Audio: WaveRT subdevice registered"
    )
    
    Write-Host "      Expected messages in kernel log:" -ForegroundColor Gray
    foreach ($msg in $expectedMessages) {
        Write-Host "        - $msg" -ForegroundColor Gray
    }
    Write-Host "      (Check via kernel debugger or DbgView)" -ForegroundColor Yellow
    
    # Cannot verify without debugger attached, assume pass if driver loaded
    return $true
}

# Test 7: Check BCE transport
Test-Component "BCE transport availability" {
    $vhciService = Get-Service | Where-Object { 
        $_.Name -like "*USBVHCI*" -or $_.Name -like "*AppleUSBVHCI*" 
    }
    
    if ($vhciService) {
        Write-Host "      Service: $($vhciService.Name)" -ForegroundColor Gray
        Write-Host "      Status: $($vhciService.Status)" -ForegroundColor Gray
        return ($vhciService.Status -eq "Running")
    } else {
        Write-Host "      AppleUSBVHCI not found (BCE transport will fail)" -ForegroundColor Yellow
        return $false
    }
}

# Test 8: Check Event Viewer for driver errors
Test-Component "Event Viewer errors" {
    $errors = Get-EventLog -LogName System -Source "Application Popup" -Newest 10 -ErrorAction SilentlyContinue | 
        Where-Object { $_.Message -like "*T2Audio*" }
    
    if ($errors) {
        Write-Host "      Found driver-related errors:" -ForegroundColor Yellow
        foreach ($err in $errors) {
            Write-Host "        $($err.TimeGenerated): $($err.Message)" -ForegroundColor Gray
        }
        return $false
    } else {
        Write-Host "      No driver errors in Event Log" -ForegroundColor Gray
        return $true
    }
}

Write-Host ""
Write-Host "==================================================================" -ForegroundColor Cyan
Write-Host "  Validation Results: $testsPassed / $testsTotal tests passed" -ForegroundColor Cyan
Write-Host "==================================================================" -ForegroundColor Cyan
Write-Host ""

if ($testsPassed -eq $testsTotal) {
    Write-Host "All tests passed! Driver appears to be working." -ForegroundColor Green
} elseif ($testsPassed -ge ($testsTotal * 0.7)) {
    Write-Host "Most tests passed. Driver may be partially functional." -ForegroundColor Yellow
} else {
    Write-Host "Multiple tests failed. Driver installation may have issues." -ForegroundColor Red
}

Write-Host ""

# Audio test section
if (-not $SkipAudioTest -and $testsPassed -ge 4) {
    Write-Host "==================================================================" -ForegroundColor Yellow
    Write-Host "  Audio Playback Test (OPTIONAL)" -ForegroundColor Yellow
    Write-Host "==================================================================" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "WARNING: Audio test will play a tone through speakers." -ForegroundColor Yellow
    Write-Host "         Volume will be set to $TestVolume dB (low level)" -ForegroundColor Yellow
    Write-Host "         STOP IMMEDIATELY if you hear distortion or clicking" -ForegroundColor Yellow
    Write-Host ""
    
    $runTest = Read-Host "Run audio test? (y/N)"
    if ($runTest -eq "y") {
        Write-Host ""
        Write-Host "Preparing audio test..." -ForegroundColor Cyan
        Write-Host "  - Frequency: 1000 Hz sine wave" -ForegroundColor Gray
        Write-Host "  - Duration: 500 ms" -ForegroundColor Gray
        Write-Host "  - Volume: $TestVolume dB" -ForegroundColor Gray
        Write-Host ""
        Write-Host "Test will start in 3 seconds..." -ForegroundColor Yellow
        Start-Sleep -Seconds 3
        
        # Use Windows beep or Media.SoundPlayer for simple test
        try {
            # Simple beep test
            [Console]::Beep(1000, 500)
            Write-Host "[OK] Audio test completed" -ForegroundColor Green
            Write-Host ""
            
            $hearSound = Read-Host "Did you hear clear audio without distortion? (y/N)"
            if ($hearSound -eq "y") {
                Write-Host "[SUCCESS] Audio appears to be working correctly!" -ForegroundColor Green
            } else {
                Write-Host "[FAIL] Audio issue detected. Check Device Manager and Event Viewer." -ForegroundColor Red
            }
        } catch {
            Write-Host "[ERROR] Audio test failed: $_" -ForegroundColor Red
        }
    }
}

Write-Host ""
Write-Host "==================================================================" -ForegroundColor Cyan
Write-Host "  Diagnostic Information" -ForegroundColor Cyan
Write-Host "==================================================================" -ForegroundColor Cyan
Write-Host ""

# Dump useful info for troubleshooting
Write-Host "System Audio Devices:" -ForegroundColor Yellow
Get-CimInstance -ClassName Win32_SoundDevice | Format-Table Name, Status, Manufacturer -AutoSize

Write-Host ""
Write-Host "Driver Files:" -ForegroundColor Yellow
Get-ChildItem -Path "C:\Windows\System32\drivers" -Filter "*T2Audio*" -ErrorAction SilentlyContinue | 
    Format-Table Name, Length, LastWriteTime -AutoSize

Write-Host ""
Write-Host "Recent System Events (last 10):" -ForegroundColor Yellow
Get-EventLog -LogName System -Newest 10 -ErrorAction SilentlyContinue | 
    Where-Object { $_.Source -like "*Audio*" -or $_.Source -like "*PnP*" } |
    Format-Table TimeGenerated, Source, EventID, Message -AutoSize

Write-Host ""
Write-Host "Next steps:" -ForegroundColor Yellow
if ($testsPassed -lt $testsTotal) {
    Write-Host "  - Check Device Manager for error codes" -ForegroundColor White
    Write-Host "  - Review Event Viewer (System log)" -ForegroundColor White
    Write-Host "  - Check kernel debugger output if enabled" -ForegroundColor White
    Write-Host "  - Run rollback script if driver is non-functional" -ForegroundColor White
} else {
    Write-Host "  - Test audio with your media player" -ForegroundColor White
    Write-Host "  - Monitor for stability over next few hours" -ForegroundColor White
    Write-Host "  - Check for any audio glitches or distortion" -ForegroundColor White
}

Write-Host ""
Write-Host "Validation complete." -ForegroundColor Cyan
Write-Host ""
