# T2AudioPort Driver Debugging Log

**Last Updated:** 2026-10-05 21:49 UTC

## Current Problem Summary
T2AudioMiniport.sys driver **loads successfully** through PortCls (DriverEntry and AddDevice succeed), but **fails in StartDevice** with **STATUS_DEVICE_CONFIGURATION_ERROR (0xC0000182)**. 

**ROOT CAUSE IDENTIFIED (2026-10-05 21:47 UTC):**
- **Failure Point**: FAIL_K in T2AudioMapResources (Device.c:125)
- **Condition**: GPR signature validation fails (0xFFFFFFFF != 0x19870423)
- **Evidence**: All GPR registers at offset 0xC000 return 0xFFFFFFFF (hardware not responding)
- **Resources**: 3 memory resources allocated, BAR1/BAR2 mapped successfully
- **Conclusion**: Either T2AUDIO_GPR_OFFSET incorrect, or T2 chip requires initialization before registers accessible

See `docs/MAPRESOURCES_ROOT_CAUSE.md` for complete analysis.

**Previous problems (FIXED):** 
- STATUS_INVALID_PARAMETER (0xC000000D) caused by swapped arguments to PcAddAdapterDevice

## Hardware Configuration
- **Model**: MacBookPro16,1 (2019)
- **OS**: Windows 11
- **Audio Chip**: Apple T2 (PCI\VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01)
- **Target Audio**: 6-channel speaker array, 48kHz, 24-bit in 32-bit containers
- **Memory Resources (CONFIRMED 2026-10-05)**:
  - 3 memory descriptors allocated by Windows
  - BAR1: Physical 0xC1000000, Length 0x400000 (4 MB) - audio buffers
  - BAR2: Physical 0xC1680000, Length 0x80000 (512 KB) - config/GPR
  - BAR3: Not queried (unused by driver)

## Boot Configuration
```
testsigning=Yes
nointegritychecks=Yes
loadoptions=DISABLE_INTEGRITY_CHECKS
```

## Driver Architecture
- **Type**: Pure PortCls WDM audio miniport driver
- **Framework**: PcInitializeAdapterDriver + PcAddAdapterDevice + PcNewPort + PcRegisterSubdevice
- **Port Type**: IPortWaveRT (WaveRT streaming model)
- **Entry Points**: DriverEntry -> T2AudioAddDevice -> T2AudioStartDevice

## Driver Signing
- **Certificate**: CN=T2AudioPort Test Certificate
- **SHA1 Thumbprint**: CA2DE95D1B065F6D6CFF6C4158DC88E2424ABBCE
- **Store Location**: LocalMachine\My, LocalMachine\Root, LocalMachine\TrustedPublisher
- **Signature Algorithm**: SHA256 with DigiCert timestamp
- **Signed Files**: T2AudioMiniport.sys, t2audiominiport.cat
- **Installation**: pnputil /add-driver T2AudioMiniport.inf /install (oem83.inf)

## Build Configuration
- **Toolchain**: MSBuild 18.10.1, WDK 10.0.28000.0
- **Platform**: x64 Debug
- **Output**: C:\Users\othysa\Desktop\mbp\T2AudioPort\Phase2\bin\Debug\T2AudioMiniport.sys
- **Installed Location**: C:\Windows\System32\drivers\T2AudioMiniport.sys (19968 bytes as of latest build)
- **Auto-signing**: Disabled (/p:SignMode=Off), manual signing with signtool

## Previous Driver (AppleAudio.sys)
- **Status**: Disabled
- **Actions Taken**:
  - Renamed to AppleAudio.sys.disabled
  - Service disabled in registry
  - oem83.inf deleted from driver store (originally occupied by AppleAudio, now reused by T2AudioMiniport)

## Critical Code Points

### Driver.c - DriverEntry (lines 4-19)
```c
NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    status = PcInitializeAdapterDriver(DriverObject, RegistryPath, T2AudioAddDevice);
    // Returns STATUS_SUCCESS normally
}
```
**Status**: No evidence this is called (no Event ID 7000/7026 in System log)

### Driver.c - T2AudioAddDevice (lines 22-38)
```c
NTSTATUS T2AudioAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    status = PcAddAdapterDevice(DriverObject, PhysicalDeviceObject,
                                T2AudioStartDevice, 16,
                                sizeof(T2AUDIO_DEVICE_CONTEXT));
}
```
**Parameters**:
- MaxObjects: 16 (changed from 1 after initial failure)
- DeviceExtensionSize: sizeof(T2AUDIO_DEVICE_CONTEXT)

**Status**: Unknown if called - no KdPrint logs visible

### Driver.c - T2AudioStartDevice (lines 40-88, simplified version)
```c
NTSTATUS T2AudioStartDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList)
{
    // SIMPLIFIED VERSION (as of latest build):
    // 1. Validate DeviceObject != NULL
    // 2. Validate Irp != NULL
    // 3. Validate ResourceList != NULL
    // 4. Get context via T2AudioGetContext(DeviceObject)
    // 5. Validate context != NULL
    // 6. RtlZeroMemory(context)
    // 7. Call T2AudioMapResources(context, ResourceList)
    // 8. Return STATUS_SUCCESS (no port creation, no BCE transport)
}
```

**Current Strategy**: Minimal implementation to isolate where STATUS_INVALID_PARAMETER originates
- Removed all PortCls port creation code (PcNewPort, Port->Init, PcRegisterSubdevice)
- Removed BCE transport initialization
- Only does parameter validation + resource mapping

**Status**: Unknown - waiting for KdPrint logs after reboot

## Key Investigations Performed

### 1. Parameter Issues (RESOLVED)
- **Initial Issue**: MaxObjects=1 was too low for PcAddAdapterDevice
- **Fix**: Changed to MaxObjects=16 (Driver.c:29)
- **Result**: Did not resolve STATUS_INVALID_PARAMETER

### 2. Port Init UnknownAdapter Parameter (TESTED)
- **Initial Issue**: Port->Init called with NULL for UnknownAdapter parameter
- **Fix**: Changed to (PUNKNOWN)miniport (Driver.c:93-94)
- **Result**: Did not resolve STATUS_INVALID_PARAMETER

### 3. Driver File Mismatch (RESOLVED)
- **Issue**: C:\Windows\System32\drivers\T2AudioMiniport.sys was 15872 bytes (old version)
- **Discovery**: Newer build was 23048 bytes
- **Fix**: Manually deleted old file, reinstalled with pnputil
- **Result**: File size now 19968 bytes (latest simplified version), but error persists

### 4. Certificate Store Issues (RESOLVED)
- **Issue**: MSBuild TestCertificate property expects SHA1 thumbprint, not certificate name
- **Attempted Fix**: Set TestCertificate="CA2DE95D1B065F6D6CFF6C4158DC88E2424ABBCE"
- **Result**: MSBuild still failed to auto-sign
- **Workaround**: Disabled MSBuild auto-signing (/p:SignMode=Off), sign manually with signtool

### 5. Catalog File Issues (RESOLVED)
- **Issue**: Inf2Cat.exe failed due to missing SourceDisksNames/SourceDisksFiles in INF
- **Fix**: Added sections to T2AudioMiniport.inf
- **Alternative**: Using makecat.exe with custom CDF file (t2audio.cdf in Temp folder)
- **Current Method**: makecat.exe workflow

### 6. Logging/Debugging Attempts (IN PROGRESS)
- **Tools Used**: DebugView (Sysinternals), Event Viewer, PnP Configuration logs
- **KdPrint Statements**: Added extensive logging throughout Driver.c (lines 13, 17, 32, 36, 53, many more)
- **Registry Setting**: Enabled kernel debug output via Debug Print Filter registry key
- **Problem**: No KdPrint output visible in DebugView or logs
- **Hypothesis**: DriverEntry/StartDevice never called, or KdPrint not working

## Current Status

### What Works
- Driver builds successfully (MSBuild with WDK 10.0.28000.0)
- Driver and catalog sign correctly with test certificate
- Driver installs via pnputil without errors
- Driver file present in C:\Windows\System32\drivers\
- Service registry key created (HKLM:\SYSTEM\CurrentControlSet\Services\T2AudioMiniport)
- Device detected by PnP Manager (shows in Device Manager under "Sound, video and game controllers")

### What Doesn't Work
- Driver fails to start: CM_PROB_FAILED_ADD (0x1F) with STATUS_INVALID_PARAMETER (0xC000000D)
- No evidence of DriverEntry being called (no System log events)
- No KdPrint output captured in DebugView
- Device restart via pnputil requires full system reboot

### BCE Transport Status
- **Implementation**: BceTransport.c complete with IOCTL 0x222018 protocol
- **Current State**: DISABLED in Driver.c (lines 65-79 commented out)
- **Reason**: Isolating driver loading issues from BCE complexity
- **Device ID**: Hardcoded to 0 for testing (context->SpeakerDeviceId = 0)
- **Plan**: Re-enable after basic driver loading works

## Hypotheses for STATUS_INVALID_PARAMETER

### Hypothesis 1: StartDevice Never Called
- **Evidence**: No KdPrint logs, no System event log entries
- **Theory**: PnP Manager rejects driver before calling StartDevice
- **Possible Causes**:
  - INF file incompatibility
  - Wrong device class or class GUID
  - Missing required registry entries
  - Driver signature verification failure (despite test signing enabled)

### Hypothesis 2: T2AudioGetContext Returns NULL
- **Code**: `context = T2AudioGetContext(DeviceObject)` (Driver.c:49)
- **Function**: Wrapper around PcGetDeviceContext() from PortCls
- **Theory**: PcAddAdapterDevice didn't allocate device extension correctly
- **Test**: Current simplified StartDevice checks for NULL context and returns STATUS_INVALID_PARAMETER

### Hypothesis 3: T2AudioMapResources Fails
- **Function**: Maps BAR1 and BAR2 memory regions via MmMapIoSpace
- **Parameters**: Uses ResourceList->Translated resources
- **Theory**: Resource mapping fails due to incorrect BAR cache types or sizes
- **Test**: Current simplified StartDevice will log "MapResources failed" if this is the issue

### Hypothesis 4: ResourceList NULL or Invalid
- **Theory**: PnP Manager passes NULL or malformed ResourceList to StartDevice
- **Evidence**: STATUS_INVALID_PARAMETER is exactly what we'd return if ResourceList == NULL
- **Test**: Current simplified StartDevice explicitly checks ResourceList != NULL

### Hypothesis 5: PortCls Framework Incompatibility
- **Theory**: Modern Windows 11 PortCls expects different initialization sequence
- **Evidence**: Many PortCls drivers use AddDevice model, not PcAddAdapterDevice
- **Alternative**: Implement manual AddDevice with IoCreateDevice + PcNewPort instead of PcAddAdapterDevice
- **Research Needed**: Compare with Microsoft sample drivers (sysvad, msvad)

## Next Steps

### Immediate (After Reboot)
1. Launch DebugView as Administrator
2. Enable Capture -> Capture Kernel in DebugView menu
3. Check Device Manager status: `Get-PnpDevice -FriendlyName "*T2 Audio*"`
4. Look for "T2Audio:" messages in DebugView
5. Check PnP Configuration log: Event Viewer -> Applications and Services Logs -> Microsoft -> Windows -> Kernel-PnP -> Configuration

### If KdPrint Logs Appear
- Identify exact failure point (parameter validation? resource mapping?)
- Add code to fix specific issue
- Rebuild, sign, install, reboot, repeat

### If No KdPrint Logs (Driver Not Loading)
1. **Verify INF correctness**:
   - Compare with working PortCls INF (e.g., sysvad sample)
   - Check Class GUID matches Media class
   - Verify Hardware ID exactly matches device

2. **Try alternative driver model**:
   - Remove PcAddAdapterDevice, implement manual AddDevice
   - Create device object with IoCreateDevice
   - Initialize PortCls manually

3. **Check for missing dependencies**:
   - Verify portcls.sys exists and loads
   - Check if ks.sys required
   - Review dumpbin /dependents output

4. **Test with kernel debugger**:
   - Set up WinDbg kernel debugging over network or serial
   - Set breakpoint on DriverEntry
   - See if function ever called

### If Still Stuck
- **Nuclear option**: Start from Microsoft sysvad sample driver
- Modify sysvad to target PCI\VEN_106B&DEV_1803 device
- Incrementally add T2 Audio-specific code
- This verifies if problem is architectural vs. device-specific

## Relevant Files

### Source Code
- `C:\Users\othysa\Desktop\mbp\T2AudioPort\Phase2\Driver\Driver.c` - Main driver entry points
- `C:\Users\othysa\Desktop\mbp\T2AudioPort\Phase2\Driver\Device.c` - Resource mapping (T2AudioMapResources)
- `C:\Users\othysa\Desktop\mbp\T2AudioPort\Phase2\Driver\WaveRTMiniport.c` - Miniport implementation (not called yet)
- `C:\Users\othysa\Desktop\mbp\T2AudioPort\Phase2\Driver\Phase4.c` - Audio DMA logic (not called yet)
- `C:\Users\othysa\Desktop\mbp\T2AudioPort\Phase2\Driver\BceTransport.c` - BCE IOCTL protocol (disabled)
- `C:\Users\othysa\Desktop\mbp\T2AudioPort\Phase2\T2AudioMiniport.h` - Common header with structures

### Build Files
- `C:\Users\othysa\Desktop\mbp\T2AudioPort\Phase2\T2AudioMiniport.vcxproj` - MSBuild project
- `C:\Users\othysa\Desktop\mbp\T2AudioPort\Phase2\Driver\T2AudioMiniport.inf` - Driver installation INF
- `C:\Users\othysa\AppData\Local\Temp\opencode\t2audio.cdf` - Catalog definition file

### Installed Files
- `C:\Windows\System32\drivers\T2AudioMiniport.sys` - Driver binary (19968 bytes)
- `C:\Windows\INF\oem83.inf` - Installed INF in driver store
- `C:\Windows\System32\CatRoot\{F750E6C3-38EE-11D1-85E5-00C04FC295EE}\t2audiominiport.cat` - Catalog file

### Tools
- `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe` - Build tool
- `C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0\x64\signtool.exe` - Driver signing
- `C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0\x64\makecat.exe` - Catalog creation
- `C:\Users\othysa\AppData\Local\Temp\opencode\Dbgview.exe` - DebugView (Sysinternals)

## Build Commands

### Full Build + Sign + Install Sequence
```powershell
# Build
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" `
  "C:\Users\othysa\Desktop\mbp\T2AudioPort\Phase2\T2AudioMiniport.vcxproj" `
  /p:Configuration=Debug /p:Platform=x64 /p:SignMode=Off /t:Build /v:minimal

# Copy to Driver folder
Copy-Item -LiteralPath "C:\Users\othysa\Desktop\mbp\T2AudioPort\Phase2\bin\Debug\T2AudioMiniport.sys" `
  -Destination "C:\Users\othysa\Desktop\mbp\T2AudioPort\Phase2\Driver\T2AudioMiniport.sys" -Force

# Sign driver
& "C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0\x64\signtool.exe" sign /a /v /s My `
  /n "T2AudioPort Test Certificate" /fd SHA256 /t http://timestamp.digicert.com `
  "C:\Users\othysa\Desktop\mbp\T2AudioPort\Phase2\Driver\T2AudioMiniport.sys"

# Create catalog
& "C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0\x64\makecat.exe" `
  "C:\Users\othysa\AppData\Local\Temp\opencode\t2audio.cdf"

# Sign catalog
& "C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0\x64\signtool.exe" sign /a /v /s My `
  /n "T2AudioPort Test Certificate" /fd SHA256 /t http://timestamp.digicert.com `
  "C:\Users\othysa\Desktop\mbp\T2AudioPort\Phase2\Driver\t2audiominiport.cat"

# Uninstall old driver (if exists)
pnputil /delete-driver oem83.inf /uninstall /force

# Install new driver
pnputil /add-driver "C:\Users\othysa\Desktop\mbp\T2AudioPort\Phase2\Driver\T2AudioMiniport.inf" /install

# Reboot required
Restart-Computer
```

## Registry Keys

### Service Entry
```
HKLM:\SYSTEM\CurrentControlSet\Services\T2AudioMiniport
  Type = 1 (SERVICE_KERNEL_DRIVER)
  Start = 3 (SERVICE_DEMAND_START)
  ErrorControl = 1 (SERVICE_ERROR_NORMAL)
  ImagePath = \SystemRoot\System32\drivers\T2AudioMiniport.sys
```

### Debug Print Filter (for KdPrint output)
```
HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter
  DEFAULT = 0xF (all levels)
```

## Event Log Queries

### Check for driver errors
```powershell
Get-WinEvent -LogName System | Where-Object {$_.ProviderName -like "*T2Audio*"}
Get-WinEvent -LogName System | Where-Object {$_.Message -like "*T2Audio*"}
```

### PnP Configuration log
```
Event Viewer -> Applications and Services Logs -> Microsoft -> Windows -> Kernel-PnP -> Configuration
Filter for Event ID 410 (device problem)
```

### Look for device failure
```powershell
Get-WinEvent -LogName 'Microsoft-Windows-Kernel-PnP/Configuration' -MaxEvents 50 | 
  Where-Object {$_.Message -like "*1803*"} | 
  Format-List TimeCreated, Message
```

## Technical References

### PortCls Documentation
- [PcInitializeAdapterDriver](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-pcinitializeadapterdriver)
- [PcAddAdapterDevice](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-pcaddadapterdevice)
- [PcNewPort](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-pcnewport)
- [IPortWaveRT](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nn-portcls-iportwavert)

### Status Codes
- **STATUS_INVALID_PARAMETER (0xC000000D)**: One or more parameters are incorrect
- **CM_PROB_FAILED_ADD (0x1F)**: Driver failed to start after PnP attempted to add device

### Sample Drivers
- **sysvad**: Microsoft Virtual Audio Device sample (WDK samples)
- **msvad**: Microsoft Virtual Audio Device (older sample, deprecated)

## Debugging Tools

### DebugView Setup
1. Run as Administrator: `C:\Users\othysa\AppData\Local\Temp\opencode\Dbgview.exe`
2. Capture -> Capture Kernel (enable)
3. Capture -> Capture Win32 (enable)
4. Edit -> Filter/Highlight -> Add filter "T2Audio*"
5. Restart device or reboot to see driver messages

### WinDbg Kernel Debugging (Not Yet Set Up)
Would require:
- bcdedit /debug on
- bcdedit /dbgsettings net hostip:x.x.x.x port:50000
- WinDbg preview on another machine
- Network connection for debugging

## Known Issues and Lessons Learned

### FIXED Issues
1. ✅ **STATUS_INVALID_PARAMETER (0xC000000D)** - Root cause: PcAddAdapterDevice arguments swapped
   - MaxObjects and DeviceExtensionSize were in wrong positions
   - Fixed by correcting call order in Driver.c:36-41
   - Verified in binary disassembly (arg4=0x10 in r9d, arg5=0x278 on stack)

### Active Issues
1. ❌ **STATUS_DEVICE_CONFIGURATION_ERROR (0xC0000182)** in T2AudioMapResources
   - Exact failure line unknown (needs granular KdPrint)
   - Hypothesis: Insufficient memory resources from Windows (unproven)
   - Device Manager: ProblemCode 10 (CM_PROB_FAILED_START)

### Verification Lessons Learned
1. **File hash ≠ argument verification** - Must use dumpbin /DISASM to confirm call sites
2. **Build artifacts in packaging/ must be signed separately** - MSBuild auto-signing disabled
3. **KdPrint requires DebugView or WinDbg** - Event Viewer does NOT show DbgPrint output
4. **pnputil /add-driver ≠ device start** - Package can be added but device may fail to start
5. **Device Manager "OK" ≠ working audio** - Endpoints must exist in Sound Control Panel
6. **Reboot may be required** - If driver file is locked, pnputil cannot replace it until reboot

### Build and Signing Process
1. **MSBuild command (VERIFIED 2026-10-05):**
   ```powershell
   & "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" `
     T2AudioMiniport.vcxproj /p:Configuration=Debug /p:Platform=x64
   ```
   Output: `src\bin\Debug\T2AudioMiniport.sys`

2. **Signing commands (NOT RE-VERIFIED after reorganization):**
   ```powershell
   signtool sign /fd SHA256 /t http://timestamp.digicert.com /a `
     /n "T2AudioPort Test Certificate" T2AudioMiniport.sys
   
   Inf2Cat /driver:. /os:10_X64
   
   signtool sign /fd SHA256 /t http://timestamp.digicert.com /a `
     /n "T2AudioPort Test Certificate" t2audiominiport.cat
   ```

3. **Installation (VERIFIED 2026-10-05):**
   ```powershell
   pnputil /add-driver "C:\Users\othysa\Desktop\mbp\T2AudioPort\packaging\T2AudioMiniport.inf" /install
   ```

4. **Device restart (VERIFIED 2026-10-05):**
   ```powershell
   pnputil /restart-device "PCI\VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01\4&3AC8FC3&0&03D8"
   ```

### Rollback Process (NOT TESTED)
**WARNING:** These commands are from previous session notes and have NOT been tested after project reorganization:
```powershell
# Restore AppleAudio.sys from backup
.\tools\Rollback-T2AudioDriver.ps1

# Or manually:
# 1. Stop and disable T2AudioMiniport service
# 2. Rename C:\Windows\System32\drivers\AppleAudio.sys.disabled back to AppleAudio.sys
# 3. Reinstall Apple Boot Camp drivers
```

### Critical Constraints (Do Not Violate)
1. **Do NOT bypass PcAddAdapterDevice** - PortCls framework requires it
2. **Do NOT return fake STATUS_SUCCESS** from failed operations
3. **Do NOT use deviceList[0] fallback** for BCE Speaker selection without name matching
4. **Do NOT assume BAR index from resource descriptor index** - Cache type matching required
5. **Do NOT change build output hash and claim "args verified"** - Use dumpbin disassembly
6. **Do NOT install unverified builds** - Test builds after path changes may differ in debug info
7. **Do NOT skip granular diagnostics** - Add logging before changing strategies

## Success Criteria

When this driver is working, we should see:
1. Device Manager shows "Apple T2 Audio Device" with Status=OK (no yellow triangle)
2. DebugView shows "T2Audio: StartDevice entry" and subsequent messages
3. New audio endpoint appears in Sound Control Panel
4. Eventually: 6-channel audio output to MacBook Pro speakers

## Timeline

### Phase 1-9: Driver Implementation (Pre-2026-10-05)
- BceTransport, WaveRT, streaming interfaces implemented
- INF created, installation automation scripts written
- Multiple test builds with various approaches

### 2026-10-05 Boot Test Session

**20:30 - Reboot and installation:**
- System rebooted after previous session's driver build
- Old driver (22,024 bytes) replaced with new build (28,168 bytes)
- `pnputil /add-driver` executed successfully
- Package assigned as oem16.inf

**20:50 - Boot test execution (VERIFIED):**
- ✅ DebugView kernel capture started (admin, Capture Kernel enabled, filter "T2Audio")
- ✅ Device restarted with `pnputil /restart-device`
- ✅ KdPrint output captured to `logs/boot_20261005_2050_capture.log` (62,587 bytes)
- ✅ STATUS_INVALID_PARAMETER (0xC000000D) **FIXED**
- ✅ DriverEntry → PcInitializeAdapterDriver: SUCCESS (0x00000000)
- ✅ AddDevice → PcAddAdapterDevice: SUCCESS (0x00000000)
- ✅ StartDevice called with valid parameters
- ❌ **NEW BLOCKER:** T2AudioMapResources failed: 0xC0000182 (STATUS_DEVICE_CONFIGURATION_ERROR)

**PcAddAdapterDevice fix details:**
- **Problem:** Arguments 4 and 5 were swapped (MaxObjects vs DeviceExtensionSize)
- **Fix:** Corrected call site in Driver.c:36-41
  - arg4 (MaxObjects) = 16
  - arg5 (DeviceExtensionSize) = 632 (PORT_CLASS_DEVICE_EXTENSION_SIZE + sizeof(T2AUDIO_DEVICE_CONTEXT))
- **Verification:** Disassembly confirmed arg4=0x10 in r9d, arg5=0x278 at [rsp+20h]

**Evidence from DebugView log (lines 9-16):**
```
00000009  29.03139496  System  T2Audio: PortCls driver initialized
00000011  29.03186798  System  T2Audio: PcAddAdapterDevice returned: 0x00000000
00000012  29.06050491  System  T2Audio: StartDevice entry
00000013  29.06050873  System  T2Audio: All parameters valid
00000014  29.06050873  System  T2Audio: Mapping resources
00000015  29.06055641  System  T2Audio: MapResources failed: 0xC0000182
```

**Current installed state:**
- Device Manager: Status=Error, ProblemCode 10 (CM_PROB_FAILED_START)
- Driver: C:\Windows\System32\drivers\T2AudioMiniport.sys
- SHA256: 30867A4BC1794839E400BE83E1D64EE9CF9E86843B61819AB5F6E3B7D5088E61
- Size: 28,168 bytes
- INF: oem16.inf, Version 1.0.0.0

**21:15 - Resource investigation (ATTEMPTED, INCONCLUSIVE):**
- Checked registry: LogConf\BasicConfigVector empty, Resources key absent
- Checked Win32_PnPAllocatedResource: 0 entries for VEN_106B&DEV_1803
- **HYPOTHESIS (not proven):** Windows does not allocate 2 memory BARs to device
- **UNKNOWN:** Exact line in Device.c that returns 0xC0000182
- **UNKNOWN:** Actual value of NumberOfEntriesOfType(CmResourceTypeMemory)
- **UNKNOWN:** Whether ResourceList is NULL or contains wrong types

**21:30 - Project reorganization:**
- Phase1/ → docs/research/ (buffer analysis tools)
- Phase2/ → src/ (driver sources and vcxproj)
- Install/ → tools/ (PowerShell scripts)
- Old directories removed: Backup/, AppleAudio_backup/
- Git repository initialized
- Documentation created: README.md, CURRENT_STATE.md, THIRD_PARTY_LICENSES.md
- .gitignore configured (secrets, artifacts excluded)
- Initial commit: d8510ea "Initial commit: T2AudioPort Windows driver"

**21:39 - Diagnostic build (COMPLETED):**
- Added granular KdPrint to T2AudioMapResources with FAIL_A through FAIL_N labels
- Added logging: memory resource count, BAR addresses, GPR values, BufferStruct metadata
- MSBuild successful (0 errors, warnings expected)
- Output: src\bin\Debug\T2AudioMiniport.sys (25,088 bytes unsigned)
- Signed: packaging\T2AudioMiniport.sys (32,264 bytes)
- SHA256: 5FE84ED9CDEA42870EA1F79FFB5CB116A7EA912A1B62970F8E38287CC8B033A9
- Commit: a4e1bc7, pushed to diagnostics branch

**21:41 - First installation attempt (FAILED - wrong driver loaded):**
- pnputil reported "Driver package is up-to-date" but did NOT replace SYS file
- Device restart captured old driver output (no diagnostic messages)
- Confirmed: C:\Windows\System32\drivers\T2AudioMiniport.sys still 28,168 bytes (old hash)

**21:46 - Manual driver replacement (SUCCESS):**
- Disabled device with pnputil /disable-device
- Copied diagnostic SYS to System32\drivers (32,264 bytes)
- Enabled device with pnputil /enable-device
- Verified hash: 5FE84ED9CDEA42870EA1F79FFB5CB116A7EA912A1B62970F8E38287CC8B033A9

**21:47 - Diagnostic capture (ROOT CAUSE IDENTIFIED):**
- DebugView restarted, device restarted
- **Full diagnostic output captured** to diagnostic_20261005_214749_SUCCESS.log
- ✅ **Memory resource count: 3** (not 0 or 1 — hypothesis of missing resources DISPROVEN)
- ✅ **BAR1: Physical 0xC1000000, Length 0x400000** (4 MB) — mapped successfully
- ✅ **BAR2: Physical 0xC1680000, Length 0x80000** (512 KB) — mapped successfully
- ❌ **GPR read: version=0xFFFFFFFF, signature=0xFFFFFFFF, bufferOffset=0xFFFFFFFF**
- ❌ **FAIL_K: GPR signature 0xFFFFFFFF != 0x19870423**
- **Conclusion**: Hardware NOT responding at offset 0xC000, or device not initialized

**21:49 - Root cause analysis completed:**
- Created docs/MAPRESOURCES_ROOT_CAUSE.md with full analysis
- Commit: 14a8e0a, pushed to diagnostics branch
- Updated DEBUGGING_LOG.md with confirmed hardware configuration

### Verified Facts (2026-10-05 21:47 UTC)

✅ Windows allocates 3 memory resources to PCI\VEN_106B&DEV_1803  
✅ BAR1 and BAR2 map successfully with MmMapIoSpaceEx  
✅ Driver executes through StartDevice to GPR register read  
❌ GPR registers at offset 0xC000 return all-ones (0xFFFFFFFF)  
❌ Either T2AUDIO_GPR_OFFSET is incorrect, or T2 requires initialization

### Next Actions (AFTER Root Cause Confirmation)

**Option 1 - Search BAR2 for signature (RECOMMENDED):**
- Scan BAR2 from 0x0000 to 0x80000 for signature 0x19870423
- Log offset where found
- Update T2AUDIO_GPR_OFFSET if found at different location

**Option 2 - Dump BAR2 contents:**
- Log first 1-4 KB of BAR2 to check if ANY non-0xFF data exists
- If all 0xFF: hardware not responding, may need BCE initialization

**Option 3 - Try different cache types:**
- Test BAR2 with PAGE_WRITECOMBINE instead of PAGE_NOCACHE
- Compare with AppleAudio.sys cache settings

**Option 4 - Check T2 power state:**
- Query PCI configuration space for power management capabilities
- Verify device is in D0 (fully powered) state

**Do NOT execute without new data:**
- Do NOT hardcode different GPR offset without evidence
- Do NOT add BCE initialization without understanding requirements
- Do NOT skip signature search before trying alternative approaches
