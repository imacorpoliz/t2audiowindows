# Current State - T2AudioPort Driver

**Last Updated:** 2026-10-05 21:25 UTC  
**Last Verified Build:** SHA256 `30867A4BC1794839E400BE83E1D64EE9CF9E86843B61819AB5F6E3B7D5088E61`

## What Works (Verified by Boot Test)

### ✅ PortCls Integration
- `DriverEntry` → `PcInitializeAdapterDriver`: **SUCCESS (0x00000000)**
- `T2AudioAddDevice` → `PcAddAdapterDevice`: **SUCCESS (0x00000000)**
- `T2AudioStartDevice` called with valid parameters
- **PcAddAdapterDevice arguments verified in binary:**
  - arg4 (MaxObjects) = 16 (0x10 in r9d register)
  - arg5 (DeviceExtensionSize) = 632 bytes (0x278 on stack [rsp+20h])
  - PORT_CLASS_DEVICE_EXTENSION_SIZE = 512 bytes
  - sizeof(T2AUDIO_DEVICE_CONTEXT) = 120 bytes
- Device context allocation and offset calculation correct
- KdPrint debug output captured successfully via DebugView

### ✅ STATUS_INVALID_PARAMETER (0xC000000D) FIXED
Root cause was swapped arguments to PcAddAdapterDevice. Fixed by correcting call site in `Driver.c:36-41`.

## Current Blocker

### ❌ STATUS_DEVICE_CONFIGURATION_ERROR (0xC0000182)
**Location:** `T2AudioMapResources` (Device.c)  
**Symptom:** Driver reaches StartDevice but fails during hardware resource mapping

**Evidence from DebugView log:**
```
00000012  29.06050491  System  T2Audio: StartDevice entry
00000013  29.06050873  System  T2Audio: All parameters valid
00000014  29.06050873  System  T2Audio: Mapping resources
00000015  29.06055641  System  T2Audio: MapResources failed: 0xC0000182
```

**Root Cause (Suspected):**
`NumberOfEntriesOfType(CmResourceTypeMemory) < 2` — Windows does not allocate 2 memory BARs required for:
- BAR1: Audio buffer access (cache type 0x404)
- BAR2: Configuration/GPR registers (cache type 0x204)

**Supporting Evidence:**
- Device Manager: ProblemCode 10 (CM_PROB_FAILED_START)
- Registry: `LogConf\BasicConfigVector` empty
- Registry: `Resources` key absent
- Win32_PnPAllocatedResource: 0 entries for VEN_106B&DEV_1803

## What's Implemented But Disabled

The following code exists but is **explicitly disabled** in the test build to isolate the boot issue:

- **BCE Transport** (`BceTransport.c`): T2 co-processor communication via `\Device\AppleUSBVHCI`
- **WaveRT Port Creation** (`Driver.c:T2AudioStartDevice`): `PcNewPort(IID_IPortWaveRT)` commented out
- **Audio Endpoint Registration** (`Driver.c`): `PcRegisterSubdevice` commented out
- **Speaker Device Discovery** (`BceTransport.c`): `T2AudioFindSpeakerDeviceId` returns STATUS_NOT_IMPLEMENTED
- **Audio I/O Commands** (`Phase4.c`): `T2AudioStartIo` / `T2AudioStopIo` blocked by `SpeakerDeviceId == 0`

These features will be re-enabled **after** the resource mapping issue is resolved.

## Next Steps (Not Yet Executed)

1. **Granular diagnostic in T2AudioMapResources:**
   - Add KdPrint before each `return STATUS_DEVICE_CONFIGURATION_ERROR` in Device.c
   - Rebuild, reinstall, capture exact failure line
   - Log actual value of `NumberOfEntriesOfType(CmResourceTypeMemory)`

2. **Investigate resource allocation:**
   - Research how AppleAudio.sys handles this device (does it use ResourceList?)
   - Check if INF needs explicit `LogConfig` directive to request BARs
   - Explore alternative: direct PCI config space access via `HalGetBusDataByOffset`

3. **If resource allocation cannot be fixed:**
   - Rewrite `T2AudioMapResources` to bypass RESOURCE_LIST
   - Use `IoGetDeviceProperty(DevicePropertyBusTypeGuid)` to confirm PCI
   - Read BAR addresses directly from PCI config space (offsets 0x10, 0x14)
   - Map physical addresses manually via `MmMapIoSpaceEx`

## Files & Evidence

**Installed Package:**
- INF: `oem16.inf`
- SYS: `C:\Windows\System32\drivers\T2AudioMiniport.sys`
- SHA256: `30867A4BC1794839E400BE83E1D64EE9CF9E86843B61819AB5F6E3B7D5088E61`
- Size: 28,168 bytes

**Test Logs:**
- Full boot test report: `docs/logs/BOOT_TEST_20261005.md`
- DebugView kernel capture: `docs/logs/boot_20261005_2050_capture.log`
- Debugging history: `docs/DEBUGGING_LOG.md`

**Source Code:**
- Working directory: `src/`
- Build project: `src/T2AudioMiniport.vcxproj`
- Signed package: `packaging/T2AudioMiniport.{sys,inf,cat}`

## Important Notes

- **Do NOT modify driver logic until resource issue is diagnosed**
- **Do NOT re-enable BCE/WaveRT before StartDevice succeeds**
- **Do NOT bypass PortCls framework**
- Changing file hash ≠ verifying binary arguments (use dumpbin /DISASM)
- KdPrint requires DebugView or WinDbg, NOT Event Viewer
- Old packages in `docs/archive/` marked unfit; do not reuse
