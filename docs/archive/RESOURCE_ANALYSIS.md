# T2 Audio Resource to BAR Mapping - Analysis Results

**Date**: 2026-10-05 22:04 UTC  
**Driver**: SHA256 C7394C568DF99D43856495DB29FB68E0009E63AA226D6049A25527C11589633B  
**Log**: `diagnostic_20261005_220400_resources.log`

---

## Windows Memory Resources (Actual)

From diagnostic log lines 19-21:

| Index | Physical Address | Length | Size | Flags |
|-------|------------------|---------|------|-------|
| Resource[0] | 0xC1000000 | 0x400000 | 4 MB | 0x0084 |
| Resource[1] | 0xC1680000 | 0x80000 | 512 KB | 0x0084 |
| Resource[2] | 0xC1670000 | 0x10000 | 64 KB | 0x0084 |

**Flags 0x0084**: CmResourceMemoryReadWrite (0x0000) + CmResourceMemoryPrefetchable (0x0004) + CmResourceMemory64 (0x0080)

---

## Address Analysis

### Observation 1: Resource Order vs Physical Address Order

**Physical address order** (ascending):
1. 0xC1000000 (Resource[0], 4 MB)
2. 0xC1670000 (Resource[2], 64 KB)  ← **Between Resource[0] and Resource[1]**
3. 0xC1680000 (Resource[1], 512 KB)

**Windows enumeration order**:
1. Resource[0]: 0xC1000000
2. Resource[1]: 0xC1680000
3. Resource[2]: 0xC1670000

**Conclusion**: Windows does NOT enumerate resources in physical address order. Resource index ≠ BAR index.

### Observation 2: Address Gaps

- Gap between Resource[0] end and Resource[2] start:
  - Resource[0]: 0xC1000000 + 0x400000 = 0xC1400000 (end)
  - Resource[2]: 0xC1670000 (start)
  - Gap: 0xC1670000 - 0xC1400000 = **0x270000 (2.4 MB)**

- Resource[2] and Resource[1] are adjacent:
  - Resource[2]: 0xC1670000 + 0x10000 = 0xC1680000 (end)
  - Resource[1]: 0xC1680000 (start)
  - Gap: **0 bytes (contiguous)**

**Conclusion**: Resource[2] and Resource[1] form a contiguous 576 KB block (64KB + 512KB).

---

## kaiT2en Reference (from audio.c:100-104)

```c
t2audio->reg_mem_bs = pci_iomap(dev, 0, 0);      // BAR0: Buffer memory
t2audio->reg_mem_cfg = pci_iomap(dev, 4, 0);     // BAR4: Config memory
t2audio->reg_mem_gpr = (u32 __iomem *) ((u8 __iomem *) t2audio->reg_mem_cfg + 0xC000);
```

**Expected**:
- BAR0 (index 0): Large buffer memory (several MB)
- BAR4 (index 4): Config memory with GPR at +0xC000

---

## Hypothesis: 64-bit BAR Layout

PCI configuration space has 6 BAR slots (BAR0-BAR5). For 64-bit BARs, each BAR consumes **two consecutive slots**:
- Lower 32 bits in BARn
- Upper 32 bits in BARn+1

**Possible layout**:

| BAR Slot | Type | Physical Address | Size | Windows Resource |
|----------|------|------------------|------|------------------|
| BAR0 (slot 0) | 64-bit lower | 0xC1000000 | | |
| BAR1 (slot 1) | 64-bit upper | (high bits) | | Resource[0] (4 MB) |
| BAR2 (slot 2) | 64-bit lower | 0xC1670000 | | |
| BAR3 (slot 3) | 64-bit upper | (high bits) | | Resource[2] (64 KB) |
| BAR4 (slot 4) | 64-bit lower | 0xC1680000 | | |
| BAR5 (slot 5) | 64-bit upper | (high bits) | | Resource[1] (512 KB) |

**Key insight**: kaiT2en references **slot index**, not resource index:
- `pci_iomap(dev, 0, 0)` → BAR slot 0+1 → Resource[0]
- `pci_iomap(dev, 4, 0)` → BAR slot 4+5 → Resource[1]

---

## Current Driver Behavior

**Lines 22-26**:
```
T2Audio: Using Resource[0] as buffer memory (kaiT2en BAR0)
T2Audio: Resource[0] Physical=0xC1000000 Length=0x400000
T2Audio: Resource[0] mapped at FFFFF4705D400000
T2Audio: Using Resource[1] as config memory (assumed kaiT2en BAR4)
T2Audio: Resource[1] Physical=0xC1680000 Length=0x80000
```

**Lines 28-29**:
```
T2Audio: GPR read: version=0xFFFFFFFF signature=0xFFFFFFFF bufferOffset=0xFFFFFFFF
T2Audio: MapResources FAIL_K: GPR signature 0xFFFFFFFF != 0x19870423
```

**Mapping**:
- Resource[0] (0xC1000000, 4 MB) → Assumed BAR0 ✅
- Resource[1] (0xC1680000, 512 KB) → Assumed BAR4 ❓
- GPR at Resource[1] + 0xC000 → Returns 0xFFFFFFFF ❌

---

## Problem Analysis

### Scenario A: Resource[1] IS BAR4, but device not initialized

**Evidence**:
- Resource sizes match expectations (4 MB buffers, 512 KB config is reasonable)
- 0xFFFFFFFF pattern suggests hardware not responding, not wrong address
- kaiT2en performs BCE initialization BEFORE reading GPR (audio.c:111-114)

**Next step**: Compare initialization sequence with kaiT2en

### Scenario B: Resource[1] is NOT BAR4

**Evidence**:
- Resource[2] (64 KB) sits between Resource[0] and Resource[1] physically
- Maybe Resource[2] is actual config space, Resource[1] is something else
- But 64 KB is very small for config space with GPR at +0xC000

**Test**: Try Resource[2] as config memory

### Scenario C: GPR offset is wrong

**Evidence**:
- 0xC000 is 48 KB into 512 KB region (plausible)
- But 0xC000 is 75% through 64 KB region (leaves only 16 KB after GPR)
- kaiT2en explicitly uses 0xC000, likely correct

---

## Recommendation

**HYPOTHESIS TO TEST**: Resource[2] might be the actual config memory (BAR2 or BAR3 slot).

**Rationale**:
1. Resource[2] is physically between buffers and Resource[1]
2. 64 KB is small but could contain GPR registers
3. If GPR is at +0xC000 (48 KB), leaves 16 KB after = 4K registers × 4 bytes = plausible

**Action**: Modify driver to try Resource[2] as config memory:
- Map Resource[2] instead of Resource[1]
- Check if Resource[2] size (0x10000) > GPR offset (0xC000) + 12 bytes
- Read GPR from Resource[2] + 0xC000

**Alternative**: Read PCI configuration space directly to see actual BAR types and addresses.

---

## Critical Finding

**Resource[2] is SMALLER than GPR_OFFSET + GPR size**:
- Resource[2] size: 0x10000 (64 KB = 65,536 bytes)
- GPR offset: 0xC000 (48 KB = 49,152 bytes)
- Remaining: 65,536 - 49,152 = **16,384 bytes (16 KB)** ✅ Enough for GPR (12 bytes minimum)

**But Resource[1] is much larger**:
- Resource[1] size: 0x80000 (512 KB = 524,288 bytes)
- After GPR offset: 524,288 - 49,152 = **475,136 bytes** ✅ More space

**Conclusion**: Both Resource[1] and Resource[2] are large enough for GPR access, but 0xFFFFFFFF suggests hardware issue, not size issue.

---

## Next Steps

### Option 1: Try Resource[2] as Config Memory (Quick Test)
Change driver to use Resource[2] instead of Resource[1] for GPR access.

### Option 2: Read PCI Configuration Space (Definitive)
Use BUS_INTERFACE_STANDARD to read BAR0-BAR5 from PCI config space and definitively map to resources.

### Option 3: Compare Initialization Sequence
Review kaiT2en BCE initialization (audio.c:111-114) to see if device needs wakeup before GPR access.

**Recommended order**: Option 1 (quickest), then Option 2 (definitive), then Option 3 (if both fail).
