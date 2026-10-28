# T2 Audio PCI BAR Mapping Analysis

**Date**: 2026-10-05  
**Purpose**: Establish correspondence between Windows memory resources and physical PCI BARs

---

## Source Reference: kaiT2en (upstream)

**Location**: `C:\Users\othysa\Desktop\mbp\upstream-kait2en\modules\t2bce_audio\audio.c`  
**Version**: KAIT2EN Fedora, commit ed5a361 "patches: enable host router runtime pm for titan ridge"  
**Origin**: https://kait2en.org/ - T2 Mac support for Fedora using DKMS modules

### Confirmed BAR Mapping (audio.c:100-104)

```c
t2audio->reg_mem_bs_dma = pci_resource_start(dev, 0);
t2audio->reg_mem_bs = pci_iomap(dev, 0, 0);      // Buffer memory: BAR0
t2audio->reg_mem_cfg = pci_iomap(dev, 4, 0);     // Config memory: BAR4

t2audio->reg_mem_gpr = (u32 __iomem *) ((u8 __iomem *) t2audio->reg_mem_cfg + 0xC000);
```

**Key findings**:
- **BAR0**: Buffer memory (audio buffers, BufferStruct metadata)
- **BAR4**: Config memory (GPR registers at offset +0xC000)
- **GPR offset**: 0xC000 from BAR4 base (CONFIRMED from upstream)

---

## Windows Memory Resources (from diagnostic log)

**Device**: PCI\VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01  
**Log**: `diagnostic_20261005_214749_SUCCESS.log`

### Resource Count
```
00000009  T2Audio: Memory resource count: 3
```

### Resource Assignments (as logged)

**Resource[0]** (identified as "BAR1" in old naming):
```
00000010  T2Audio: BAR1 Physical=0xC1000000 Length=0x400000
00000011  T2Audio: BAR1 mapped at FFFFF4705D400000
```
- Physical address: 0xC1000000
- Length: 0x400000 (4 MB)
- Mapped successfully

**Resource[1]** (identified as "BAR2" in old naming):
```
00000012  T2Audio: BAR2 Physical=0xC1680000 Length=0x80000
00000013  T2Audio: BAR2 mapped at FFFFF384EBB2C000
```
- Physical address: 0xC1680000
- Length: 0x80000 (512 KB)
- Mapped successfully

**Resource[2]**:
- Not queried by current driver (only first 2 resources used)

---

## Current Driver Implementation (Device.c)

```c
// Uses ResourceList index 0 and 1, without verifying PCI BAR correspondence

descriptor = ResourceList->lpVtbl->FindTranslatedEntry(
    (INTERFACE *)ResourceList, CmResourceTypeMemory, 0);
Context->Bar1Mapped = MmMapIoSpaceEx(descriptor->u.Memory.Start, ...);

descriptor = ResourceList->lpVtbl->FindTranslatedEntry(
    (INTERFACE *)ResourceList, CmResourceTypeMemory, 1);
Context->Bar2Mapped = MmMapIoSpaceEx(descriptor->u.Memory.Start, ...);

gpr = (PUCHAR)Context->Bar2Mapped + T2AUDIO_GPR_OFFSET;  // Offset 0xC000
```

**Assumption**: Resource[0] = BAR0 (buffers), Resource[1] = BAR4 (config)  
**Problem**: NOT VERIFIED against actual PCI BAR layout

---

## Issue: FAIL_K (GPR Signature 0xFFFFFFFF)

**Current behavior**:
```
00000014  T2Audio: GPR read: version=0xFFFFFFFF signature=0xFFFFFFFF bufferOffset=0xFFFFFFFF
00000015  T2Audio: MapResources FAIL_K: GPR signature 0xFFFFFFFF != 0x19870423
```

**Possible causes**:
1. Resource[1] does NOT correspond to BAR4 (config memory)
2. Windows enumerates resources in different order than Linux PCI BAR indices
3. BAR4 is 64-bit and spans two BAR slots
4. Resource[1] actually maps to a different BAR (e.g., BAR2, not BAR4)

---

## Required Investigation

### 1. Read PCI Configuration Space

Need to read BARs 0-5 from PCI config space (offsets 0x10-0x27) to determine:
- Which BARs are implemented (non-zero)
- BAR types (32-bit vs 64-bit)
- Base addresses and sizes
- Correspondence to Windows resources

**Method**: Use BUS_INTERFACE_STANDARD or IRP_MN_READ_CONFIG (read-only)

### 2. Match Resources to BARs

Compare:
- PCI BAR base addresses (from config space)
- Windows resource physical addresses (0xC1000000, 0xC1680000, resource[2])
- Identify which resource corresponds to BAR0 (buffers) and BAR4 (config)

### 3. Verify or Correct Mapping

**If Resource[1] is NOT BAR4**:
- Find correct resource index for BAR4
- Update driver to select correct resource for config memory
- Re-read GPR at correct base + 0xC000

**If Resource[1] IS BAR4**:
- Investigate why GPR returns 0xFFFFFFFF
- Consider initialization requirements (BCE, power state)
- Check if device needs wake-up before register access

---

## Critical Distinction

**Previously stated** (incorrect): "ROOT CAUSE: GPR signature 0xFFFFFFFF"  
**Correct statement**: "FAIL_K is the failure branch; root cause requires BAR verification"

FAIL_K identifies WHERE the code fails (signature check), but NOT WHY hardware returns 0xFFFFFFFF.

Possible root causes:
- Wrong resource mapped (Resource[1] ≠ BAR4)
- Correct resource but device not initialized
- Correct resource but different GPR offset needed

Must eliminate "wrong resource" hypothesis before concluding device initialization issue.

---

## Next Action

1. Implement PCI config space read (BUS_INTERFACE_STANDARD)
2. Log BAR0-BAR5 addresses and types from config space
3. Match to Windows resource[0], resource[1], resource[2]
4. If mismatch found: correct resource selection
5. If match confirmed: investigate initialization sequence

**Status**: Investigation required before any code changes
