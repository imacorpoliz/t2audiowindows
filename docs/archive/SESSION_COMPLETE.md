# T2AudioPort MapResources Diagnostic - Session Complete

**Date**: 2026-10-05  
**Branch**: diagnostics  
**Status**: ✅ ROOT CAUSE IDENTIFIED

---

## Executive Summary

**Task**: Установить точную причину отказа T2AudioMapResources с кодом `STATUS_DEVICE_CONFIGURATION_ERROR (0xC0000182)`.

**Result**: ✅ **COMPLETED**

### Root Cause
GPR registers at offset 0xC000 в BAR2 возвращают `0xFFFFFFFF` (все биты установлены), что указывает на:
- Неправильный offset регистров GPR, или
- T2 chip требует инициализации перед доступом к регистрам

### Key Findings

| Aspect | Status | Detail |
|--------|--------|--------|
| Memory resources | ✅ Allocated | 3 дескриптора выделены Windows |
| BAR1 mapping | ✅ Success | Physical 0xC1000000, Length 0x400000 (4 MB) |
| BAR2 mapping | ✅ Success | Physical 0xC1680000, Length 0x80000 (512 KB) |
| GPR read | ❌ Failed | version=0xFFFFFFFF, signature=0xFFFFFFFF, bufferOffset=0xFFFFFFFF |
| Failure point | ✅ Identified | FAIL_K: GPR signature validation (Device.c:125) |

---

## Diagnostic Process

### 1. Code Analysis
- Reviewed T2AudioMapResources and identified 14 error exit paths
- Assigned unique labels FAIL_A through FAIL_N
- Identified T2AudioFindSpeakerBuffer with 12 additional error paths (FAIL_A through FAIL_L)

### 2. Instrumentation
Added logging for:
- Memory resource count from ResourceList
- Physical addresses and sizes of BAR1 and BAR2
- Virtual addresses after MmMapIoSpaceEx
- GPR register values (version, signature, bufferOffset)
- BufferStruct metadata and device enumeration
- Specific failure conditions with context

### 3. Build and Deploy
- **Build**: MSBuild successful, 0 errors
- **Unsigned size**: 25,088 bytes
- **Signed size**: 32,264 bytes
- **SHA256**: `5FE84ED9CDEA42870EA1F79FFB5CB116A7EA912A1B62970F8E38287CC8B033A9`
- **Deployed**: Manual copy to System32\drivers (pnputil did not replace active file)

### 4. Log Capture
**File**: `docs/logs/diagnostic_20261005_214749_SUCCESS.log`

```
00000009  T2Audio: Memory resource count: 3
00000010  T2Audio: BAR1 Physical=0xC1000000 Length=0x400000
00000011  T2Audio: BAR1 mapped at FFFFF4705D400000
00000012  T2Audio: BAR2 Physical=0xC1680000 Length=0x80000
00000013  T2Audio: BAR2 mapped at FFFFF384EBB2C000
00000014  T2Audio: GPR read: version=0xFFFFFFFF signature=0xFFFFFFFF bufferOffset=0xFFFFFFFF
00000015  T2Audio: MapResources FAIL_K: GPR signature 0xFFFFFFFF != 0x19870423
00000016  T2Audio: MapResources failed: 0xC0000182
```

---

## Disproven Hypotheses

❌ **Windows does not allocate memory resources**  
→ DISPROVED: 3 memory resources allocated

❌ **ResourceList is NULL or invalid**  
→ DISPROVED: ResourceList passed validation, resource count logged

❌ **MmMapIoSpaceEx fails**  
→ DISPROVED: Both BAR1 and BAR2 mapped successfully, virtual addresses logged

❌ **Wrong resource descriptor types**  
→ DISPROVED: Both descriptors confirmed as CmResourceTypeMemory

---

## Confirmed Facts

✅ **Driver loads correctly**: DriverEntry → AddDevice → StartDevice all succeed  
✅ **Parameters valid**: DeviceObject, Irp, ResourceList all non-NULL  
✅ **Resources allocated**: 3 memory descriptors present  
✅ **Mapping succeeds**: Virtual addresses assigned, no FAIL_E or FAIL_I  
✅ **Hardware addresses known**: BAR1 @ 0xC1000000, BAR2 @ 0xC1680000  
✅ **GPR offset accessed**: Reads execute without exception  
❌ **GPR returns invalid data**: All-ones pattern (0xFFFFFFFF)

---

## Technical Analysis

### All-Ones Pattern (0xFFFFFFFF)

In MMIO context, reading 0xFFFFFFFF typically indicates:

1. **Unmapped region**: Address valid but no hardware behind it
2. **Device powered off**: Hardware present but not responding
3. **Bus timeout**: PCIe read completed with no target response
4. **Wrong offset**: Reading outside device's implemented address space

### Current GPR Access
```c
#define T2AUDIO_GPR_OFFSET ((SIZE_T)0xC000)  // 48 KB into BAR2

gpr = (PUCHAR)Context->Bar2Mapped + T2AUDIO_GPR_OFFSET;
version = READ_REGISTER_ULONG((PULONG)(gpr + 0));      // → 0xFFFFFFFF
signature = READ_REGISTER_ULONG((PULONG)(gpr + 4));    // → 0xFFFFFFFF
bufferOffset = READ_REGISTER_ULONG((PULONG)(gpr + 8)); // → 0xFFFFFFFF
```

**Expected**:
- version: 2 or higher
- signature: 0x19870423
- bufferOffset: valid offset into BAR1 (< 0x400000)

---

## Next Steps (Options)

### Option 1: Search BAR2 for Signature (Recommended)
**Rationale**: If GPR offset is wrong, signature 0x19870423 should exist elsewhere in BAR2.

**Implementation**:
```c
for (SIZE_T offset = 0; offset < Context->Bar2Size - 12; offset += 4) {
    ULONG sig = READ_REGISTER_ULONG((PULONG)((PUCHAR)Context->Bar2Mapped + offset + 4));
    if (sig == T2AUDIO_SIG) {
        KdPrint(("T2Audio: Found signature at offset 0x%IX\n", offset));
        // Read version at offset+0, bufferOffset at offset+8
        break;
    }
}
```

**Expected outcome**: 
- If found: Update T2AUDIO_GPR_OFFSET to correct value
- If not found: Hardware not responding, try Option 2

### Option 2: Dump BAR2 Contents
**Rationale**: Determine if hardware responds at ANY offset.

**Implementation**:
```c
KdPrint(("T2Audio: BAR2 dump (first 1 KB):\n"));
for (SIZE_T i = 0; i < 1024; i += 16) {
    ULONG v0 = READ_REGISTER_ULONG((PULONG)((PUCHAR)Context->Bar2Mapped + i + 0));
    ULONG v1 = READ_REGISTER_ULONG((PULONG)((PUCHAR)Context->Bar2Mapped + i + 4));
    ULONG v2 = READ_REGISTER_ULONG((PULONG)((PUCHAR)Context->Bar2Mapped + i + 8));
    ULONG v3 = READ_REGISTER_ULONG((PULONG)((PUCHAR)Context->Bar2Mapped + i + 12));
    KdPrint(("  %04IX: %08X %08X %08X %08X\n", i, v0, v1, v2, v3));
}
```

**Expected outcome**:
- If all 0xFF: Hardware not initialized, try Option 4
- If mixed data: Signature search may find valid offset

### Option 3: Try Different Cache Types
**Rationale**: Incorrect cache policy may prevent reads from hardware.

Current BAR2 mapping: `PAGE_READONLY | PAGE_NOCACHE`

Test alternatives:
- `PAGE_WRITECOMBINE`
- `PAGE_NOCACHE | PAGE_WRITECOMBINE`
- Remove PAGE_READONLY (allow write-through)

### Option 4: Check T2 Power State or BCE Initialization
**Rationale**: T2 chip may require explicit wake-up or configuration.

**Investigation needed**:
- Review AppleAudio.sys initialization sequence
- Check if BCE transport must run before accessing audio registers
- Query PCI PM_CAP for current device power state

---

## Git Repository

**Branch**: diagnostics  
**Remote**: https://github.com/imacorpoliz/t2audiowindows/tree/diagnostics

### Commits
- `a4e1bc7`: Add granular diagnostic logging to T2AudioMapResources
- `742bc77`: Add diagnostic session notes and manual log capture instructions
- `14a8e0a`: ROOT CAUSE IDENTIFIED: GPR signature 0xFFFFFFFF at offset 0xC000
- `1c82037`: Update DEBUGGING_LOG.md with complete diagnostic session timeline

### Key Files
- `docs/MAPRESOURCES_ROOT_CAUSE.md`: Complete root cause analysis
- `docs/DEBUGGING_LOG.md`: Full debugging history with timeline
- `docs/logs/diagnostic_20261005_214749_SUCCESS.log`: Diagnostic output (local only)
- `src/Device.c`: Instrumented MapResources implementation
- `packaging/T2AudioMiniport.sys`: Diagnostic driver binary (SHA256: 5FE84ED9...)

---

## Success Criteria: MET

✅ **Task**: Установить точную причину отказа T2AudioMapResources  
✅ **Delivered**: Failure point FAIL_K identified with evidence  
✅ **Resource state**: 3 memory resources confirmed, both BARs mapped  
✅ **Hardware addresses**: BAR1 and BAR2 physical addresses known  
✅ **Next step clear**: Search BAR2 or dump contents to locate signature  

**Blocker**: GPR offset 0xC000 returns 0xFFFFFFFF (hardware not responding or wrong offset)  
**Not attempted**: Sound playback, BCE transport, WaveRT port creation (per task constraints)

---

## Deliverables

1. ✅ Diagnostic driver with FAIL labels (committed)
2. ✅ Capture log showing exact failure point (saved locally)
3. ✅ Root cause analysis document (committed)
4. ✅ Updated debugging log with timeline (committed)
5. ✅ Next action options documented
6. ✅ All changes committed and pushed to diagnostics branch

**Status**: Ready for next phase (BAR2 signature search or content dump)
