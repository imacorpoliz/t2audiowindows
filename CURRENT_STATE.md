# Current State - T2AudioPort Driver

**Last Updated:** 2026-10-05 22:30 UTC (Session handoff)  
**Project Root:** `C:\Users\othysa\Desktop\mbp\T2AudioPort`  
**Git Branch:** `master` (local, no remote configured)  
**Last Commit:** `d8510ea` (Initial commit: T2AudioPort Windows driver)

---

## Project Goal

Native Windows WDM audio driver (PortCls miniport) for Apple T2 chip audio (PCI\VEN_106B&DEV_1803) on MacBook Pro 16,1 (2019). Target: 6-channel speaker array at 48kHz/24-bit.

**Current Stage:** Boot test phase — isolating driver load issues before enabling audio functionality.

---

## Hardware & System

- **Model:** MacBookPro16,1 (2019)
- **OS:** Windows 11 (build unknown)
- **Boot Config:** `testsigning=Yes`, `nointegritychecks=Yes`, `loadoptions=DISABLE_INTEGRITY_CHECKS`
- **Target Device:** Apple T2 Audio (PCI\VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01)
- **Device Instance ID:** `PCI\VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01\4&3AC8FC3&0&03D8`

---

## Installed Driver Status (VERIFIED 2026-10-05 22:15 UTC)

**Installed Package:**
- **INF:** `oem16.inf`
- **Driver Version:** 1.0.0.0
- **System File:** `C:\Windows\System32\drivers\T2AudioMiniport.sys`
- **SHA256:** `30867A4BC1794839E400BE83E1D64EE9CF9E86843B61819AB5F6E3B7D5088E61`
- **Size:** 28,168 bytes

**Device Manager Status:**
- **Status:** Error
- **Problem Code:** 10 (CM_PROB_FAILED_START)
- **Friendly Name:** "Apple T2 Audio Device (6-channel Native Driver)"

**Package in Repository:**
- **Location:** `packaging/T2AudioMiniport.{sys,inf,cat}`
- **SYS SHA256:** `30867A4BC1794839E400BE83E1D64EE9CF9E86843B61819AB5F6E3B7D5088E61` (matches System32)
- **Status:** Signed with test certificate (CN=T2AudioPort Test Certificate, thumbprint CA2DE95D...)

---

## What Works (VERIFIED by Boot Test 2026-10-05 20:50 UTC)

### ✅ PortCls Integration
**Evidence:** `docs/logs/boot_20261005_2050_capture.log` (DebugView kernel capture, lines 9-16)

```
00000009  29.03139496  System  T2Audio: PortCls driver initialized
00000011  29.03186798  System  T2Audio: PcAddAdapterDevice returned: 0x00000000
```

- ✅ `DriverEntry` → `PcInitializeAdapterDriver`: **SUCCESS (0x00000000)**
- ✅ `T2AudioAddDevice` → `PcAddAdapterDevice`: **SUCCESS (0x00000000)**
- ✅ `T2AudioStartDevice` called with valid parameters

### ✅ PcAddAdapterDevice Arguments (VERIFIED by dumpbin 2026-10-05)
**Binary disassembly confirms:**
- arg1 (DriverObject): `rcx` from `[rsp+50h]`
- arg2 (PhysicalDeviceObject): `rdx` from `[rsp+58h]`
- arg3 (StartDevice): `r8` = `T2AudioStartDevice` function pointer
- arg4 (MaxObjects): `r9d` = **0x10** (16 decimal)
- arg5 (DeviceExtensionSize): `[rsp+20h]` = **0x278** (632 decimal)

**Constants:**
- `PORT_CLASS_DEVICE_EXTENSION_SIZE` = 512 bytes (64 * sizeof(ULONG_PTR) on x64)
- `sizeof(T2AUDIO_DEVICE_CONTEXT)` = 120 bytes
- Sum: 512 + 120 = 632 ✓

**Context Offset:** `T2AudioGetContext` adds **0x200** (512) to DeviceExtension pointer (verified at address 0x1400010DA)

### ✅ STATUS_INVALID_PARAMETER (0xC000000D) FIXED
**Root cause (fixed in this build):** Arguments to `PcAddAdapterDevice` were swapped (MaxObjects and DeviceExtensionSize reversed). Corrected in `src/Driver.c:36-41`.

---

## Current Blocker (VERIFIED 2026-10-05 20:50 UTC)

### ❌ STATUS_DEVICE_CONFIGURATION_ERROR (0xC0000182)

**Location:** `T2AudioMapResources` (`src/Device.c`)  
**NTSTATUS:** `0xC0000182` (STATUS_DEVICE_CONFIGURATION_ERROR)

**Evidence from DebugView log (line 16):**
```
00000015  29.06055641  System  T2Audio: MapResources failed: 0xC0000182
```

**Full sequence:**
```
00000012  29.06050491  System  T2Audio: StartDevice entry
00000013  29.06050873  System  T2Audio: All parameters valid
00000014  29.06050873  System  T2Audio: Mapping resources
00000015  29.06055641  System  T2Audio: MapResources failed: 0xC0000182
```

**What is KNOWN:**
- `T2AudioStartDevice` successfully enters and validates parameters
- `T2AudioMapResources` is called
- Function returns `STATUS_DEVICE_CONFIGURATION_ERROR` after ~5ms

**What is UNKNOWN:**
- **Exact line in Device.c that returns the error** (no granular KdPrint)
- **Actual value of `NumberOfEntriesOfType(CmResourceTypeMemory)`**
- Whether ResourceList is NULL or contains wrong resource types
- Whether PortCls provides resources at all for this device

**HYPOTHESIS (NOT PROVEN):**
`NumberOfEntriesOfType(CmResourceTypeMemory) < 2` — Windows does not allocate 2 memory BARs. But this is speculation until granular diagnostic is added.

**Circumstantial Evidence (not definitive):**
- Device Manager: ProblemCode 10 (CM_PROB_FAILED_START) — confirms driver returned error
- Registry `LogConf\BasicConfigVector`: empty (checked 2026-10-05)
- Registry `Resources` key: absent (checked 2026-10-05)
- Win32_PnPAllocatedResource: 0 entries for VEN_106B&DEV_1803
- **NOTE:** These observations show no *runtime* allocated resources but do not prove what PortCls passes to StartDevice IRP.

---

## What's Implemented But DISABLED

The following code exists in `src/` but is **explicitly disabled** to isolate boot issues:

| Component | File | Status | Blocker |
|-----------|------|--------|---------|
| BCE Transport | `BceTransport.c` | NOT CALLED | `T2AudioOpenBceTransport()` commented out in StartDevice |
| WaveRT Port | `Driver.c` | NOT CREATED | `PcNewPort(IID_IPortWaveRT)` commented out |
| Audio Endpoint | `Driver.c` | NOT REGISTERED | `PcRegisterSubdevice()` commented out |
| Speaker Discovery | `BceTransport.c` | STUBBED | `T2AudioFindSpeakerDeviceId` returns `STATUS_NOT_IMPLEMENTED` |
| Audio I/O | `Phase4.c` | BLOCKED | `T2AudioStartIo/StopIo` check `SpeakerDeviceId == 0` and exit |

**Re-enable order:** Fix MapResources → Enable BCE → Enable WaveRT → Test audio I/O

---

## Next Diagnostic Step (NOT YET EXECUTED)

### Step 1: Granular MapResources Logging

**Goal:** Identify exact failure line in `T2AudioMapResources` (`src/Device.c`)

**Changes needed:**
1. Add `KdPrint` before **each** `return STATUS_DEVICE_CONFIGURATION_ERROR` in Device.c
2. Log values: `NumberOfEntriesOfType(CmResourceTypeMemory)`, `Bar1Physical.QuadPart`, `Bar2Physical.QuadPart`
3. Rebuild, sign, reinstall
4. Capture DebugView log during device restart
5. **Do NOT change logic** — only add logging

**Example:**
```c
ULONG memCount = ResourceList->NumberOfEntriesOfType(CmResourceTypeMemory);
KdPrint(("T2Audio: Found %u memory resources\n", memCount));
if (memCount < 2) {
    KdPrint(("T2Audio: Insufficient memory resources (need 2)\n"));
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}
```

### Step 2: If Resources Are Absent
- Research how `AppleAudio.sys` accesses hardware (does it use ResourceList or direct PCI config?)
- Check if INF needs `LogConfig` directive
- Consider direct PCI config space access via `HalGetBusDataByOffset`

### Step 3: If Resources Are Present But Wrong
- Verify cache type matching logic
- Check BAR size validation
- Investigate MmMapIoSpaceEx failure

---

## Critical Technical Constraints

**DO NOT violate these without explicit user approval:**

1. **PcAddAdapterDevice calling convention:**
   - arg4 = MaxObjects (not DeviceExtensionSize)
   - arg5 = DeviceExtensionSize (not MaxObjects)
   - Must use `PORT_CLASS_DEVICE_EXTENSION_SIZE + sizeof(T2AUDIO_DEVICE_CONTEXT)`

2. **Device context layout:**
   - PortCls reserves first `PORT_CLASS_DEVICE_EXTENSION_SIZE` (512) bytes
   - Custom context starts at `DeviceExtension + 512`
   - Use `sizeof()` and WDK constants, not hardcoded values

3. **PortCls framework:**
   - Do NOT bypass `PcAddAdapterDevice`
   - Do NOT return fake `STATUS_SUCCESS` from failed operations
   - Do NOT manually register WDM dispatch routines (PortCls handles this)

4. **BCE Speaker selection:**
   - Do NOT use `deviceList[0]` fallback
   - Speaker device ID must be discovered via BCE `GET_PROPERTY` (not yet implemented)
   - BufferStruct name matching ("Speaker") does NOT provide BCE DeviceId

5. **Verification requirements:**
   - Hash change ≠ proof of correct binary arguments (use `dumpbin /DISASM`)
   - Adding package to DriverStore ≠ successful device start
   - KdPrint requires DebugView or WinDbg, NOT Event Viewer
   - Device Manager "OK" status ≠ working audio (endpoints must exist)

6. **Diagnostic discipline:**
   - Add granular logging BEFORE changing MapResources strategy
   - One hypothesis at a time (don't combine PCI config + INF + fallback in one change)
   - Capture exact NTSTATUS at failure point before proposing fixes

---

## Project File Locations

**Source Code:**
- `src/Driver.c` — DriverEntry, AddDevice, StartDevice (MapResources called here)
- `src/Device.c` — T2AudioMapResources, T2AudioUnmapResources, T2AudioFindSpeakerBuffer
- `src/BceTransport.c` — BCE protocol (disabled)
- `src/Phase4.c` — START_IO/STOP_IO commands (disabled)
- `src/WaveRTMiniport.c` — WaveRT miniport interface (disabled)
- `src/WaveRTStream.c` — Stream implementation (disabled)
- `src/T2AudioMiniport.h` — Shared types and constants
- `src/T2AudioMiniport.vcxproj` — MSBuild project (Debug|x64 configuration)

**Build Output:**
- `src/bin/Debug/T2AudioMiniport.sys` — Latest build (20,992 bytes, hash differs from verified package)
- **DO NOT INSTALL** — paths changed after initial verification

**Verified Package (INSTALL THIS):**
- `packaging/T2AudioMiniport.sys` — SHA256 30867A4B... (28,168 bytes)
- `packaging/T2AudioMiniport.inf` — Driver manifest
- `packaging/t2audiominiport.cat` — Signed catalog

**Tools:**
- `tools/Install-T2AudioDriver.ps1` — Installation script (UNVERIFIED, uses absolute paths)
- `tools/Rollback-T2AudioDriver.ps1` — Restore AppleAudio.sys (UNVERIFIED)
- `tools/PreInstall-Check.ps1`, `tools/PostInstall-Validate.ps1` — Diagnostic scripts

**Documentation:**
- `README.md` — Project overview, build instructions
- `CURRENT_STATE.md` — This file (authoritative status)
- `DEBUGGING_LOG.md` — Session-by-session history
- `THIRD_PARTY_LICENSES.md` — kaiT2en attribution
- `docs/logs/BOOT_TEST_20261005.md` — Full boot test report
- `docs/logs/boot_20261005_2050_capture.log` — DebugView kernel capture (62,587 bytes)
- `docs/research/` — Phase1 buffer analysis tools (BufferStructParser.c, Phase1Test.c)
- `docs/archive/` — Outdated status documents (PROJECT_COMPLETE.md is OBSOLETE)

**Local Only (excluded from Git):**
- `local_backups/AppleAudio.sys.*.bak` — Original Apple driver backups
- `local_artifacts/` — Old .obj files and .sys versions
- `T2AudioCert.pfx` — Private signing key (root directory, DO NOT COMMIT)
- `src/bin/`, `src/obj/` — Build artifacts

**Third-Party Attribution:**
- **kaiT2en** (Linux T2 driver): https://github.com/kekrby/linux-t2
- Used for: BCE protocol constants, message types
- License: GPL-2.0 (this project is clean-room reimplementation, no GPL code copied)
- Specific commit/version: UNKNOWN (not recorded in earlier sessions)

---

## Build Instructions (VERIFIED 2026-10-05)

**Prerequisites:**
- Visual Studio 2022 (v143 toolset, "18" branding)
- Windows Driver Kit 10.0.28000.0
- MSBuild: `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`

**Build command:**
```powershell
cd C:\Users\othysa\Desktop\mbp\T2AudioPort\src
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" `
  T2AudioMiniport.vcxproj /p:Configuration=Debug /p:Platform=x64
```

**Output:** `src\bin\Debug\T2AudioMiniport.sys`

**Signing (UNVERIFIED — commands not tested this session):**
```powershell
cd packaging
signtool sign /fd SHA256 /t http://timestamp.digicert.com /a `
  /n "T2AudioPort Test Certificate" T2AudioMiniport.sys

Inf2Cat /driver:. /os:10_X64

signtool sign /fd SHA256 /t http://timestamp.digicert.com /a `
  /n "T2AudioPort Test Certificate" t2audiominiport.cat
```

**Installation (VERIFIED 2026-10-05):**
```powershell
pnputil /add-driver "C:\Users\othysa\Desktop\mbp\T2AudioPort\packaging\T2AudioMiniport.inf" /install
```

**Debug capture (VERIFIED 2026-10-05):**
1. Download DebugView: https://live.sysinternals.com/Dbgview.exe
2. Run as Administrator
3. Capture > Capture Kernel (Ctrl+K)
4. Edit > Filter/Highlight: "T2Audio"
5. Restart device: `pnputil /restart-device "PCI\VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01\4&3AC8FC3&0&03D8"`

**Rollback (UNVERIFIED):**
```powershell
# Restore Apple driver (if backup exists)
.\tools\Rollback-T2AudioDriver.ps1
```

---

## Git Status

**Repository:** Initialized (2026-10-05)  
**Branch:** `master` (local only, no remote)  
**Last Commit:** `d8510ea` — "Initial commit: T2AudioPort Windows driver"  
**Uncommitted Changes:**
- Modified: `.gitignore` (added `src/bin/`, `src/obj/`)
- Untracked: `packaging/T2AudioMiniport.sys`, `packaging/t2audiominiport.cat`

**Not in Git (by design):**
- `T2AudioCert.pfx` (private key)
- `local_backups/` (Apple driver backups)
- `local_artifacts/` (old build outputs)
- `src/bin/`, `src/obj/` (build artifacts)

**To push to GitHub:**
1. User must provide repository URL
2. Create remote: `git remote add origin <URL>`
3. Create working branch: `git checkout -b diagnostics`
4. Commit pending changes
5. Push: `git push -u origin diagnostics`

---

## Known Gaps in Documentation

1. **kaiT2en version/commit:** Not recorded. Need to document exact source for protocol constants.
2. **AppleAudio.sys research:** No documented comparison of resource usage (how does Apple driver access hardware?).
3. **PowerShell scripts:** Not tested after reorganization. Absolute paths need conversion to `$PSScriptRoot`-relative.
4. **Inf2Cat/signtool commands:** Recorded from previous session but not re-tested after file moves.
5. **Windows 11 build number:** Not captured (only "Windows 11" known).
6. **C4152 warnings:** Vtable pointer conversion warnings not resolved (present in WaveRTStream.c:230-240).

---

## CRITICAL: Do Not Assume Success

- **"Driver loads"** ≠ **"Driver works"** — DriverEntry succeeds but StartDevice fails
- **"Package installed"** ≠ **"Device started"** — oem16.inf assigned but ProblemCode 10
- **"Hash changed"** ≠ **"Args fixed"** — Must verify binary disassembly
- **"BUILD COMPLETE"** in archive docs is **OBSOLETE** — Ignore docs/archive/PROJECT_COMPLETE.md

The driver is **NOT COMPLETE**. It loads but cannot access hardware. Audio endpoints do not exist. No sound output.
