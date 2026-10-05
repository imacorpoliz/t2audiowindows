# Session Handoff - Quick Start for Next Agent

**Date:** 2026-10-05 22:45 UTC  
**Project:** T2AudioPort - Windows Audio Driver for Apple T2  
**Location:** `C:\Users\othysa\Desktop\mbp\T2AudioPort`

---

## Read These Files First

1. **CURRENT_STATE.md** — Authoritative project status (verified facts, current blocker, next steps)
2. **README.md** — Project overview, structure, build instructions
3. **docs/DEBUGGING_LOG.md** — Session history, timeline, lessons learned
4. **docs/logs/BOOT_TEST_20261005.md** — Full boot test report with evidence

---

## Critical Facts (Verified 2026-10-05)

### ✅ What Works
- **DriverEntry** → PcInitializeAdapterDriver: SUCCESS (0x00000000)
- **AddDevice** → PcAddAdapterDevice: SUCCESS (0x00000000)
- **STATUS_INVALID_PARAMETER (0xC000000D) FIXED** by correcting PcAddAdapterDevice args
- KdPrint successfully captured via DebugView

### ❌ Current Blocker
- **T2AudioMapResources** returns `STATUS_DEVICE_CONFIGURATION_ERROR (0xC0000182)`
- Device Manager: ProblemCode 10 (CM_PROB_FAILED_START)
- Exact failure line in Device.c: **UNKNOWN** (needs granular KdPrint)
- Root cause: **HYPOTHESIS ONLY** (suspected missing PCI resources, not proven)

### 📦 Installed Package
- **Path:** `C:\Windows\System32\drivers\T2AudioMiniport.sys`
- **SHA256:** `30867A4BC1794839E400BE83E1D64EE9CF9E86843B61819AB5F6E3B7D5088E61`
- **Size:** 28,168 bytes
- **INF:** oem16.inf
- **Status:** Installed but device fails to start

---

## Next Action (Ready to Execute)

**Add granular diagnostic logging to T2AudioMapResources:**

1. Edit `src/Device.c`
2. Add KdPrint statements before EACH `return STATUS_DEVICE_CONFIGURATION_ERROR`
3. Log these values:
   - `NumberOfEntriesOfType(CmResourceTypeMemory)`
   - `Bar1Physical.QuadPart`, `Bar2Physical.QuadPart`
   - Any intermediate validation results
4. Rebuild: `MSBuild src\T2AudioMiniport.vcxproj /p:Configuration=Debug /p:Platform=x64`
5. Sign with signtool (commands in CURRENT_STATE.md)
6. Install: `pnputil /add-driver packaging\T2AudioMiniport.inf /install`
7. Capture DebugView output during device restart
8. Identify exact failure line

**DO NOT:**
- Change MapResources logic before diagnostic
- Bypass PortCls or return fake STATUS_SUCCESS
- Add INF LogConfig without evidence it's needed
- Rewrite for direct PCI config access without proof

---

## File Locations

**Sources:** `src/*.c`, `src/*.h`, `src/T2AudioMiniport.vcxproj`  
**Verified Package:** `packaging/T2AudioMiniport.{sys,inf,cat}` (use this for installation)  
**Boot Test Log:** `docs/logs/boot_20261005_2050_capture.log` (62KB, lines 9-16 show success/failure)  
**Tools:** `tools/*.ps1` (installation scripts, NOT TESTED after reorganization)

---

## Git Status

**Branch:** master (local, no remote)  
**Commits:**
- `2b0e14e` — Session handoff: Update documentation with boot test results
- `d8510ea` — Initial commit: T2AudioPort Windows driver

**Untracked:** `packaging/T2AudioMiniport.sys`, `packaging/t2audiominiport.cat` (signed binaries, not yet committed)

**To push to GitHub:**
1. User must provide repository URL
2. `git remote add origin <URL>`
3. `git push -u origin master`

---

## Hardware Details

- **Model:** MacBookPro16,1 (2019)
- **Device:** Apple T2 Audio (PCI\VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01)
- **Instance ID:** `PCI\VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01\4&3AC8FC3&0&03D8`
- **Boot Config:** testsigning=Yes, nointegritychecks=Yes

---

## Important Constraints

1. **PcAddAdapterDevice signature:**
   - arg4 = MaxObjects (16)
   - arg5 = DeviceExtensionSize (632 = 512 + 120)

2. **Context offset:** DeviceExtension + 512 (PORT_CLASS_DEVICE_EXTENSION_SIZE)

3. **Verification method:** Use `dumpbin /DISASM`, not just hash comparison

4. **KdPrint capture:** DebugView (admin, Capture Kernel), NOT Event Viewer

5. **Do NOT assume success:** Build success ≠ device start ≠ working audio

---

## Known Gaps

1. **kaiT2en version:** Not recorded (need to document exact commit/version for protocol constants)
2. **AppleAudio.sys research:** No documented comparison (how does it access hardware?)
3. **PowerShell scripts:** Absolute paths need conversion to `$PSScriptRoot`-relative
4. **Signing commands:** Not re-tested after file reorganization
5. **Exact MapResources failure:** Unknown until granular KdPrint added

---

## Session Summary

**Completed this session:**
- ✅ Boot test executed (STATUS_INVALID_PARAMETER fixed, MapResources 0xC0000182 identified)
- ✅ Binary disassembly verified PcAddAdapterDevice args
- ✅ Project reorganized (Phase1→docs/research, Phase2→src, Install→tools)
- ✅ Git initialized, 2 commits created
- ✅ Documentation prepared for handoff (CURRENT_STATE, DEBUGGING_LOG updated)

**NOT completed:**
- ❌ MapResources diagnostic (needs code change + rebuild + test)
- ❌ Root cause of 0xC0000182 (hypothesis only, not proven)
- ❌ PowerShell script path updates (still use absolute paths)
- ❌ GitHub remote configuration (user must provide URL)

---

## Quick Commands

**Build:**
```powershell
cd C:\Users\othysa\Desktop\mbp\T2AudioPort\src
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" T2AudioMiniport.vcxproj /p:Configuration=Debug /p:Platform=x64
```

**Install:**
```powershell
pnputil /add-driver "C:\Users\othysa\Desktop\mbp\T2AudioPort\packaging\T2AudioMiniport.inf" /install
```

**Restart device:**
```powershell
pnputil /restart-device "PCI\VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01\4&3AC8FC3&0&03D8"
```

**DebugView capture:**
1. Run as admin
2. Capture > Capture Kernel
3. Filter: "T2Audio"

---

**Driver is NOT complete. It loads but cannot access hardware. No audio output yet.**
