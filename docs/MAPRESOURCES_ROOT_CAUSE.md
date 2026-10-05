# T2AudioMapResources Root Cause Analysis

**Date**: 2026-10-05 21:47 UTC  
**Status**: ✅ ROOT CAUSE IDENTIFIED

## Executive Summary

`STATUS_DEVICE_CONFIGURATION_ERROR (0xC0000182)` occurs at **FAIL_K**: GPR signature validation fails because **all GPR registers return 0xFFFFFFFF**.

## Diagnostic Evidence

From `diagnostic_20261005_214749_SUCCESS.log`:

```
00000009  0.02934860  System  T2Audio: Memory resource count: 3
00000010  0.02935160  System  T2Audio: BAR1 Physical=0xC1000000 Length=0x400000
00000011  0.02938410  System  T2Audio: BAR1 mapped at FFFFF4705D400000
00000012  0.02938650  System  T2Audio: BAR2 Physical=0xC1680000 Length=0x80000
00000013  0.02939440  System  T2Audio: BAR2 mapped at FFFFF384EBB2C000
00000014  0.02945010  System  T2Audio: GPR read: version=0xFFFFFFFF signature=0xFFFFFFFF bufferOffset=0xFFFFFFFF
00000015  0.02945220  System  T2Audio: MapResources FAIL_K: GPR signature 0xFFFFFFFF != 0x19870423
00000016  0.02946160  System  T2Audio: MapResources failed: 0xC0000182
```

## Key Findings

### ✅ Windows DOES allocate memory resources
- **3 memory resources** allocated (hypothesis of missing resources: DISPROVED)
- BAR1: Physical 0xC1000000, Length 0x400000 (4 MB)
- BAR2: Physical 0xC1680000, Length 0x80000 (512 KB)

### ✅ MmMapIoSpaceEx succeeds
- BAR1 mapped to virtual address 0xFFFFF4705D400000
- BAR2 mapped to virtual address 0xFFFFF384EBB2C000
- No FAIL_E or FAIL_I (mapping failures)

### ❌ GPR registers return invalid data
- Version: 0xFFFFFFFF (expected: ≥ 2)
- Signature: 0xFFFFFFFF (expected: 0x19870423)
- BufferOffset: 0xFFFFFFFF (expected: valid offset into BAR1)

### ❌ Failure occurs at FAIL_K
- Code: `Device.c:125` (signature validation)
- Condition: `signature != T2AUDIO_SIG`
- Actual: `0xFFFFFFFF != 0x19870423`

## Root Cause

**All-ones pattern (0xFFFFFFFF) from MMIO read indicates hardware is NOT responding.**

Possible causes:
1. **Device not powered on**: T2 chip in low-power state
2. **Wrong BAR mapping**: Reading wrong physical address or cache type
3. **Bus not initialized**: PCIe enumeration incomplete
4. **Device disabled**: T2 audio function disabled in firmware/hardware
5. **Timing issue**: Reading too early before hardware initialization
6. **Wrong GPR offset**: T2AUDIO_GPR_OFFSET (0xC000) incorrect for this device

## What the Driver Got Right

### Memory Resources (3 descriptors allocated)
Device has 3 memory BARs. We only use first 2:
- **Index 0** → BAR1 (audio buffers): 0xC1000000, 4 MB
- **Index 1** → BAR2 (config/GPR): 0xC1680000, 512 KB  
- **Index 2** → (unused, not queried)

### BAR Sizes Reasonable
- BAR1: 4 MB is plausible for multi-channel audio buffers
- BAR2: 512 KB is plausible for config space and metadata

### Mapping Succeeds
- No memory allocation failures
- Virtual addresses assigned correctly
- No access violations (would cause BSOD, not 0xFFFFFFFF pattern)

## What Is Wrong

### GPR Offset May Be Incorrect
Current code:
```c
#define T2AUDIO_GPR_OFFSET ((SIZE_T)0xC000)  // 48 KB into BAR2

gpr = (PUCHAR)Context->Bar2Mapped + T2AUDIO_GPR_OFFSET;
version = READ_REGISTER_ULONG((PULONG)(gpr + 0));
signature = READ_REGISTER_ULONG((PULONG)(gpr + 4));
bufferOffset = READ_REGISTER_ULONG((PULONG)(gpr + 8));
```

If offset is wrong, we're reading unmapped or unpopulated region → 0xFFFFFFFF.

### Cache Type May Be Incorrect
Current mapping for BAR2:
```c
Context->Bar2Mapped = MmMapIoSpaceEx(descriptor->u.Memory.Start,
                                     Context->Bar2Size,
                                     PAGE_READONLY | PAGE_NOCACHE);
```

If T2 requires different cache policy (e.g., PAGE_WRITECOMBINE), reads may return invalid data.

### Device May Not Be Ready
T2 chip may need:
- Explicit power-on command via another interface (BCE?)
- Firmware initialization from macOS
- Reset sequence before registers become valid

## Next Steps

### Option 1: Scan BAR2 for Valid Signature (RECOMMENDED)
Instead of fixed offset 0xC000, search for signature 0x19870423:

```c
// Search BAR2 for signature at 4-byte aligned offsets
for (SIZE_T offset = 0; offset < Context->Bar2Size - 12; offset += 4) {
    ULONG sig = READ_REGISTER_ULONG((PULONG)((PUCHAR)Context->Bar2Mapped + offset));
    if (sig == T2AUDIO_SIG) {
        // Found signature, check version at offset+(-4) or nearby
        KdPrint(("T2Audio: Found signature at offset 0x%IX\n", offset));
        break;
    }
}
```

### Option 2: Dump BAR2 First 64 KB
Log first few KB of BAR2 to see if ANY non-0xFF data exists:

```c
KdPrint(("T2Audio: BAR2 dump (first 256 bytes):\n"));
for (SIZE_T i = 0; i < 256; i += 16) {
    KdPrint(("  %04IX: %08X %08X %08X %08X\n", i,
             READ_REGISTER_ULONG((PULONG)((PUCHAR)Context->Bar2Mapped + i + 0)),
             READ_REGISTER_ULONG((PULONG)((PUCHAR)Context->Bar2Mapped + i + 4)),
             READ_REGISTER_ULONG((PULONG)((PUCHAR)Context->Bar2Mapped + i + 8)),
             READ_REGISTER_ULONG((PULONG)((PUCHAR)Context->Bar2Mapped + i + 12))));
}
```

### Option 3: Try Different Cache Types
Test BAR2 with:
- `PAGE_NOCACHE` (current)
- `PAGE_WRITECOMBINE`
- `PAGE_NOCACHE | PAGE_WRITECOMBINE`

### Option 4: Compare with Working AppleAudio.sys
Reverse-engineer:
- How AppleAudio.sys finds GPR offset
- Whether it performs device initialization before reading
- Whether it uses different resource descriptor indices

### Option 5: Check T2 Power State
Query PCI configuration space for:
- Power Management Capabilities (PM_CAP)
- Current power state (D0/D1/D2/D3)
- Device enable bit (Command register bit 0)

## Why This Is NOT the Final Blocker

Even if GPR offset is wrong, finding the correct offset is tractable:
- 512 KB search space with known signature pattern
- Signature 0x19870423 is unique and easy to locate
- Once found, BufferStruct offset can be read from GPR+8

If hardware is NOT responding at all:
- Need BCE transport to wake up T2 audio
- Or need AppleAudio.sys initialization sequence

## Success Criteria Met

✅ **Exact failure branch identified**: FAIL_K (GPR signature mismatch)  
✅ **Resource state confirmed**: 3 memory resources allocated, 2 mapped successfully  
✅ **Hardware addresses known**: BAR1 @ 0xC1000000, BAR2 @ 0xC1680000  
✅ **Next diagnostic step clear**: Search BAR2 for signature or dump contents

## Files

- **Diagnostic Log**: `docs/logs/diagnostic_20261005_214749_SUCCESS.log`
- **Driver Binary**: SHA256 `5FE84ED9CDEA42870EA1F79FFB5CB116A7EA912A1B62970F8E38287CC8B033A9`
- **Code Location**: `src/Device.c:76-78` (GPR read), `Device.c:125` (FAIL_K)

## Recommendations

1. **Immediate**: Implement BAR2 signature search (Option 1)
2. **If not found**: Dump BAR2 contents to check if hardware responds (Option 2)
3. **If all 0xFF**: Investigate T2 power state or BCE initialization requirement
4. **If found at different offset**: Update T2AUDIO_GPR_OFFSET constant

This diagnostic successfully isolated the problem to **GPR offset or device initialization**, eliminating resource allocation and memory mapping as failure causes.
