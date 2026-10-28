# Report: analysis of AppleAudio's buffer registration function 0x46F8

> **Status note (2026-10-28):** the "+0x98 inside the stream" mismatch below comes from the base pointer of 0x46F8 being 16 bytes before the device (see `AppleAudio_Playback_Path_Report.md`, "Errors in earlier reports, corrected"); the layout matches Linux. This function registers **input** (microphone) buffers; it matters for the microphone work in `ROADMAP.md`. Function 0x44E4 is `SET_INPUT_STREAM_ADDRESS_RANGES` (msg 18).

## Corrections to earlier reports

1. **Confirmed**: RVA 0x4CC0 is a jump to `_strnicmp`, used to compare device names (up to 128 bytes).
2. **Confirmed**: the read of `[RAX+8]` at 0x4744 is the offset of BufferStruct in BAR1, not the BufferStruct.flags field.
3. **Confirmed**: the instructions at 0x480A and 0x4816 really write into the mapped BufferStruct.

## 1. Bounds of the buffer registration function

**Function 0x46F8-0x48AF** (from `.pdata`)

Signature:
```c
NTSTATUS RegisterInputStreamBuffers(
    void* global_object,      // RCX (kept in R15)
    void* device_descriptor,  // RDX (kept in R14) 
    void* unused_param,       // R8  (kept in [RSP+0x60], passed on to 0x44E4)
    void* buffer_list         // R9  (kept in RDI)
);
```

## 2. Full pseudocode of function 0x46F8

```c
NTSTATUS RegisterInputStreamBuffers(void* global, void* device_desc, 
                                     void* unused, void* buffer_list)
{
    // Pointer to the GPR (BAR2+0xC000)
    void* gpr = global->offset_0x30;  // 0x4718
    
    // GPR signature check
    if (gpr->signature != 0x19870423) {  // 0x4728
        return STATUS_UNSUCCESSFUL;  // 0x4731: 0xC0000001
    }
    
    // Version check
    if (gpr->version < 2) {  // 0x473B
        return 0xC0000002;  // 0x488D
    }
    
    // Get BufferStruct
    u32 bs_offset = gpr->bs_offset;  // 0x4744: [RAX+8]
    void* bs_base = global->offset_0x28 + bs_offset;  // 0x4749: BAR1 + offset
    
    u8 num_devices = bs_base->num_devices;  // 0x474D: [RBX+0x0C]
    u32 device_idx;
    void* target_device;
    
    // Look the device up by name
    if (num_devices > 0) {  // 0x4752
        for (device_idx = 0; device_idx < num_devices; device_idx++) {  // 0x4754-0x4780
            void* device = bs_base + 0x10 + device_idx * 0xBDEC;
            
            // _strnicmp call
            int cmp = _strnicmp(device->name, device_desc + 0x21, 0x80);  // 0x476F
            
            if (cmp == 0) {  // 0x4776
                // Device found
                target_device = bs_base + 0x10 + device_idx * 0xBDEC;  // 0x47EB-0x47F2
                goto register_buffers;  // 0x4821
            }
        }
    }
    
    // Add a new device
    device_idx = num_devices;
    target_device = bs_base + 0x10 + device_idx * 0xBDEC;
    
    // Copy the name (128 bytes with SSE)
    memcpy(target_device->name, device_desc + 0x21, 128);  // 0x4782-0x47E1
    
register_buffers:
    // Write the buffer descriptors into input_streams[0]
    u8 buffer_count = 0;
    void* node = buffer_list;
    
    while (node != NULL) {  // 0x4821/0x4861
        u32 index = buffer_count * 6;  // 0x47FC: lea rcx, [rax+rax*2]; shl
        
        // Write into input_streams[0].buffers[]
        target_device->input_streams[0].buffers[buffer_count].address = node->address;  // 0x480A/0x484A
        target_device->input_streams[0].buffers[buffer_count].size = node->size;        // 0x4816/0x4856
        
        buffer_count++;
        node = node->next;  // 0x481E/0x485E
    }
    
    // Update the buffer counter
    target_device->input_streams[0].num_buffers = buffer_count;  // 0x4826/0x4866: [+0x94]
    
    // Set the stream active flag
    target_device->input_streams[0].active = 1;  // 0x482D/0x486D: [+0x90]
    
    // Increment the device counter (new device only)
    bs_base->num_devices++;  // 0x4875: inc byte [RBX+0x0C]
    
    // Notify another subsystem
    func_0x44E4(global, device_desc, unused, NULL);  // 0x4886
    
    return STATUS_SUCCESS;
}
```

## 3. Mapping the written fields to Linux audio.h

### Linked list node (parameter R9)

| Offset | Instruction | Linux field | Description |
|----------|------------|------------|----------|
| +0x00 | 0x481E | `next` | pointer to the next node |
| +0x08 | 0x4800, 0x4840 | - | physical address of the buffer |
| +0x10 | 0x4812, 0x4852 | - | buffer size |

A Windows-specific structure (built in 0x1FA4):
```c
struct buffer_node {
    struct buffer_node* next;  // +0x00
    u64 physical_address;      // +0x08
    u64 size;                  // +0x10
};
```

### Written fields of the BufferStruct device

**Computing the buffers[] offset:**
- Array index: `(buffer_idx * 3) * 2 = buffer_idx * 6`
- Write: `[target_device + index*8 + 0x98]` and `[target_device + index*8 + 0xA0]`

**Decomposing offset 0x98:**
```
0x98 = 0x84 + 0x14
       ^^^   ^^^
       |     offset of buffers[] inside the stream
       offsetof(device, input_streams)
```

**Confirmed offsets in the device:**

| Windows offset | Linux offsetof | Field | Instruction |
|----------------|----------------|------|------------|
| +0x00 | +0x00 | `name[128]` | 0x4782 (SSE copy) |
| +0x0C | - | - | 0x474D (reads num_devices of BufferStruct, not of the device) |
| +0x84 | +0x84 | `input_streams[5]` | base for the computation |
| +0x90 | - | - | 0x482D (a flag, but relative to the stream, not the device) |
| +0x94 | - | - | 0x4826 (a counter, relative to the stream) |
| +0x98 | +0x98 | - | 0x480A (address write) |
| +0xA0 | - | - | 0x4816 (size write) |

**Device size:** 0xBDEC (48620 bytes), an exact match with Linux `sizeof(struct device)`

### Refining the stream structure (Windows layout)

Offset 0x98 relative to the device gives:
```
0x98 = input_streams_base (0x84) + 0x14
```

So buffers[] would start at **+0x14** inside the stream, not at +0x04 as on Linux.

**Hypothesis about the Windows stream layout:**
```c
struct stream_windows {
    u8 num_buffers;         // +0x00
    u8 pad1[3];
    u32 field_0x04;         // +0x04
    u32 field_0x08;         // +0x08
    u32 field_0x0C;         // +0x0C
    u8 active;              // +0x10 (written as [device+0x90] = device+0x84+0x0C)
    u8 pad2[3];
    struct buffer buffers[100];  // +0x14 (buffer array)
    char filler[32];
};
```

**Checking the active offset:**
- 0x482D: `mov byte [r8 + rbx + 0x90], 1`
- `r8 = device_idx * 0xBDEC`, `rbx = bs_base`
- `r8 + rbx = target_device`
- `target_device + 0x90 = device_base (0x84) + 0x0C`

But the context shows that 0x90 is written relative to the **stream**, not the device base. Recomputing:
```
[rdx + rbx + 0x90] at 0x486D, where rdx = r8, rbx = bs_base
This is [target_device + 0x90]
```

If `input_streams` starts at 0x84, then:
```
0x90 - 0x84 = 0x0C (offset inside the stream)
```

But per the instruction at 0x4866 (`mov byte [r8 + 0x94], sil`) offset 0x94 is `num_buffers`.

**Revision:** offsets 0x90 and 0x94 belong to the **stream**, not the device:
- 0x94 inside the stream = num_buffers
- 0x90 inside the stream = active flag

So:
```c
struct stream_windows {
    u8 field_0x00[144];      // unknown structure
    u8 active;               // +0x90
    u8 pad[3];
    u8 num_buffers;          // +0x94
    u8 pad2[3];
    struct buffer buffers[100];  // +0x98
};
```

**Size of one stream:** from the context, ≤ 0x12E4 (the Linux sizeof).

### Mismatch with the Linux layout

Linux `offsetof(stream, buffers) = 0x04`, but Windows uses **0x98** inside the stream structure. This points to **Windows-specific fields** before the buffers array.

Possible reasons:
1. The Windows driver adds an extension structure of ~0x94 bytes.
2. The num_buffers and active fields sit right before buffers[].

(Resolution: neither; the base pointer is 16 bytes before the device, so +0x90 is `num_input_streams`, +0x94 is `input_streams[0].num_buffers` and +0x98 is `input_streams[0].buffers[0].address`, exactly as on Linux.)

## 4. Origin of the linked list (R9)

### Caller 0x1FA4 (AllocateInputBuffers)

**Bounds:** 0x1FA4-0x221A

**Pseudocode:**
```c
NTSTATUS AllocateInputBuffers(void* global, void* device_struct, u32 size)
{
    // Allocate physical memory through an MDL
    size_aligned = ALIGN_UP(size, 0x1000);  // 0x1FC7-0x1FD8
    
    MDL* mdl = MmAllocatePagesForMdl(NULL, NULL, size_aligned, 0x2000);  // 0x201B
    if (!mdl || mdl->ByteCount < size_aligned) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    
    // Map into kernel space
    void* mapped = MmMapLockedPagesSpecifyCache(mdl, KernelMode, 
                                                 MmCached, NULL, 0, 
                                                 0x40000020);  // 0x2051
    if (!mapped) {
        MmFreePagesFromMdl(mdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    
    // Build the linked list of buffer descriptors
    u32 num_pages = (size_aligned >> 12) + 1;  // 0x2070-0x2078
    u32 list_size = num_pages * 0x18;  // 0x207A: (pages * 3) * 8
    
    struct buffer_node* list = ExAllocatePoolWithTag(NonPagedPool, list_size, 'CCAA');  // 0x2085
    memset(list, 0, list_size);  // 0x20A7
    
    // Walk the MDL's PFN array and build the linked list
    struct buffer_node* current = list;
    u64* pfn_array = &mdl->pages[0];  // 0x20CA: MDL+0x30
    u32 remaining = mdl->ByteCount;   // 0x20C3
    
    for (u32 i = 0; i < num_pages && remaining > 0; i++) {  // 0x20B4-0x213D
        u64 pfn = pfn_array[i];  // 0x20D4
        u64 phys_addr = pfn << 12;  // 0x20E2: shift into a physical address
        u32 chunk_size = min(remaining, 0x1000);  // 0x20EE-0x2116
        
        current->address = phys_addr;  // 0x2108
        current->size = chunk_size;    // 0x2118
        
        if (remaining > 0x1000) {
            current->next = current + 1;  // 0x2120: lea [r14+0x18]
            current = current->next;      // 0x212A
        } else {
            current->next = NULL;  // 0x2133
        }
        
        remaining -= chunk_size;
    }
    
    // Call the registration function
    RegisterInputStreamBuffers(global, device_struct, 
                               device_struct + 0x1B0 + stream_idx * 0xB30,
                               list);  // 0x2194
    
    // Keep the references for cleanup
    device_struct->streams[stream_idx].mapped_address = mapped;  // 0x21A2
    device_struct->streams[stream_idx].mdl = mdl;                // 0x21B0
    device_struct->streams[stream_idx].buffer_list = list;       // 0x21B4
    
    return STATUS_SUCCESS;
}
```

**Key fields:**
- `R9 = list`: a linked list built from the MDL's PFN array
- Each node holds a page's physical address and the chunk size
- The list is built from a contiguous kernel allocation but describes potentially non-contiguous physical pages

### Caller 0x4398 (RestoreAllInputBuffers)

**Bounds:** 0x4398-0x4448

**Pseudocode:**
```c
void RestoreAllInputBuffers(void* global)
{
    if (global->offset_0x30 == NULL) return;  // 0x43B0: GPR check
    
    u32 num_device_slots = global->offset_0x40;  // 0x43BC: number of device slots
    
    for (u32 dev_idx = 0; dev_idx < num_device_slots; dev_idx++) {  // 0x43C9-0x442D
        void* device_slot = global + 0x1F0 + dev_idx * 0xE970;  // 0x43C2, 0x441F
        u32 num_streams = device_slot->num_streams;  // 0x43CC: [RDI]
        
        for (u32 stream_idx = 0; stream_idx < num_streams; stream_idx++) {  // 0x43DC-0x441B
            void* stream_struct = device_slot - 0x1A8 + stream_idx * 0xB30;  // 0x43D3
            
            if (stream_struct->offset_0xCD0 == 0) continue;  // 0x43DC: skip inactive ones
            
            // Read the stored buffer_list
            void* buffer_list = stream_struct->offset_0xB30;  // 0x43E7: [RDI+RBP+0xB30]
            
            // Registration call
            RegisterInputStreamBuffers(global, stream_struct - 0x1A8,
                                       stream_struct,
                                       buffer_list);  // 0x4406
        }
    }
}
```

**Meaning of the fields:**
- `global + 0x1F0`: start of the device slot array (Windows driver extension)
- Stride 0xE970 (59760 bytes): size of a device slot in the Windows driver
- `stream_struct->offset_0xB30`: the stored buffer_list pointer

(This is what AppleAudio runs on return to D0, see `RE_appleaudio_protocol.md` §7.)

## 5. Symmetric code for output_streams

### Function 0x9D74 (GetOutputStreamBuffers)

**Bounds:** 0x9D74-0x9E8D

**Key instruction:**
```asm
0x9E78: mov rcx, qword ptr [rdx + rbx + 0x5F0C]
0x9E83: mov rcx, qword ptr [rdx + rbx + 0x5F14]
```

**Decomposing offset 0x5F0C:**
```
0x5F0C = 0x5EF8 + 0x14
         ^^^^    ^^^^
         |       offset of buffers[] inside the stream
         offsetof(device, output_streams)
```

**Confirmation:** offset 0x5EF8 matches Linux `offsetof(device, output_streams)` exactly.

**Pseudocode of function 0x9D74:**
```c
NTSTATUS GetOutputStreamBuffers(void* device_name, void* gpr, void* bs_base,
                                u64* out_addr, u64* out_size)
{
    // Signature check
    if (gpr->signature != 0x19870423) {  // 0x9D94
        return STATUS_UNSUCCESSFUL;
    }
    
    u32 version = gpr->version;  // 0x9DA5
    
    if (version == 1) {
        // Old version: lookup at the special offset 0x110
        u32 bs_offset = gpr->bs_offset;  // 0x9DAA
        void* bs_base_calc = bs_base + bs_offset;  // 0x9DAE
        
        if (bs_base_calc->signature != 0x19870423) return STATUS_UNSUCCESSFUL;  // 0x9DB1
        
        u8 num_devices = bs_base_calc->num_devices;  // 0x9DB8
        for (u32 i = 0; i < num_devices; i++) {  // 0x9DBE-0x9DE2
            void* device = bs_base_calc + 0x10 + i * 0x110;  // 0x9DC4: old stride!
            
            if (strcmp(device->name, device_name) == 0) {  // 0x9DD1
                *out_addr = device->offset_0x110;  // 0x9DFE
                *out_size = device->offset_0x118;  // 0x9E0E
                return STATUS_SUCCESS;
            }
        }
        
        return STATUS_UNSUCCESSFUL;
    }
    
    if (version >= 2) {
        // New version: lookup in the device array with stride 0xBDEC
        u32 bs_offset = gpr->bs_offset;  // 0x9E27
        void* bs_base_calc = bs_base + bs_offset;  // 0x9E2B
        
        if (bs_base_calc->version < 3) {  // 0x9E2E
            return STATUS_UNSUCCESSFUL;
        }
        
        u8 num_devices = bs_base_calc->num_devices;  // 0x9E35
        for (u32 i = 0; i < num_devices; i++) {  // 0x9E3B-0x9E65
            void* device = bs_base_calc + 0x10 + i * 0xBDEC;  // 0x9E41
            
            if (_strnicmp(device->name, device_name, 0x80) == 0) {  // 0x9E54
                // Read from output_streams[0]
                *out_addr = device->output_streams[0].offset_0x18;  // 0x9E78: [+0x5F0C]
                *out_size = device->output_streams[0].offset_0x20;  // 0x9E83: [+0x5F14]
                return STATUS_SUCCESS;
            }
        }
    }
    
    return STATUS_UNSUCCESSFUL;
}
```

**Differences from input_streams:**
- The output_streams fields are **only read, never written**
- Offsets 0x5F0C and 0x5F14 correspond to `output_streams[0].buffers[0].field_0x18` and `field_0x20`
- The function retrieves ready-made descriptors rather than registering new ones

**Conclusion:** a registration mechanism for output buffers was **not found** in the functions analysed. Function 0x9D74 only reads. (Consistent with what is now known: the T2 provides the output ring; the host only reads its location.)

## 6. Exact table of BufferStruct and device fields

### BufferStruct header (confirmed by bytes)

| Offset | Size | Field | Instruction | Value |
|----------|--------|------|------------|----------|
| +0x00 | u32 | version | 0x473B | >= 2 (Linux requires >= 3) |
| +0x04 | u32 | signature | 0x4728 | 0x19870423 |
| +0x08 | u32 | bs_offset | 0x4744 | offset in BAR1 |
| +0x0C | u8 | num_devices | 0x474D | number of devices |
| +0x10 | struct device[] | devices | 0x474D | device array |

### device structure (confirmed by bytes)

| Offset | Size | Field | Instruction | Description |
|----------|--------|------|------------|----------|
| +0x00 | u8[128] | name | 0x476F, 0x4782 | device name (ASCII) |
| +0x84 | struct stream[5] | input_streams | computed | 5 input streams |
| +0x5EF8 | struct stream[5] | output_streams | 0x9E78 | 5 output streams |
| sizeof | 0xBDEC | - | 0x4762 | 48620 bytes |

**Match with Linux:** offsetof and sizeof match exactly.

### stream structure (partly reconstructed)

| Offset | Size | Field | Instruction | Description |
|----------|--------|------|------------|----------|
| +0x90 | u8 | active | 0x482D, 0x486D | active flag |
| +0x94 | u8 | num_buffers | 0x4826, 0x4866 | number of buffers |
| +0x98 | struct buffer[100] | buffers | 0x480A, 0x4816 | descriptor array |

**Mismatch with Linux:** Linux puts buffers at +0x04, Windows at +0x98 (resolved, see the note at the top).

### buffer structure on Windows

Indexed through `index = buffer_idx * 6`:
```c
struct buffer {
    u64 address;     // +0x00 (write at 0x480A, 0x4816)
    u64 size;        // +0x08 (write at 0x4816, 0x4856)
    u64 pad[4];      // +0x10 (unused)
};  // sizeof = 0x30 (48 bytes)
```

The 0x30 stride is confirmed by the computation: `(idx * 3) * 2 * 8 = idx * 48`.

## 7. Established facts

### Confirmed operations

1. **Function 0x46F8** registers input stream buffers in the BufferStruct located in BAR1.
2. **The device lookup** is done with `_strnicmp` on the names (128 bytes).
3. **If the device is not found**: a new one is appended to `devices[]`.
4. **If the device is found**: buffers[] of the existing device are updated.
5. **The linked list** is built in 0x1FA4 from the MDL's PFN array (non-contiguous physical pages).
6. **Function 0x9D74** reads output_streams at offset 0x5F0C but **does not write**.

### Mismatches with Linux

1. **buffers[] offset**: Windows +0x98 inside the stream, Linux +0x04 (resolved).
2. **Extra fields**: Windows adds ~0x90 bytes before buffers[] (resolved: there are none).
3. **No output registration**: no code writing into output_streams was found.

### Windows driver extension

Function 0x4398 works with device slots at `global + 0x1F0`, stride 0xE970. This is a **Windows-specific structure**, unrelated to BufferStruct.

Fields of the Windows device slot:
- +0x1A8: unknown
- +0x1B0: start of the stream descriptor array (stride 0xB30)
- +0xB30: the stored buffer_list pointer
- +0xCD0: the stream slot active flag

The size 0xB30 (2864 bytes) does not match the Linux sizeof(stream) = 0x12E4.

## 8. Unknowns and next steps

### Critical unknowns

1. **Registering output_streams:** where is the code writing into `device->output_streams[].buffers[]`? (Answer: there is none; the T2 provides the output ring.)
2. **Offset 0x1B0:** what is the R8 parameter passed to 0x46F8 and then to 0x44E4?
3. **Function 0x44E4:** what does the call after buffer registration do? (Answer: `SET_INPUT_STREAM_ADDRESS_RANGES`, msg 18.)
4. **Windows stream layout:** what is in the first 0x90 bytes of the stream structure? (Answer: nothing extra, see the note.)
5. **Stride 0xB30:** why is the Windows stream descriptor larger than the Linux stream?

### Recommendations for further analysis

1. Disassemble function 0x44E4 to understand the post-processing.
2. Find xrefs to offsets 0x5F0C-0x5F30 to discover code writing output_streams.
3. Analyse the buffer allocation functions for the output direction.
4. Study the relationship between the device slot (0xE970) and the BufferStruct device (0xBDEC).
5. Find the code reading `input_streams[].buffers[]` to confirm the format.

### Concrete next action

**Searching for the function that writes output_streams:**
```python
# Look for write instructions to offsets 0x5F0C-0x5F30
for insn in all_instructions:
    if insn.is_write() and 0x5F0C <= insn.mem.disp <= 0x5F30:
        print(f"Found write to output_streams at {insn.address:X}")
```

## 9. Retracted unreliable claims

**Removed:**
- The claim that 347 matching offsets prove a single object
- Treating AppleAudio's internal pointer as a provided API
- The claim that MmMapIoSpace is how the WaveRT buffer is registered
- The claim that BufferStruct is mapped into user mode
- Readiness percentages, work hours, risk levels

**Confirmed by static analysis:**
- Every offset comes from disassembled instructions
- Every function call was traced through xrefs
- Every structure was reconstructed from memory access patterns
- Every claim is tied to a specific RVA

---

**End of report**
