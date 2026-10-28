# Final report of the AppleAudio.sys re-analysis

**Date**: 2026-10-04  
**Task**: independent re-verification of the MmMapIoSpaceEx calls, fixing the RVA→offset conversion errors, full tracing to the buffer usage

> Archived. Later work answered the open questions: the speaker ring is `OutputStreams[0].Buffers[0]` of the "Speaker" device in BAR0 (read by `0x9D74`), PCM is written by AppleAudio's copy loop `0x14000A715`–`0x14000B2C0`, and the GPR block is at config-BAR + 0xC000 (not a BufferStruct). See `docs/DRIVER_GUIDE.md` §2 and `docs/analysis/RE_appleaudio_protocol.md`.

---

## Verification result

### Binary SHA256: ✓ MATCHES

```
File checked: C:\Users\othysa\Desktop\mbp\MBP-Audio-driver\AppleAudio.sys
SHA256: CD8B81FABC9BE6127A082DA22C4CC046A4D46E0225EB6BAF6D27E0DC9862A150
Size: 122752 bytes (0x1DF00)
PE Timestamp: 0x5F73BACC
```

This is the **RIGHT binary**, AppleAudio.sys 6.1.8100.4.

### Fixing the RVA → file offset conversion error

**Problem in the previous analysis**: the (RVA - raw offset) difference of the .text section was applied to every section.

**The correct section table**:

```
.text    VA=0x001000  RawOff=0x000400  RawSize=0x00C800  EXEC
.rdata   VA=0x00E000  RawOff=0x00CC00  RawSize=0x003600
.data    VA=0x012000  RawOff=0x010200  RawSize=0x002800
.pdata   VA=0x015000  RawOff=0x012A00  RawSize=0x000C00
PAGE     VA=0x016000  RawOff=0x013600  RawSize=0x007200  EXEC
INIT     VA=0x01E000  RawOff=0x01A800  RawSize=0x000E00  EXEC
.rsrc    VA=0x01F000  RawOff=0x01B600  RawSize=0x000400
.reloc   VA=0x020000  RawOff=0x01BA00  RawSize=0x000400
```

**The correct conversion for the PAGE section**:
```
file_offset = (RVA - 0x16000) + 0x13600
```

**Checking the RVAs provided**:

| RVA      | Section | File offset | Actual bytes (start)            |
|----------|---------|-------------|----------------------------------|
| 0x17204  | PAGE    | 0x14804     | `FF 15 4E 6E FF FF 48 89 43 70` |
| 0x1721C  | PAGE    | 0x1481C     | `FF 15 36 6E FF FF 48 89 43 68` |

**Disassembly**:

```asm
; RVA 0x17204 (file offset 0x14804)
call qword ptr [rip-0x91B2]    ; FF 15 4E 6E FF FF
mov [rbx+0x70], rax            ; 48 89 43 70

Target IAT slot: 0x17204 + 6 - 0x91B2 = 0xE058

; RVA 0x1721C (file offset 0x1481C)
call qword ptr [rip-0x91CA]    ; FF 15 36 6E FF FF
mov [rbx+0x68], rax            ; 48 89 43 68

Target IAT slot: 0x1721C + 6 - 0x91CA = 0xE058
```

**IAT slot 0xE058**: `ntoskrnl.exe!MmMapIoSpaceEx` ✓ CONFIRMED

---

## Full check of the executable sections

### Scanning every EXEC section for calls to IAT 0xE058

**Method**: search for `FF 15` instructions (call [rip+offset]) in every section with the `IMAGE_SCN_MEM_EXECUTE` flag.

**Results**:

```
.text: 152 rip-relative calls found, 0 → IAT 0xE058
PAGE:  227 rip-relative calls found, 2 → IAT 0xE058
INIT:   11 rip-relative calls found, 0 → IAT 0xE058
```

**MmMapIoSpaceEx calls**:

| RVA     | Section | File offset | Instruction                 |
|---------|---------|-------------|-----------------------------|
| 0x17204 | PAGE    | 0x14804     | `FF 15 4E 6E FF FF`         |
| 0x1721C | PAGE    | 0x1481C     | `FF 15 36 6E FF FF`         |

**Conclusion**: AppleAudio.sys has exactly **2 MmMapIoSpaceEx calls**, both in the PAGE section.

---

## Independent check with the capstone disassembler

### Function 0x16FF0–0x172FE (full analysis)

**Signature** (reconstruction):
```c
NTSTATUS InitializeMappedResources(
    void* object,           // rcx (kept in rbx)
    void* physicalDevice,   // rdx (kept in rbp)
    void* context           // r8  (kept in r14)
);
```

**Key operations**:

#### 1. Initialising the object fields (0x16FF0–0x17044)

```asm
0x16FF0: push rbx/rbp/rsi/rdi/r13/r14/r15
0x16FFB: sub rsp, 0x40
0x16FFF: xor r15d, r15d                    ; r15 = 0
0x17002: mov [rcx+0x30], rdx               ; obj->field_30 = physDevice
0x17006: mov [rcx+0x50], r15               ; obj->field_50 = 0
0x17011: mov [rcx+0x28], r15               ; obj->field_28 = 0
0x17019: mov [rcx+0x58], r15               ; obj->field_58 = 0
0x17021: mov [rdi], r15                    ; rdi = obj + 0x40
0x17024: mov rbx, rcx                      ; rbx = object
0x17032: mov dword [rcx+0x48], 1          ; obj->field_48 = 1
0x17035: mov r14, r8                       ; r14 = context
0x1703E: call [rip-0x8E0C]                 ; PcGetPhysicalDeviceObject
0x17044: test eax, eax
0x17046: js error
```

#### 2. Buffer allocation and initialisation (0x1707C–0x170CF)

```asm
0x1707C: call [rip-0x8E32]                 ; unknown WDF/PortCls function
0x1708A: mov edx, 0x200                    ; size = 512
0x1708F: mov ecx, 0x100                    ; pool type
0x17094: mov r8d, 0x44554141               ; tag 'AUUD'
0x1709A: call 0x13A4                       ; ExAllocatePool
0x1709F: test rax, rax
0x170A2: je error
0x170A7: call 0x17E08                      ; initialise the allocated object
0x170B1: mov [rbx+0x50], rax               ; obj->field_50 = allocated_buffer
0x170CA: call 0x17E74                      ; further setup
```

#### 3. Device interface registration (0x170D6–0x17247)

```asm
0x170DD: mov rdx, [rdi]
0x170E0: lea r8, [rip-0x8647]
0x170E7: mov ecx, 0x40
0x17109: call [rip-0x8EBF]                ; unknown WDF function
0x17156: call [rip-0x8F0C]                ; possibly IoSetDeviceInterfaceState
0x17197: call [rip-0x8F4D]                ; another WDF call
```

#### 4. Zeroing the mapped pointers (0x171A2–0x171AC)

```asm
0x171A2: lfence
0x171A5: mov qword ptr [rbx+0x68], r15    ; obj->mapped_bar1 = NULL
0x171AC: mov qword ptr [rbx+0x70], r15    ; obj->mapped_bar2 = NULL
```

#### 5. Iterating over the resources, the MmMapIoSpaceEx calls (0x171B0–0x1722F)

```asm
0x171B0: mov rcx, r14                      ; rcx = context
0x171B3: mov rax, [r14]                    ; rax = context->vtable
0x171B9: lea edx, [r8+3]                   ; edx = 3
0x171BD: mov rax, [rax+0x28]               ; rax = vtable[5] (GetResourceCount?)
0x171C1: call [rip-0x8F77]                 ; indirect call through CFG
0x171C7: mov rcx, [r14]
0x171CF: mov edx, 3
0x171D2: mov rax, [rcx+0x20]               ; rax = vtable[4] (GetResourceList?)
0x171D6: mov rcx, r14
0x171D9: call [rip-0x8F8F]                 ; indirect call through CFG
0x171DF: mov r14d, eax                     ; r14d = resource_count
0x171E2: test eax, eax
0x171E4: je 0x17231
0x171E6: add rdi, r13                      ; r13 = 4
0x171E9: cmp byte ptr [rdi-4], 3          ; resource type check
0x171ED: jne 0x17228                       ; skip non-memory resources
0x171EF: test esi, esi                     ; esi = index (0, 1, 2...)
0x171F1: je 0x17210                        ; if (index == 0)

; Second memory resource (index != 0, but == 2 per the code)
0x171F3: cmp esi, 2
0x171F6: jne 0x17226
0x171F8: mov edx, [rdi+8]                  ; edx = size
0x171FB: mov r8d, 0x204                    ; r8d = cache_type (MmNonCached | MmWriteCombined)
0x17201: mov rcx, [rdi]                    ; rcx = physical_address
0x17204: call [rip-0x91B2]                 ; MmMapIoSpaceEx(phys, size, 0x204)
0x1720A: mov [rbx+0x70], rax               ; obj->mapped_bar2 = rax
0x1720E: jmp 0x17226

; First memory resource (index == 0)
0x17210: mov edx, [rdi+8]                  ; edx = size
0x17213: mov r8d, 0x404                    ; r8d = cache_type
0x17219: mov rcx, [rdi]                    ; rcx = physical_address
0x1721C: call [rip-0x91CA]                 ; MmMapIoSpaceEx(phys, size, 0x404)
0x17222: mov [rbx+0x68], rax               ; obj->mapped_bar1 = rax
0x17226: inc esi                           ; index++
0x17228: add rdi, 0x14                     ; next resource (stride 20 bytes)
0x1722C: cmp esi, r14d
0x1722F: jb 0x171E9                        ; loop
```

**MmMapIoSpaceEx call parameters**:

| Call  | RVA     | PhysAddr      | Size         | CacheType | Result            |
|-------|---------|---------------|--------------|-----------|-------------------|
| 1st   | 0x1721C | `[rdi+0]`     | `[rdi+8]`    | 0x404     | `[rbx+0x68]`      |
| 2nd   | 0x17204 | `[rdi+0]`     | `[rdi+8]`    | 0x204     | `[rbx+0x70]`      |

**RDI** points to the resource descriptor in the list provided through the context (r14).

**Cache types**:
- `0x404` = `MmCached | MmWriteCombined`
- `0x204` = `MmNonCached | MmWriteCombined`

#### 6. Storing the mapped addresses in the global object (0x17231–0x1724C)

```asm
0x17231: mov r8, [rbx+0x70]                ; r8 = obj->mapped_bar2
0x17235: mov rdx, [rbx+0x68]               ; rdx = obj->mapped_bar1
0x17239: add r8, 0xC000                    ; r8 = mapped_bar2 + 0xC000
0x17240: mov rcx, [rip-0x2BB7]             ; rcx = global_object @ 0x14690
0x17247: call 0x45F0                       ; SetMappedAddresses(global, bar1, bar2+0xC000)
```

**Function 0x45F0** (3 instructions):
```asm
0x045F0: mov [rcx+0x28], rdx               ; global->bar1_mapped = bar1
0x045F4: mov [rcx+0x30], r8                ; global->bar2_mapped = bar2 + 0xC000
0x045F8: ret
```

#### 7. Further setup through registry/properties (0x1724C–0x172EA)

```asm
0x1724C: lfence
0x1724F: mov rax, [rip-0x2B6E]
0x1727C: call [rip-0x9032]                 ; RtlQueryRegistryValues or similar
0x172B3: call [rip-0x9069]                 ; another property call
0x172D9: call [rip-0x908F]                 ; another property call
0x172E2: call 0x19010                      ; cleanup/finalisation
0x172EA: call 0x18DC0                      ; cleanup/finalisation
0x172EF: add rsp, 0x40
0x172F3-0x172FD: pop r15/r14/r13/rdi/rsi/rbp/rbx; ret
```

---

## Tracing the use of the mapped addresses to the buffers

### Global object 0x14690

**Fields**:
- `+0x28`: mapped BAR1 (`[rbx+0x68]` from function 0x16FF0)
- `+0x30`: mapped BAR2 + 0xC000 (`[rbx+0x70]+0xC000` from function 0x16FF0)

**Accesses to the fields**: 347 found by scanning every EXEC section.

### Function 0x4700–0x4892: working with the mapped buffer

**Key code** (RVA 0x4718–0x4750):

```asm
0x04718: mov rax, [rcx+0x30]               ; rcx = global @ 0x14690
                                           ; rax = mapped_bar2 + 0xC000
0x0471C: xor sil, sil
0x0471F: mov rdi, r9
0x04722: mov r14, rdx
0x04725: mov r15, rcx
0x04728: cmp dword ptr [rax+4], 0x19870423 ; magic signature check
0x0472F: je 0x473B
0x04731: mov eax, 0xC0000001               ; STATUS_UNSUCCESSFUL
0x04736: jmp 0x4892

0x0473B: cmp dword ptr [rax], 2            ; version check
0x0473E: jb 0x488D
0x04744: mov ebx, [rax+8]                  ; ebx = buffer_offset
0x04747: xor ebp, ebp
0x04749: add rbx, [rcx+0x28]               ; rbx = mapped_bar1 + buffer_offset
                                           ; ← RBX now points to the buffer
0x0474D: mov al, [rbx+0xC]                 ; count = buffer[0xC]
0x04750: test al, al
0x04752: je 0x4782
0x04754: mov r12d, ebp
0x04757: lea rdx, [rbx+0x10]               ; rdx = buffer + 0x10
0x0475B: imul rax, r12, 0xBDEC
0x04762: mov r8d, 0x80                     ; size = 0x80
0x04768: lea rcx, [r14+0x21]               ; rcx = dest
0x0476C: add rdx, rax                      ; rdx = src (buffer entry)
0x0476F: call 0x4CC0                       ; memcpy-like function
0x04774: test eax, eax
0x04776: je 0x47EB
0x04778: movzx eax, byte ptr [rbx+0xC]
0x0477C: inc ebp                           ; index++
0x0477E: cmp ebp, eax
```

**Analysis**:

1. **Mapped BAR2 + 0xC000** holds the **metadata**:
   - `+0x00`: version (must be ≥ 2)
   - `+0x04`: magic `0x19870423`
   - `+0x08`: buffer offset relative to BAR1

2. **Mapped BAR1 + offset** → **the actual data buffer**:
   - `+0x0C`: number of elements
   - `+0x10+`: an array of structures of `0xBDEC` bytes each

3. **Usage**: copying data from the mapped buffer into a local driver buffer through function 0x4CC0. (Later: 0x4CC0 is `_strnicmp`, a name comparison.)

### Multiple accesses to the mapped addresses

**Functions reading `[global+0x28]` or `[global+0x30]`**:

- 0x29A8/0x29AC: reading both BARs
- 0x3632, 0x3730, 0x3830, 0x394B, 0x3A4B, 0x3B48, 0x3C48, 0x3D48: reading BAR1
- 0x4718–0x4750: the main buffer work (see above)
- 0x4A14, 0x4B94: reading BAR1

**Pattern**: the global object 0x14690 is the **central store** of the mapped addresses for the whole driver.

---

## Answers to the task's questions

### 1. What exactly is mapped?

**BAR1** (the first memory resource):
- Size: `[rdi+8]` (from the CM_RESOURCE_LIST)
- Cache type: 0x404 (Cached | WriteCombined)
- Purpose: **the main data buffer** (metadata + array of structures)

**BAR2** (the second memory resource):
- Size: `[rdi+8]`
- Cache type: 0x204 (NonCached | WriteCombined)
- Purpose: **control registers** (the metadata at offset +0xC000 point to an offset in BAR1)

**The physical addresses** come from the WDF resources (passed through the context r14 to function 0x16FF0).

### 2. Where do the physical addresses and sizes come from?

**Source**: the WDF framework provides the resource list through the context object (parameter r8 of function 0x16FF0).

**Mechanism**:
1. Calling vtable[5] (0x171BD): get the resource count
2. Calling vtable[4] (0x171D2): get the resource list
3. Iterating over the list with a type check (byte ptr [rdi-4] == 3 → memory resource)

**Resource descriptor format**:
- `+0x00`: physical address (QWORD)
- `+0x08`: size (DWORD)
- `+0x0C`: flags/type
- Stride: 0x14 (20 bytes)

### 3. What object is in RBX?

**RBX** = the driver's local object (the rcx parameter of function 0x16FF0).

**Structure** (partial reconstruction):

```c
struct LocalDriverObject {
    // ...
    void*  field_28;        // +0x28
    void*  field_30;        // +0x30
    void*  field_38;        // +0x38
    void*  field_40;        // +0x40 (rdi = rcx + 0x40)
    DWORD  field_48;        // +0x48 = 1
    void*  allocated_buffer;// +0x50 (tag 'AUUD', size 512)
    void*  field_58;        // +0x58
    void*  mapped_bar1;     // +0x68 ← the first MmMapIoSpaceEx
    void*  mapped_bar2;     // +0x70 ← the second MmMapIoSpaceEx
    // ...
};
```

**Purpose**: temporary storage of the mapped addresses before they are copied into the global object 0x14690.

### 4. Why are the mapping results stored in `[rbx+0x70]` and `[rbx+0x68]`?

**Goal**: keeping the `MmMapIoSpaceEx` results before passing them to the global object.

**Data flow**:
```
MmMapIoSpaceEx (0x1721C) → [rbx+0x68]
MmMapIoSpaceEx (0x17204) → [rbx+0x70]
           ↓                     ↓
      (0x17231-0x17235)    (read)
           ↓                     ↓
      function 0x45F0
           ↓
   [global_0x14690 + 0x28] = [rbx+0x68]
   [global_0x14690 + 0x30] = [rbx+0x70] + 0xC000
```

**Why is offset 0xC000 added?**
- BAR2 holds the control registers and metadata tables.
- Offset 0xC000 points to the **BufferStruct** (like the GPR registers in Linux kait2en). (Correction: it points to the GPR block, which in turn holds the BufferStruct offset.)
- That structure holds the offset of the buffer in BAR1.

### 5. Where do the mapped addresses go next?

**Global object 0x14690**:
- Fields `+0x28` and `+0x30` are reachable from 347 places in the code.
- The main buffer function: **0x4700–0x4892**.
- Operation: **reading data structures from mapped memory and copying them into local buffers**.

**Link to the audio buffers**:
- Function 0x16FF0 is called at device initialisation (DriverEntry/AddDevice).
- The global object 0x14690 is the central store for access to the T2 hardware.
- The data buffers (through BAR1) hold **device metadata, not the PCM buffers directly**.

**Not found**: a link to the WaveSpeaker KS pin callback or to PCM rendering.

---

## Corrections of earlier errors

### 1. RVA → file offset conversion ✓ FIXED

**Before**: the .text offset was applied to every section.  
**Now**: per-section conversion from the section table.

### 2. "MmMapIoSpaceEx is not used" ✗ WAS WRONG

**Before**: "IAT@0xE058 is not used, 0 calls found".  
**Fact**: **2 calls** found in the PAGE section (RVA 0x17204, 0x1721C).  
**Cause of the error**: only .text was searched; PAGE and INIT were skipped.

### 3. "RVA 0x17204 = function epilogue" ✗ WAS WRONG

**Before**: "0x17204: mov rbp, [rsp+0x38]; add rsp, 0x20; pop rdi; ret".  
**Fact**: that was the content of RVA 0x171F4 (an offset computation error).  
**Correct**: RVA 0x17204 = `call [rip-0x91B2]` → MmMapIoSpaceEx.

### 4. Offsets +0x68/+0x70 "are not used" ✗ WAS WRONG

**Before**: "The patterns `48 89 43 68` and `48 89 43 70` were not found".  
**Fact**: found at RVA 0x17222 and 0x1720A respectively.  
**Cause of the error**: only .text was searched; the PAGE section was skipped.

### 5. Function 0x1FA0 MmMapLockedPagesSpecifyCache ≠ T2 buffers

**Before, in BUFFER_PATH_CORRECTIONS.md**: an assumed link to the PCM buffers.  
**Fact**: function 0x1FA0 is not called from function 0x16FF0 (different initialisation chains).  
**Conclusion**: MmMapLockedPagesSpecifyCache (0x2051) and MmMapIoSpaceEx (0x17204/0x1721C) are **different mechanisms**, not directly related. (Later: 0x1FA0 allocates the input/microphone buffers.)

---

## Reproducible check script

### Verify_RVA.ps1

**File**: `C:\Users\othysa\Desktop\mbp\Verify_RVA.ps1`

**Features**:
1. Parsing the PE headers (sections, imports)
2. Correct RVA → file offset conversion (per section)
3. Byte dumps at a file offset
4. A full scan of every EXEC section for `call [rip+offset]` → IAT targets
5. Identifying the IAT slots through the Import Directory Table

**Run**:
```powershell
pwsh -NoProfile -File Verify_RVA.ps1
```

**Expected result**:
```
=== Scan of ALL EXEC sections ===
.text: 152 rip-relative calls found
PAGE:  227 rip-relative calls found
INIT:   11 rip-relative calls found

--- Calls targeting IAT slot 0xE058 (MmMapIoSpaceEx) ---
  PAGE: RVA 0x17204 (file 0x14804)  bytes: FF 15 4E 6E FF FF
  PAGE: RVA 0x1721C (file 0x1481C)  bytes: FF 15 36 6E FF FF
```

### disasm2.py

**File**: `C:\Users\othysa\Desktop\mbp\disasm2.py`

**Features**:
1. Independent disassembly with capstone
2. Automatic annotation of IAT calls and rip-relative data
3. Xref search (call/jmp/lea references to an RVA)
4. Field scan (accesses to offsets 0x68/0x70)

**Run**:
```bash
py disasm2.py dis 16FF0 172FE    # disassemble the function
py disasm2.py xref 16FF0          # find xrefs
py disasm2.py field               # scan fields +0x68/+0x70
```

**Checking the MmMapIoSpaceEx calls**:
```bash
py disasm_func.py 16FF0 172FE | Select-String "17204|1721C"
```

**Result**:
```
0x17204: call     qword ptr [rip - 0x91b2]   ; -> IAT 0xE058 = ntoskrnl.exe!MmMapIoSpaceEx
0x1721C: call     qword ptr [rip - 0x91ca]   ; -> IAT 0xE058 = ntoskrnl.exe!MmMapIoSpaceEx
```

---

## Conclusion

### Confirmed facts

✓ AppleAudio.sys **uses MmMapIoSpaceEx** (2 calls in function 0x16FF0)  
✓ **2 T2 memory resources** are mapped (BAR1 + BAR2)  
✓ BAR1: the main data buffer (Cached|WriteCombined)  
✓ BAR2+0xC000: metadata and control registers (NonCached|WriteCombined)  
✓ The mapped addresses are kept in the global object 0x14690 (fields +0x28, +0x30)  
✓ The global object is reachable from **347 places** in the code  
✓ The main buffer work: function 0x4700 (reading metadata + copying structures)  
✓ The magic signature `0x19870423` confirms a structured T2 protocol

### What was NOT found

✗ **A direct link to the PCM buffers**: function 0x16FF0 runs at device initialisation, not during audio playback  
✗ **The WaveSpeaker pin callback**: the chain from KS audio rendering to the mapped buffers was not traced  
✗ **Writing PCM data**: function 0x4700 **reads** from mapped memory, it does not write  
✗ **The link to IOCTL 0x222018**: the message path (0x4A60) and the buffer path (0x16FF0) are independent mechanisms

### Unresolved questions

1. **Where is function 0x16FF0 called from?** The xref search found no direct calls (probably dispatch through a vtable or callback).
2. **Where are the PCM data written?** The mapped buffers are used to read metadata, not to write audio samples.
3. **The link between the BAR1 buffers and WaveSpeaker?** Not established by the static analysis.
4. **The purpose of the "BufferStruct" at BAR2+0xC000?** It holds an offset, version, magic, like the Linux GPR mechanism, but the structure details are not fully reconstructed.

### Overall assessment

**MmMapIoSpaceEx calls: FOUND and CONFIRMED** (2 calls, RVA 0x17204 + 0x1721C).

**Mapped addresses: traced to the global object and the buffer functions**.

**PCM audio path: NOT FOUND**; it needs runtime debugging or more static analysis of the functions using the global object 0x14690.

**Next step**: a runtime breakpoint on MmMapIoSpaceEx (0x17204/0x1721C) to capture the physical addresses and sizes of the mapped regions.

---

## Mapping to Linux KAIT2EN

A detailed mapping table of the structures, fields and mechanisms between the Linux driver and AppleAudio.sys: **[Linux_Windows_Structure_Mapping.md](../../analysis/Linux_Windows_Structure_Mapping.md)**

**Key findings**:
- ✓ BAR mapping: Linux BAR0/BAR4 ↔ Windows [rbx+0x68]/[rbx+0x70]
- ✓ GPR structure: Linux reg_mem_gpr+0xC000 ↔ Windows [global+0x30]
- ✓ BufferStruct signature 0x19870423: found in both drivers
- ✓ Device stride 0xBDEC: matches exactly
- ✗ PCM write path: found on Linux (zero-copy ALSA), NOT FOUND on Windows

**Recommendation**: a hybrid approach: use the proven Linux BufferStruct parsing logic + the existing Windows BAR mapping initialisation. Effort estimate: 40-60 hours for a minimal viable PCM playback driver.
