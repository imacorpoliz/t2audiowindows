# T2AudioPort Driver - Current State

**Last Updated**: 2026-10-05 22:17 UTC  
**Status**: ✅ **Phase 3 Complete - WaveRT Registered**  
**Branch**: diagnostics

---

## Executive Summary

**Phase 3 SUCCESS**: WaveRT port and miniport successfully registered. Driver completes StartDevice without errors. KSCATEGORY_AUDIO interface created. BCE transport remains disabled.

**Phase 2 SUCCESS** (maintained): Resource mapping works correctly. GPR signature valid. Speaker buffer located.

**User-visible audio endpoint NOT created** — expected for WaveRT-only driver without topology port (future work).

---

## Current Driver Status

| Component | Status | Notes |
|-----------|--------|-------|
| DriverEntry | ✅ Working | PcInitializeAdapterDriver succeeds |
| AddDevice | ✅ Working | PcAddAdapterDevice succeeds |
| StartDevice | ✅ Working | Returns STATUS_SUCCESS |
| MapResources | ✅ Working | Resource[2] validated, GPR valid |
| FindSpeakerBuffer | ✅ Working | Buffer at 0x12C000, size 0x61800 |
| WaveRT Port | ✅ Created | PcNewPort(&CLSID_PortWaveRT) success |
| WaveRT Miniport | ✅ Registered | Port->Init() and PcRegisterSubdevice() success |
| KSCATEGORY_AUDIO | ✅ Registered | Interface in DeviceClasses registry |
| Topology Port | ❌ Not implemented | Required for user-visible endpoint |
| Audio Endpoint | ❌ Not created | Requires topology port |
| BCE Transport | 🚫 Disabled | SpeakerDeviceId = 0 (by design) |
| Audio Playback | ❌ Not tested | No endpoint available |

---

## Hardware Resources (Confirmed)

| Windows Resource | Physical Address | Size | Purpose |
|------------------|------------------|------|---------|
| Resource[0] | 0xC1000000 | 4 MB | Audio buffers (kaiT2en BAR0) |
| Resource[2] | 0xC1670000 | 64 KB | **Config memory** (kaiT2en BAR4) ✅ |
| Resource[1] | 0xC1680000 | 512 KB | Unknown (not used) |

**Key finding**: Windows resource index ≠ PCI BAR slot index due to 64-bit BAR pairing.

**Note on BAR identification**: Resource[2] selection validated **experimentally** (GPR signature test). Mapping to physical PCI BAR4 is hypothesis based on kaiT2en reference but not independently verified through PCI config space read.

---

## GPR Registers (Valid)

From Resource[2] + 0xC000:

| Register | Value | Expected | Status |
|----------|-------|----------|--------|
| Version | 0x00000003 | ≥ 2 | ✅ Valid |
| Signature | 0x19870423 | 0x19870423 | ✅ Match |
| Buffer Offset | 0x00004000 | Variable | ✅ Valid |

**BufferStruct location**: Resource[2] + 0x4000 (16 KB offset)

---

## Speaker Buffer Metadata

| Property | Value |
|----------|-------|
| Device Name | "Speaker" |
| Device Index | 1 (out of 5 total devices) |
| Output Streams | 1 |
| Buffers per Stream | 1 |
| Buffer Offset (in BAR0) | 0x12C000 (1,228,800 bytes) |
| Buffer Size | 0x61800 (399,360 bytes ≈ 390 KB) |
| Physical Address | 0xC112C000 |

---

## Installed Driver

**File**: C:\Windows\System32\drivers\T2AudioMiniport.sys  
**SHA256**: 220B956584C9223B90A48DD219FBD0CABB2EA5F5D1D4FB291CE2C7830DEF0C31  
**Size**: 33,288 bytes (signed)  
**INF**: oem16.inf  
**Date**: 2026-10-05 22:06 UTC

**Changes from previous version**:
- Added Resource[2] test before Resource[1]
- Automatic selection of correct config resource
- Detailed logging of all resources and GPR values

---

## Log Evidence

From `diagnostic_20261005_220710_SUCCESS.log` (lines 16-31):

```
T2Audio: Testing Resource[2] as config memory
T2Audio: Resource[2] Physical=0xC1670000 Length=0x10000
T2Audio: Resource[2] GPR test: version=0x00000003 signature=0x19870423 bufferOffset=0x00004000
T2Audio: FOUND valid signature in Resource[2]! Using Resource[2] as config.
T2Audio: Using config memory: Physical=0xC1670000 Length=0x10000
T2Audio: BAR2 mapped at FFFFF384E967F000
T2Audio: GPR read: version=0x00000003 signature=0x19870423 bufferOffset=0x00004000
T2Audio: BufferStruct at offset 0x4000
T2Audio: BufferStruct: Signature=0x19870423 Version=3 NumDevices=5
T2Audio: Device[0]: Name='Bridge Loopback' (match=0) NumOut=1
T2Audio: Device[1]: Name='Speaker' (match=7) NumOut=1
T2Audio: Speaker stream[0]: NumBuffers=1
T2Audio: Speaker buffer[0]: Address=0x12c000 Size=0x61800
T2Audio: FindSpeaker SUCCESS: buffer located
T2Audio: MapResources SUCCESS: buffer=0x12c000 size=0x61800
```

---

## Next Steps

### Phase 3: Enable Audio Endpoint (NOT STARTED)

1. Remove test mode flag in StartDevice
2. Uncomment WaveRT miniport creation
3. Register subdevice with PortCls
4. Verify audio endpoint appears in Windows Sound settings
5. Test basic playback (generate test tone)

**Prerequisites**:
- Current MapResources logic (working) ✅
- WaveRT miniport implementation (exists, disabled)
- Property handlers for format/position (exists)

**Risk**: Medium. Endpoint creation is standard PortCls, but format negotiation may need tuning.

### Phase 4: Audio Playback Testing

1. Configure sample rate (48 kHz), bit depth (24-bit), channels (6)
2. Implement GetPosition for stream position reporting
3. Test playback with Windows Media Player or test app
4. Verify data reaches Speaker buffer in BAR0

**Prerequisites**:
- Audio endpoint created ✅ (after Phase 3)
- Hardware DMA working (unknown, needs testing)
- T2 chip DSP initialized (unknown, likely needed)

### Phase 5: DSP and Synchronization

1. Research T2 DSP initialization sequence from kaiT2en/AppleAudio
2. Implement sample rate configuration
3. Implement volume control (if needed)
4. Implement start/stop/pause commands

**Prerequisites**:
- Audio plays but may have issues (distortion, timing)
- Understanding of T2 DSP register layout

---

## Known Limitations

1. **Test mode enabled**: StartDevice returns success but doesn't create audio endpoint
2. **No volume control**: Not implemented yet
3. **No power management**: Device always on
4. **No hotplug handling**: Driver assumes device present at boot
5. **Single format**: Hardcoded to 48kHz/24-bit/6-channel

---

## Build Configuration

**Project**: C:\Users\othysa\Desktop\mbp\T2AudioPort\src\T2AudioMiniport.vcxproj  
**Toolchain**: MSBuild 18.10.1, WDK 10.0.28000.0  
**Platform**: x64 Debug  
**Signing**: Manual with SHA256, DigiCert timestamp  
**Certificate**: CN=T2AudioPort Test Certificate (SHA1: CA2DE95D...)

---

## Git Repository

**URL**: https://github.com/imacorpoliz/t2audiowindows  
**Branch**: diagnostics  
**Remote**: origin (verified)

**Last commit**: a355fcf (2026-10-05 22:09 UTC)  
**Working tree**: Clean (all changes committed and pushed)

---

## References

- **kaiT2en**: https://github.com/kaitek666/kaiT2en (Linux T2 audio driver)
  - Buffer mapping: modules/t2bce_audio/audio.c:100-104
  - BAR0 = buffers, BAR4 = config, GPR at +0xC000
- **AppleAudio.sys**: Original Windows driver (disabled, reference only)
- **DEBUGGING_LOG.md**: Historical debugging record
- **MAPRESOURCES_ROOT_CAUSE.md**: Initial diagnostic session (FAIL_K identified)

---

## Device Information

**Model**: MacBookPro16,1 (2019)  
**OS**: Windows 11  
**PCI Device**: PCI\VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01\4&3AC8FC3&0&03D8  
**Friendly Name**: Apple T2 Audio Device (6-channel Native Driver)  
**Current Status**: Error (expected until WaveRT miniport enabled)

**Boot Configuration**:
```
testsigning=Yes
nointegritychecks=Yes
loadoptions=DISABLE_INTEGRITY_CHECKS
```

---

## Critical Success Factors

✅ Resource mapping works  
✅ GPR signature valid  
✅ Speaker buffer located  
⏳ Audio endpoint creation (next)  
⏳ Playback testing (future)  
⏳ DSP initialization (future)

**Blocker removed**: STATUS_DEVICE_CONFIGURATION_ERROR resolved by using correct resource.
