# Static Analysis of AppleAudio.sys Microphone Capture (P2-RE)

> **Date:** 2026-10-29  
> **Target:** `C:\Windows\System32\drivers\AppleAudio.sys` (SHA256: `b4c6f52d77066b4aa33baaea697bc9c16cadb1e4b1586cd0c11212a58708483d`, PE x86_64)  
> **Tool:** REA / Ghidra 12.1.4  
> **Purpose:** Resolve all questions from `GEMINI.md` (§ "P2-RE") following the microphone power-off incident.

---

## 1. Message 18 (`SET_INPUT_STREAM_ADDRESS_RANGES`, `0x140004344`)

### Assembly & Structure
Function `0x140004344` (`FUN_140004344`) builds and sends BCE Message 18. The command buffer is **always 837 bytes (`0x345`)**:

```asm
0x140004376: MOV [RSP + 0x30], EAX          ; Tag (context counter)
0x140004390: MOV byte ptr [RSP + 0x34], 1    ; Type = 1 (COMMAND)
0x140004380: MOV RAX, [RDX]                 ; DeviceId (0x21 "Digital Mic")
0x140004383: MOV [RSP + 0x35], RAX
0x140004395: MOV dword ptr [RSP + 0x3d], 0x12 ; Message = 18 (0x12)
0x14000436b: AND dword ptr [RSP + 0x41], 0  ; Status = 0
0x140004388: MOV RAX, [R8]                  ; StreamId (0x22)
0x14000438b: MOV [RSP + 0x45], RAX
0x14000439d: TEST R9, R9                    ; R9 = buffer_node list pointer
0x1400043a0: JNZ 0x1400043f5
; Path A: R9 == NULL
0x1400043a2: MOV qword ptr [RSP + 0x4d], 1  ; count = 1
0x1400043a7: MOV qword ptr [RSP + 0x55], 1  ; range[0].address = 1
0x1400043ac: MOV qword ptr [RSP + 0x5d], 1  ; range[0].size = 1
0x1400043b1: LEA R9, [RBP + 0x280]          ; Reply buffer
0x1400043b8: MOV dword ptr [RSP + 0x20], 0x1d ; Reply size = 29 (0x1D)
0x1400043c0: MOV R8D, 0x345                 ; Command buffer length = 837 bytes (0x345)
0x1400043c6: LEA RDX, [RSP + 0x30]          ; Command buffer pointer
0x1400043cb: MOV RCX, R10
0x1400043ce: CALL 0x1400048ac              ; SendBceMessage (IOCTL 0x222018)
...
; Path B: R9 != NULL (iterates linked list of range nodes)
0x1400043f5: ADD RCX, 1                     ; RCX = range count
0x1400043f8: MOV [RSP + 0x4d], RCX
0x1400043fd: CMP RCX, 0x32                  ; max 49 (0x31) ranges
0x140004401: JNC 0x14000442f
0x140004403: MOV RAX, [R9 + 8]              ; RAX = node->physical_address
0x140004407: ADD RCX, RCX                   ; RCX * 2
0x14000440a: MOV [RSP + RCX*8 + 0x45], RAX  ; range[i].address
0x14000440f: MOV RCX, [RSP + 0x4d]
0x140004414: MOV RAX, [R9 + 0x10]           ; RAX = node->size
0x140004418: ADD RCX, RCX
0x14000441b: MOV [RSP + RCX*8 + 0x4d], RAX  ; range[i].size
0x140004420: MOV R9, [R9]                   ; R9 = node->next
0x140004423: TEST R9, R9
0x140004426: JZ 0x1400043b1
0x140004428: MOV RCX, [RSP + 0x4d]
0x14000442d: JMP 0x1400043f5
```

### Callers and the Immediate `count=1, 1, 1` Path
- Caller `0x140004558` (the only direct caller of `0x140004344`):
  At `0x1400046dd`, it explicitly executes:
  ```asm
  0x1400046dd: XOR R9D, R9D
  0x1400046e0: MOV RDX, R14
  0x1400046e3: MOV RCX, R15
  0x1400046e6: CALL 0x140004344
  ```
  **`R9` is ALWAYS NULL when called from `0x140004558`.**
  Therefore, Path A (`count=1, address=1, size=1`) is **always taken** in AppleAudio.
  The reason: the actual physical buffer descriptors are already written into `BufferStruct` by the preceding loop in `0x140004558`. The `1, 1, 1` payload signals bridgeOS to read the ranges from `BufferStruct` for StreamId `0x22`.

- **Difference from `0x1400044E4`:**
  `0x1400044E4` in earlier documentation was an erroneous reference to `0x14000445c` (`SET_PROPERTY 'nsrt'`). `0x140004344` is the true and only function sending message 18.

---

## 2. Input Buffer Allocation (`0x140001ea4` / `0x140001FA4`)

### Memory Allocation Mechanism
AppleAudio uses `MmAllocatePagesForMdl`:
```asm
0x140001ecb: MOV R9D, [RSP + 0x20]          ; aligned buffer size
0x140001ed0: XOR R8D, R8D                   ; SkipBytes = 0
0x140001ed3: MOV RDX, 0x1fffffffff          ; HighAddress = 128 GB
0x140001edd: MOV ECX, 0x2000                ; LowAddress = 8 KB
0x140001ee2: CALL MmAllocatePagesForMdl
...
0x140001ef7: CALL MmMapLockedPagesSpecifyCache (KernelMode, MmCached)
```
- AppleAudio does **not** use `AllocateCommonBuffer` or `MmAllocateContiguousMemory`.
- It allocates potentially non-contiguous physical pages via `MmAllocatePagesForMdl` (address range `0x2000` to `0x1FFFFFFFFF`).
- It then scans `mdl->pages[i]` (PFN array):
  `phys_addr = pfn << 12;`
  It coalesces contiguous physical pages:
  `if (prev->address + prev->size == phys_addr) prev->size += 0x1000;`
  `else start_new_node();`
- That is why BufferStruct on hardware held 3 buffers (`0x3000`, `0x20000`, `0xDC00`): the OS granted 3 contiguous extents totaling the requested ring size.
- **Which addresses go into BufferStruct:** Direct host physical addresses (`pfn << 12`), identical to `MmGetPhysicalAddress`.

---

## 3. `SET_PROPERTY 'nsrt'` (Message 9, `0x14000445c`)

### Packet Layout (66 bytes = `0x42`)
Sent by `FUN_14000445c` via helper `FUN_14000451c`:
```c
struct T2AUDIO_SET_PROPERTY_NSRT {
    T2AUDIO_MESSAGE_HEADER Header; // Tag, Type=1, DeviceId=0x21 (13 bytes)
    T2AUDIO_MESSAGE_BASE Base;     // Message=9, Status=0 (8 bytes)
    // Payload starts at offset 21:
    ULONG64 Object;                // 0x15: DeviceId (0x21)
    ULONG Element;                 // 0x1D: 0
    ULONG Scope;                   // 0x21: 'glob' (0x676C6F62)
    ULONG Selector;                // 0x25: 'nsrt' (0x6E737274)
    ULONG64 DataSize;              // 0x29: 8 bytes
    double NominalSampleRate;      // 0x31: (double)48000.0 (0x40E7700000000000)
    ULONG64 Reserved;              // 0x39: 0
};
```
- Total length: **66 bytes (`0x42`)**.
- Expected reply: Message 10 (`SET_PROPERTY_RESPONSE`), Status 0, length 29 (`0x1D`).
- **Timing:** Sent during stream creation (`FUN_1400193f0`), **before** `START_IO`.

---

## 4. Complete Ordered Sequence of BCE Messages

### Stream Creation / Open:
1. `SET_INPUT_STREAM_ADDRESS_RANGES` (msg 18, 837 bytes) on device `0x21`, stream `0x22`, count 1, range `1, 1`.
2. `SET_PROPERTY 'nsrt'` (msg 9, 66 bytes) on device `0x21`, scope `'glob'`, value `48000.0`.

### Transition to RUN (`SetState(KSSTATE_RUN)` in `0x140009570`):
3. `START_IO` (msg 0, 21 bytes) on device `0x21` (`"Digital Mic"`).
   - Reply checked: Message 1 (`START_IO_RESPONSE`), Status 0.
   - **No other BCE commands are sent on RUN.**

### Transition to STOP (`SetState(KSSTATE_STOP)` in `0x140009570`):
4. `STOP_IO` (msg 2, 21 bytes) on device `0x21`.
   - Reply checked: Message 3 (`STOP_IO_RESPONSE`), Status 0.

### Wake from Sleep (D0 in `0x1400041f8`):
5. Re-writes BufferStruct entries for each input stream.
6. Re-sends `SET_INPUT_STREAM_ADDRESS_RANGES` (msg 18, 837 bytes).

---

## 5. BufferStruct Fields Written by AppleAudio

In `FUN_140004558`:
- `bs_base + device_idx * 0xBDEC` is 16 bytes before the `device` struct.
- Offset relative to device:
  - `device + 0x00`: `Name` (copied if new device)
  - `device + 0x80`: `NumInputStreams = 1`
  - `device + 0x84`: `InputStreams[0].NumBuffers = buffer_count`
  - `device + 0x88`: `InputStreams[0].Buffers[0].Address`
  - `device + 0x90`: `InputStreams[0].Buffers[0].Size`
  - Subsequent buffers at `+0x30` stride (`0x88 + i*0x30`, `0x90 + i*0x30`).
- No other fields (e.g. `active` flags at `+0x0C` or `+0x10`) exist in the Windows layout.
- The `T2AudioMiniport.h` struct layout in `T2AudioPort` matches AppleAudio byte for byte.
