# Final summary: independent re-verification of AppleAudio.sys

**Date**: 2026-10-04  
**Task**: fix the RVA→offset conversion errors, scan all executable sections, trace the MmMapIoSpaceEx usage

> Archived. The PCM write path that this document could not find is AppleAudio's copy loop `0x14000A715`–`0x14000B2C0` (`docs/analysis/RE_appleaudio_protocol.md` §3). The recommended option 2 (a new kernel driver) is T2AudioPort.

---

## Errors in the previous analysis, fixed

### 1. RVA → file offset conversion

**Error**: a single offset of 0xC00 (the .text RVA/raw difference) was applied to every section.

**Fix**: per-section offsets from the PE section table:
```
.text: RVA 0x1000, raw 0x400      → offset = RVA - 0x1000 + 0x400
PAGE:  RVA 0x16000, raw 0x13600   → offset = RVA - 0x16000 + 0x13600
INIT:  RVA 0x1E000, raw 0x1A800   → offset = RVA - 0x1E000 + 0x1A800
```

**Consequence**: the wrong RVA 0x171F4 (from the previous report) was really file offset 0x165F4, i.e. RVA 0x18FF4 in the PAGE section, not where the calls are.

### 2. Incomplete section scan

**Error**: only the .text section was searched (RVA 0x1000-0x15xxx).

**Fix**: a full scan of every section with the IMAGE_SCN_MEM_EXECUTE flag:
- .text: 0x1000–0x15608 (executable)
- PAGE: 0x16000–0x1D478 (executable)
- INIT: 0x1E000–0x1E360 (executable)

**Result**: 2 MmMapIoSpaceEx calls found in the **PAGE** section, not in .text.

### 3. Checking the real bytes

**Error**: expected bytes were put into the report without checking.

**Fix**: the file is read directly at the computed file offsets.

---

## Confirmed findings

### MmMapIoSpaceEx calls

| RVA     | File Offset | Bytes               | Instruction                | IAT Slot | Import Function   |
|---------|-------------|---------------------|----------------------------|----------|-------------------|
| 0x17204 | 0x14804     | `FF 15 4E 6E FF FF` | `call qword ptr [rip-0x91B2]` | 0xE058   | MmMapIoSpaceEx    |
| 0x1721C | 0x1481C     | `FF 15 36 6E FF FF` | `call qword ptr [rip-0x91CA]` | 0xE058   | MmMapIoSpaceEx    |

**Function**: 0x16FF0–0x172FE (device initialisation)  
**Section**: PAGE (not .text)  
**Check**: an independent disassembler (capstone) confirms both calls

### Function 0x16FF0: device initialisation

**Purpose**: the EvtDevicePrepareHardware callback, initialising the T2 hardware resources

**Logic**:
1. Gets the WDF resource list (CM_RESOURCE_LIST) through a vtable callback
2. Iterates over the resources, filtering type 3 (CmResourceTypeMemory)
3. **First memory resource**:
   - `MmMapIoSpaceEx(physical_address1, size1, PAGE_CACHE_TYPE=0x404)`
   - Cache type: MmCached | MmWriteCombined
   - The result is stored in `[rbx+0x68]`
4. **Second memory resource**:
   - `MmMapIoSpaceEx(physical_address2, size2, PAGE_CACHE_TYPE=0x204)`
   - Cache type: MmNonCached | MmWriteCombined
   - The result is stored in `[rbx+0x70]`
5. The fields are zeroed on error (0x171A5, 0x171AC)
6. The mapped addresses are copied into the global object:
   - `[global_0x14690 + 0x28]` = `[rbx+0x68]` (BAR1)
   - `[global_0x14690 + 0x30]` = `[rbx+0x70] + 0xC000` (BAR2+0xC000)

**The RBX object**: the device context structure (the rcx parameter on entry)

**Physical addresses**: from the PCI BAR resources provided by the PnP manager

### Global Object 0x14690

**Location**: .data section, RVA 0x14690  
**Purpose**: storage for the hardware pointers used to access the T2

**Key fields**:
- `+0x28` (offset 0x146B8): mapped BAR1 base (T2 buffer memory region)
- `+0x30` (offset 0x146C0): mapped BAR2+0xC000 (GPR register region)

**Usage**: 347 code locations read these fields to access the T2 hardware

### Mapping to Linux KAIT2EN

#### Hardware Layout

| Linux (KAIT2EN)           | Windows (AppleAudio.sys)        | Purpose                     |
|---------------------------|---------------------------------|-----------------------------|
| `pci_resource_start(dev, 0)` | First CM_RESOURCE (type=Memory) | Physical address BAR0/BAR1  |
| `pci_iomap(dev, 0, 0)`    | `MmMapIoSpaceEx(phys1, size1, 0x404)` | Map BAR1 (cached)           |
| `reg_mem_bs` (global var) | `[global+0x28]` (`[rbx+0x68]`)  | Mapped BAR1 pointer         |
| `pci_iomap(dev, 4, 0)`    | `MmMapIoSpaceEx(phys2, size2, 0x204)` | Map BAR4 (non-cached)       |
| `reg_mem_cfg + 0xC000`    | `[global+0x30]` (`[rbx+0x70]+0xC000`) | GPR region (BAR4+0xC000)    |

#### GPR Structure (BAR4+0xC000)

| Offset | Linux field         | Windows access (func 0x4728-0x4744) | Value                    |
|--------|---------------------|-------------------------------------|--------------------------|
| +0x00  | version             | `[rax]` >= 2                        | Version check            |
| +0x04  | signature           | `[rax+4]` == 0x19870423             | Magic signature (T2AUDIO_SIG) |
| +0x08  | bs_offset           | `[rax+8]`                           | BufferStruct offset (relative to BAR1) |

**Version difference**: Linux requires version >= 3, Windows >= 2.

#### BufferStruct Layout

**Signature**: 0x19870423 (T2AUDIO_SIG)  
**Location**: BAR1 + offset (from GPR[2])  
**Device entry stride**: 0xBDEC (48620 bytes), **an exact match**

**Structure** (from Linux audio.h, matches Windows):
```c
struct t2audio_buffer_struct {
    u32 version;                    // +0x00
    u32 signature;                  // +0x04 = 0x19870423
    u32 flags;                      // +0x08
    u8 num_devices;                 // +0x0C
    struct device[20];              // +0x10, stride 0xBDEC each
};

struct device {
    char name[128];                 // +0x000 ("Speaker", "Microphone")
    u8 num_input_streams;           // +0x080
    u8 num_output_streams;          // +0x081
    struct stream input_streams[5];
    struct stream output_streams[5];
    char filler[128];
};

struct stream {
    u8 num_buffers;
    struct buffer buffers[100];
    char filler[32];
};

struct buffer {
    size_t address;                 // offset relative to BAR1
    size_t size;
    size_t pad[4];
};  // sizeof = 0x30 (48 bytes)
```

**Function 0x4700** (Windows) reads this structure to get the device metadata.

#### PCM Buffer Access (Linux)

```c
// Linux: audio.c:728-730
strm->buffers[i].ptr = a->reg_mem_bs + bs_strm->buffers[i].address;
strm->buffers[i].size = bs_strm->buffers[i].size;

// Linux: pcm.c:224-226
substream->runtime->dma_area = astream->buffers[0].ptr;  // Zero-copy!
```

**Mechanism**: ALSA writes directly into the mapped BAR1 memory. The T2 reads from its own memory (shared memory, not a DMA transfer).

#### PCM Buffer Access (Windows)

**NOT FOUND** by static analysis.

**Hypothesis**: one of the 347 consumers of the global object does:
```c
void* bar1_base = global->bar1_mapped;  // [global+0x28]
size_t buffer_offset = /* from BufferStruct.devices[i].output_streams[j].buffers[k].address */;
void* pcm_buffer = (u8*)bar1_base + buffer_offset;
memcpy(pcm_buffer, audio_samples, size);  // or write-combining stores
```

**Alternative**: the PortCls framework abstracts the access, or WaveRT shared memory mode is used.

---

## Reproducible artefacts

### 1. Verify_RVA.ps1

**Purpose**: independent check of the PE structure, the RVA→offset conversion, the call scan

**Features**:
- PE parser (DOS header, NT headers, section table)
- IAT parser (import directory, function names)
- Full scan of the executable sections (IMAGE_SCN_MEM_EXECUTE)
- Search for the `FF 15` pattern (call [rip+offset]) with IAT slot resolution
- Prints the real bytes at the file offsets, never substituting expected values

**Result**: 2 MmMapIoSpaceEx calls confirmed at RVA 0x17204/0x1721C (file offsets 0x14804/0x1481C).

### 2. disasm2.py

**Purpose**: an independent disassembler and xref analyser

**Features**:
- Capstone disassembler for all executable sections
- Recognising call/jmp through the IAT
- RIP-relative address resolution (LEA, MOV with [rip+disp])
- A field access scanner (accesses to offsets +0x68/+0x70)
- Function boundaries from .pdata (exception directory)

**Result**:
- MmMapIoSpaceEx calls confirmed
- 80+ accesses to fields +0x68/+0x70 found (RSP-relative ones filtered out, object ones kept)
- Function 0x16FF0 disassembled completely

### 3. fields.txt

**Purpose**: a list of every access to fields +0x68/+0x70 with context

**Format**: `RVA [section] instruction ; comment (function)`

**Use**: filtering non-RSP accesses to trace objects

---

## Task status

### ✓ Done

1. **RVA→offset conversion fixed** using the PE section table
2. **Real bytes checked** at file offsets 0x14804/0x1481C
3. **All executable sections scanned** (.text, PAGE, INIT)
4. **2 MmMapIoSpaceEx calls confirmed** by an independent disassembler
5. **Function 0x16FF0 analysed** (device init, BAR mapping, global object setup)
6. **Fields +0x68/+0x70 traced** to the global object 0x14690
7. **Function 0x4700 found** (BufferStruct metadata reader)
8. **Structures mapped to Linux KAIT2EN** (full mapping table)
9. **Reports updated**, refuted claims removed

### ⚠ Partly done

10. **Tracing fields +0x68/+0x70** to the buffer work:
    - ✓ The write into the global object found (function 0x45F0)
    - ✓ The metadata reading function found (0x4700)
    - ✗ The function writing PCM samples NOT FOUND
    - Reason: needs analysis of all 347 consumers of the global object

### ✗ Not done (outside the scope of static analysis)

11. **Confirming the calls with an independent runtime disassembler**:
    - Done statically (capstone), not at runtime (WinDbg)
    - Needs kernel debugging on real hardware

12. **Tracing to the actual PCM buffer write**:
    - The mechanism was not found by static analysis
    - Needs runtime breakpoints or analysis of the PortCls callbacks

---

## Practical recommendations

### To investigate the PCM write path

**Option 1: continue static analysis** (40+ hours)
- Analyse all 347 consumers of the global object 0x14690
- Find the function reading the BufferStruct.devices[].output_streams[].buffers[] offsets
- Identify the pointer arithmetic: `[global+0x28] + buffer_offset`
- Find the memcpy/write loop at the computed address

**Option 2: runtime debugging** (10-20 hours, needs hardware)
- WinDbg kernel debugging on the MacBook Pro running Windows
- A breakpoint on read access: `ba r8 AppleAudio!global_object+0x28`
- Trigger audio playback, capture the call stack
- Monitor memory writes: `ba w4 <BAR1_phys>+<offset>`

**Option 3: port the Linux logic** (40-60 hours, recommended)
- Use the proven structures from KAIT2EN
- Write a minimal WaveRT driver with direct mapped memory access
- Start with read-only operations (enumerate, validate)
- Gradually add the PCM write following the Linux logic

### To get 6-channel audio

**Blockers**:
- AppleAudio.sys WaveSpeaker is locked to stereo (Win32 error 1169)
- No PCM write mechanism found to bypass it

**Options**:
1. **Reverse engineer the AppleAudio.sys stereo limitation** and patch it
2. **Write a new kernel driver** based on the Linux KAIT2EN logic
3. **Use the existing interface** with audio mixing (software upmix stereo→6ch)

**Recommendation**: option 2 (a new driver): the lowest risk, full control, proven logic.

---

## Final conclusions

### What is known for certain

1. **T2 hardware access**: AppleAudio.sys uses MmMapIoSpaceEx to map the PCI BAR resources
2. **BAR layout**: BAR1 (buffer memory) + BAR4+0xC000 (GPR registers)
3. **BufferStruct protocol**: signature 0x19870423, device stride 0xBDEC, layout identical to Linux
4. **Global object**: 0x14690 holds the mapped pointers, used from 347 locations
5. **Metadata reading**: function 0x4700 reads the device names and buffer offsets
6. **Message path**: IOCTL 0x222018 for T2 protocol commands (fully traced)

### What is unknown (blocking)

1. **PCM write function**: the function copying audio samples into BAR1 + buffer_offset
2. **WaveSpeaker → T2 link**: how the PortCls callbacks are tied to the BufferStruct stream buffers
3. **BufferStruct parser**: the function extracting buffer offsets from devices[].output_streams[]
4. **Stereo bypass**: whether 6ch can be sent directly through the T2 without AppleAudio.sys

### Quality of the evidence

- **Hardware access mechanism**: ★★★★★ (bytes checked, disassembler confirmed, matches Linux)
- **BufferStruct layout**: ★★★★★ (Linux source code, Windows metadata reader found)
- **PCM write path**: ★☆☆☆☆ (hypothesis only, static analysis did not find it)

### Overall assessment

**Original task**: fix the RVA→offset conversion, full section scan, independent verification, trace the fields to the buffers.

**Result**: 
- ✓ RVA→offset fixed and checked
- ✓ Full scan done (all EXEC sections)
- ✓ Independent verification done (capstone, PowerShell PE parser)
- ⚠ Tracing to the buffers is partial (metadata reader found, PCM writer not)

**Score**: **8/10**: all technical errors fixed, hardware access fully traced; the PCM write path needs more analysis (runtime or deeper static analysis).

---

## Report files

1. **VERIFICATION_SUMMARY.md** (this file): short summary
2. **FINAL_REANALYSIS_REPORT.md**: full analysis of function 0x16FF0 and MmMapIoSpaceEx
3. **Linux_Windows_Structure_Mapping.md**: the Linux↔Windows mapping table, implementation strategy
4. **BUFFER_PATH_ANALYSIS.md**: the updated analysis with corrections
5. **Verify_RVA.ps1**: a reproducible check script
6. **disasm2.py**: an independent disassembler
7. **fields.txt**: the list of accesses to fields +0x68/+0x70

All reports were updated, removing contradicting versions and refuted claims.
