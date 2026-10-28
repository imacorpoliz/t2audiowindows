# BAR Mapping Solution - RESOLVED

**Date**: 2026-10-05 22:07 UTC  
**Status**: ✅ **PROBLEM SOLVED**  
**Driver**: SHA256 220B956584C9223B90A48DD219FBD0CABB2EA5F5D1D4FB291CE2C7830DEF0C31

---

## Root Cause

**INCORRECT RESOURCE SELECTION**: Driver was using Resource[1] as config memory, but **Resource[2] contains the valid GPR registers**.

---

## Evidence from Log (lines 16-31)

```
T2Audio: Testing Resource[2] as config memory
T2Audio: Resource[2] Physical=0xC1670000 Length=0x10000
T2Audio: Resource[2] GPR test: version=0x00000003 signature=0x19870423 bufferOffset=0x00004000
T2Audio: FOUND valid signature in Resource[2]! Using Resource[2] as config.
T2Audio: Using config memory: Physical=0xC1670000 Length=0x10000
T2Audio: BAR2 mapped at FFFFF384E967F000
T2Audio: GPR read: version=0x00000003 signature=0x19870423 bufferOffset=0x00004000
T2Audio: BufferStruct at offset 0x4000
T2Audio: FindSpeakerBuffer entry
T2Audio: BufferStruct: Signature=0x19870423 Version=3 NumDevices=5
T2Audio: Device[0]: Name='Bridge Loopback' (match=0) NumOut=1
T2Audio: Device[1]: Name='Speaker' (match=7) NumOut=1
T2Audio: Speaker stream[0]: NumBuffers=1
T2Audio: Speaker buffer[0]: Address=0x12c000 Size=0x61800
T2Audio: FindSpeaker SUCCESS: buffer located
T2Audio: MapResources SUCCESS: buffer=0x12c000 size=0x61800
```

---

## Correct Resource Mapping

| Windows Resource | Physical Address | Size | Purpose | kaiT2en Reference |
|------------------|------------------|------|---------|-------------------|
| Resource[0] | 0xC1000000 | 4 MB (0x400000) | Buffer memory | BAR0 (`pci_iomap(dev, 0, 0)`) |
| Resource[2] | 0xC1670000 | 64 KB (0x10000) | **Config memory** | BAR4 (`pci_iomap(dev, 4, 0)`) |
| Resource[1] | 0xC1680000 | 512 KB (0x80000) | Unknown (not used) | BAR5? |

---

## GPR Values (Valid)

From Resource[2] + 0xC000:

| Field | Value | Status |
|-------|-------|--------|
| Version | 0x00000003 | ✅ Valid (≥ 2) |
| Signature | 0x19870423 | ✅ Valid (matches T2AUDIO_SIG) |
| Buffer Offset | 0x00004000 | ✅ Valid (16 KB into Resource[2]) |

**NOTE**: Buffer offset is 0x4000, NOT 0xC000. GPR registers are at config_base + 0xC000, but BufferStruct metadata is at config_base + 0x4000.

---

## Speaker Buffer Details

| Property | Value |
|----------|-------|
| Device Name | "Speaker" |
| Number of Output Streams | 1 |
| Number of Buffers | 1 |
| Buffer Address (offset in BAR0) | 0x12C000 |
| Buffer Size | 0x61800 (399,360 bytes ≈ 390 KB) |

**Physical address**: 0xC1000000 + 0x12C000 = 0xC112C000

---

## Device Enumeration

BufferStruct contains **5 devices**:

0. "Bridge Loopback" (1 output stream)
1. **"Speaker"** (1 output stream) ← Target device
2. (not logged, but exists)
3. (not logged, but exists)
4. (not logged, but exists)

---

## Why Resource Index ≠ BAR Index

**PCI 64-bit BAR layout**: Each 64-bit BAR consumes TWO configuration space slots.

| Config Space Slot | Content | Windows Resource |
|-------------------|---------|------------------|
| BAR0 (offset 0x10) | 64-bit address lower 32 bits | |
| BAR1 (offset 0x14) | 64-bit address upper 32 bits | Resource[0] (4 MB) |
| BAR2 (offset 0x18) | 64-bit address lower 32 bits | |
| BAR3 (offset 0x1C) | 64-bit address upper 32 bits | Resource[2] (64 KB) |
| BAR4 (offset 0x20) | 64-bit address lower 32 bits | |
| BAR5 (offset 0x24) | 64-bit address upper 32 bits | Resource[1] (512 KB) |

**Linux `pci_iomap(dev, N, 0)` uses slot index N**, not resource enumeration index.

- kaiT2en BAR0 (slot 0+1) → Windows Resource[0] ✅
- kaiT2en BAR4 (slot 4+5) → Windows Resource[2] ✅ (NOT Resource[1])

Windows enumerates resources in **address order** or **discovery order**, not BAR slot order.

---

## Previous Incorrect Assumption

**Old code**:
```c
// Resource[1] was assumed to be BAR4
descriptor = ResourceList->lpVtbl->FindTranslatedEntry(..., 1);
// GPR read returned 0xFFFFFFFF (hardware not responding)
```

**Why it failed**: Resource[1] (0xC1680000) is likely BAR5, not BAR4. Hardware does not have GPR registers at that location.

---

## Solution Applied

**New code** (Device.c:93-132):
```c
// Test Resource[2] FIRST
if (memoryResourceCount >= 3) {
    testDescriptor = ResourceList->lpVtbl->FindTranslatedEntry(..., 2);
    testMapped = MmMapIoSpaceEx(...);
    testGpr = testMapped + T2AUDIO_GPR_OFFSET;
    testSignature = READ_REGISTER_ULONG((PULONG)(testGpr + 4));
    
    if (testSignature == T2AUDIO_SIG) {
        descriptor = testDescriptor; // Use Resource[2]
        goto MapConfigResource;
    }
}
// Fall back to Resource[1] if Resource[2] test fails
```

---

## Next Steps

1. ✅ **MapResources now succeeds** - StartDevice returns STATUS_SUCCESS
2. ⏳ **Enable WaveRT miniport** - Create audio endpoint for testing
3. ⏳ **Test audio playback** - Verify data reaches Speaker buffer
4. ⏳ **Implement DSP initialization** - Required for actual audio output
5. ⏳ **BCE transport** - For full device communication

---

## Commit Status

Changes ready to commit:
- `src/Device.c`: Resource[2] test logic
- `docs/RESOURCE_ANALYSIS.md`: Analysis leading to solution
- `docs/BAR_MAPPING_SOLUTION.md`: This file (solution summary)
- `docs/logs/diagnostic_20261005_220710_SUCCESS.log`: Proof of success

**Branch**: diagnostics  
**Ready for**: Commit and push
