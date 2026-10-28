# CRITICAL ERRORS IN THE PREVIOUS ANALYSIS: CORRECTIONS

> **Status note (2026-10-28):** historical. The open questions below were answered later: function `0x1FA0` (`FUN_140001fa4`) allocates the **input** (microphone) buffers and registers them in BufferStruct (`AppleAudio_Reverse_Engineering_Report.md` §5, `RE_appleaudio_protocol.md` §7); the speaker output uses the ring provided by the T2 in BAR0, mapped by `MmMapIoSpaceEx` elsewhere (`BUFFER_PATH_ANALYSIS.md`), and PCM is written by the copy loop `0x14000A715`–`0x14000B2C0`.

**Binary**: AppleAudio.sys 6.1.8100.4  
**SHA256**: cd8b81fabc9be6127a082da22c4cc046a4d46e0225eb6baf6d27e0dc9862a150 ✓ VERIFIED  
**Size**: 122752 bytes  
**PE Timestamp**: 0x5F73BACC

---

## CHECKING THE LANDMARKS GIVEN IN THE TASK

### 1. MmMapIoSpaceEx IAT RVA = 0xE058 ✗ WRONG

**Check**:
- IAT slot 0xE058 points to RVA 0x1E860 (an import string in .rdata)
- Search for every `call [rip+offset]` targeting 0xE058: **0 hits**
- The string "MmMapIoSpaceEx" is at RVA 0x1E862

**Conclusion**: IAT@0xE058 is NOT used for MmMapIoSpaceEx calls in this binary.

### 2. Direct calls at RVA 0x17204 and 0x1721C ✗ WRONG

**Check of RVA 0x17204**:
```
0x171F4: 00 C0 48 8B 5C 24 30 8B C6 48 8B 74 24 40 48 8B
0x17204: 6C 24 38 48 83 C4 20 5F C3  ← function epilogue (ret)
         mov rbp, [rsp+0x38]
         add rsp, 0x20
         pop rdi
         ret
```

**Check of RVA 0x1721C**:
```
0x1720C: C3 CC CC CC 48 89 0D 89 B6 FF FF 33 C0 C3 CC CC
0x1721C: 48 89 5C 24 08  ← a new function (prologue)
         mov [rsp+8], rbx
```

**Conclusion**: these RVAs hold a function return / function start, **NOT MmMapIoSpaceEx calls**.

### 3. The pattern "mov edx, [rdi+8]; mov r8d, 0x204" ✗ NOT FOUND

Searching for the bytes `8B 57 08` (mov edx, [rdi+8]) + `41 B8 04 02 00 00` (mov r8d, 0x204): **0 hits**.

### 4. The pattern "mov r8d, 0x404" ✗ NOT FOUND

Searching for the bytes `41 B8 04 04 00 00`: **0 hits**.

### 5. The patterns "mov [rbx+0x68], rax" and "mov [rbx+0x70], rax" ✗ NOT FOUND

Searching for the bytes `48 89 43 68` and `48 89 43 70`: **0 hits**.

---

## ACTUAL RESULTS OF THE ANALYSIS

### The only memory-mapping call found

**RVA 0x2051**: MmMapLockedPagesSpecifyCache
```
0x2040: 40 45 33 C9 83 64 24 20 00 45 33 C0 33
0x2050: D2 FF 15 A9 C0 00 00  ← call [rip+0xC0A9] -> IAT@0xE100
        xor edx, edx
        call [rip+0xC0A9]  ; MmMapLockedPagesSpecifyCache
        
0x2057: 48 89 44 24 30          ← mov [rsp+0x30], rax
0x205C: 0F AE E8                ← lfence
0x205F: 48 85 C0                ← test rax, rax
0x2062: 0F 84 89 01 00 00       ← je error_handler
```

**Function parameters** (x64 fastcall convention):
- rcx: MDL pointer (its origin needs back-tracing)
- rdx: 0 (xor edx, edx before the call)
- r8d: 0 (45 33 C0 = xor r8d, r8d at 0x2045)
- r9d: 0 (45 33 C9 = xor r9d, r9d at 0x2043)
- [rsp+0x20]: 0 (83 64 24 20 00 at 0x2047)

**Result**:
- rax = virtual address of the mapped pages
- Stored in [rsp+0x30]

### The function containing the call

**Start**: RVA 0x1FA0 (by the prologue pattern)
```
0x1FA0: 44 89 44 24 18 48 89 54 24 10 48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 40
        mov [rsp+0x18], r8d
        mov [rsp+0x10], rdx
        mov [rsp+0x08], rcx
        push rbp, rsi, rdi, r12, r13, r14, r15
        sub rsp, 0x40
```

**Context** (code before the MmMapLockedPagesSpecifyCache call):
```
0x1FF0: 00 0F 86 13 02 00 00 48 8B 7C 24 30 4C 8D BA C8
0x2000: 0C 00 00 49 83 3F 00 0F 85 AB 01 00 00 44 8B CD
        mov rdi, [rsp+0x30]         ; rdi = a function parameter (the 1st?)
        lea r15, [r10+0xCC8]        ; r15 = pointer + 0xCC8
        cmp qword [r15], 0          ; check [r15]
        jne label                   ; if not zero - already initialised
        
0x2010: 45 33 C0 48 8B D7 B9 00 20 00 00 FF 15 EF C0 00
        xor r8d, r8d
        mov rdx, rdi                ; rdx = the first parameter
        mov ecx, 0x2000             ; ecx = 0x2000 (8192 bytes)
        call [rip+0xC0EF]           ; some call (allocation?)
        
0x2020: 00 48 8B F0 48 85 C0 0F 84 D8 01 00 00 39 68 28
        mov rsi, rax                ; rsi = the result
        test rax, rax
        je error
        cmp [rax+0x28], ebp         ; check field +0x28
        
0x2030: 48 8B C8 0F AE E8 0F 82 B8 01 00 00 C7 44 24 28
        mov rcx, rax                ; rcx = the MDL for MmMapLockedPages
        lfence
        jb error
        mov dword [rsp+0x28], 0x40000020  ; cache flag
        
0x2040: 20 00 00 40 45 33 C9 83 64 24 20 00 45 33 C0 33
0x2050: D2 FF 15 A9 C0 00 00        ; MmMapLockedPagesSpecifyCache call
```

**Context after the call**:
```
0x2057: 48 89 44 24 30              ; store rax in [rsp+0x30]
0x205C: 0F AE E8                    ; lfence
0x205F: 48 85 C0                    ; test rax, rax
0x2062: 0F 84 89 01 00 00           ; je error_handler
0x2068: 8B C5 41 B8 43 43 41 41     ; mov eax, ebp; mov r8d, 0x41414343 ('CCAA')
0x2070: C1 E8 0C B9 00 02 00 00     ; shr eax, 0x0C; mov ecx, 0x200
0x2078: FF C0 4C 8D 24 40           ; inc eax; lea r12, [rax+rax*2]
0x207E: 49 C1 E4 03                 ; shl r12, 3  ; r12 = (ebp >> 0xC + 1) * 24
0x2082: 49 8B D4                    ; mov rdx, r12
0x2085: FF 15 2D C0 00 00           ; call [IAT]  ; ExAllocatePool?
0x208B: 48 89 44 24 38              ; mov [rsp+0x38], rax
0x2090: 4C 8B F0                    ; mov r14, rax
0x2093: 4C 8B E8                    ; mov r13, rax
0x2096: 48 85 C0                    ; test rax, rax
0x2099: 0F 84 41 01 00 00           ; je error
```

---

## PURPOSE ANALYSIS

### What is mapped?

The function:
1. Allocates **8192 bytes** (0x2000) at RVA 0x2010
2. Checks field +0x28 of the object obtained
3. Maps the pages with **MmMapLockedPagesSpecifyCache** using the MDL from the first allocation
4. Stores the virtual address in [rsp+0x30]
5. Allocates an extra buffer of `(ebp >> 0xC + 1) * 24 bytes`

**ebp** = a function parameter (probably a size in bytes or pages).

### Link to audio buffers?

**NOT DETERMINED** from this piece of code.

**Needed**:
1. Trace the callers of this function (who calls RVA 0x1FA0?)
2. Analyse the ebp parameter (where does its value come from?)
3. Find the uses of [rsp+0x30] after return
4. The link to field +0xCC8 of the object (buffer initialisation?)

---

## CRITICAL CONCLUSIONS

### 1. All landmarks from the task are WRONG for this binary

- IAT@0xE058 is never called
- RVA 0x17204/0x1721C contain no MmMapIoSpaceEx
- The +0x68/+0x70 patterns are absent
- The constants 0x204/0x404 are absent

**Hypotheses**:
- The data was taken from another driver version
- RVAs were confused with file offsets
- A bug in the disassembler/analysis tool
- The information came from another module (not AppleAudio.sys)

### 2. MmMapIoSpaceEx NOT FOUND in the code

Searched through:
- IAT slots 0xE000-0xF000: 152 calls found, **none leads to 0xE058**
- The pattern "FF 15" (call [rip+offset]): no calls to MmMapIoSpaceEx
- Indirect calls (call rax/rcx/rdx): only 2 in .text

**Possibility 1**: MmMapIoSpaceEx is called from another module (AppleUSBVHCI.sys)  
**Possibility 2**: this driver uses only MmMapLockedPages (not IoSpace)  
**Possibility 3**: the mapping is done by a framework (WDF/PortCls), not explicitly

(Later finding: `MmMapIoSpaceEx` is called in function 0x16FF0, see `BUFFER_PATH_ANALYSIS.md`.)

### 3. The only MmMapLockedPagesSpecifyCache call

**RVA 0x2051** in function 0x1FA0.

**Known**:
- Maps an MDL of 8192 bytes
- Uses the cache flag 0x40000020
- The result is stored in a local variable
- Runs conditionally (check of [r15+0xCC8])

**NOT known**:
- Where function 0x1FA0 is called from
- What data the MDL holds (PCM? control structures?)
- Where the mapping result is written after the function returns
- The link to the WaveSpeaker/audio rendering pipeline

---

## RECOMMENDATIONS

### Continue the static analysis

**Step 1**: find the calls of function 0x1FA0
```powershell
# Look for call [rip+offset] -> 0x1FA0
# Look for call rva_absolute (E8 xx xx xx xx)
```

**Step 2**: trace the ebp parameter back to its source

**Step 3**: find the uses of [rsp+0x30] after the return from MmMapLockedPages

**Step 4**: check offset +0xCC8 in the object structures

**Step 5**: link to the known WaveSpeaker/Speaker/Audio symbols

### Runtime debugging option

**If** the static analysis gives nothing:
1. WinDbg kernel debugging
2. A breakpoint on MmMapLockedPagesSpecifyCache
3. Capture the call stack, parameters, return value
4. Check whether it is called during audio playback

### Checking AppleUSBVHCI.sys

**If** AppleAudio.sys does NOT do the mapping itself:
- Check the exports of AppleUSBVHCI.sys (it may provide functions)
- Check DeviceIoControl calls with unknown IOCTLs
- Perhaps the buffers are mapped in AppleUSBVHCI and the address returned through an IOCTL

---

## ANSWERS TO THE TASK'S QUESTIONS

### What exactly is mapped?

**UNKNOWN** (at the time).

The only mapping found (RVA 0x2051):
- Size: 8192 bytes (0x2000)
- Type: MmMapLockedPagesSpecifyCache (MDL mapping)
- Purpose: needs further tracing

### Was the PCM buffer found?

**NO**.

No link between the mapping found and the PCM buffers was established.

### Which call/field remains unresolved?

**Unresolved**:
1. Who calls function 0x1FA0 (where does control come from)?
2. What the 8192-byte MDL holds (audio? descriptors?)
3. Where the virtual address [rsp+0x30] is written after the return
4. Where the MmMapIoSpaceEx call is (if there is one)
5. How the physical T2 buffers (PCI BAR0) relate to this code

---

## CONCLUSION

**All the landmarks provided (RVA 0x17204, 0x1721C, IAT 0xE058, offsets +0x68/+0x70) DO NOT MATCH the binary AppleAudio.sys 6.1.8100.4 with SHA256 cd8b81fabc9be6127a082da22c4cc046a4d46e0225eb6baf6d27e0dc9862a150.**

**Actually found**: one MmMapLockedPagesSpecifyCache call at RVA 0x2051, purpose undetermined; the callers and the use of the result need further tracing.

**The path to the PCM buffer was NOT FOUND** within the landmarks provided.
