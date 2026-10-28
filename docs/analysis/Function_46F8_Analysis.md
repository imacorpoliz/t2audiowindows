# Analysis of function 0x46F8: registering a new device in BufferStruct

> **Status note (2026-10-28):** the 0x98/0x14 offset puzzle below is resolved in `AppleAudio_Playback_Path_Report.md` ("Errors in earlier reports, corrected"): the function's base points 16 bytes before the device, so `+0x90` is `num_input_streams` and `+0x98` is `input_streams[0].buffers[0].address`, matching Linux. Function 0x44E4 is `SET_INPUT_STREAM_ADDRESS_RANGES` (msg 18), see `RE_appleaudio_protocol.md` §2.

## Function signature

```c
NTSTATUS RegisterDeviceToBufferStruct(
    void* global_object,     // rcx (kept in r15)
    void* device_descriptor, // rdx (kept in r14)
    void* param3,            // r8  (kept in [rsp+0x60])
    void* linked_list        // r9  (kept in rdi)
);
```

## Pseudocode

```c
NTSTATUS RegisterDeviceToBufferStruct(void* global, void* device_desc, void* param3, void* list_head)
{
    // 0x46F8-0x04725: prologue, saving registers
    void* gpr_ptr = global->field_0x30;  // 0x4718: mapped BAR2+0xC000
    u8 stream_index = 0;                  // 0x471C: xor sil, sil
    void* linked_list = list_head;        // 0x471F: rdi = r9
    
    // 0x4728: check the GPR structure signature
    if (gpr_ptr[0x04] != 0x19870423) {
        return STATUS_UNSUCCESSFUL;       // 0x4731: 0xC0000001
    }
    
    // 0x473B: version check
    if (gpr_ptr[0x00] < 2) {
        return 0xC0000002;                // 0x488D
    }
    
    // 0x4744: get the BufferStruct base
    u32 bs_offset = gpr_ptr[0x08];
    void* bs_base = global->field_0x28 + bs_offset;  // 0x4749: mapped BAR1 + offset
    
    u8 num_devices = bs_base[0x0C];      // 0x474D
    if (num_devices == 0) {
        goto add_new_device;              // 0x4752 -> 0x4782
    }
    
    // Loop looking the device up by name (0x4754-0x4780)
    for (u32 device_idx = 0; device_idx < num_devices; device_idx++) {
        void* device = bs_base + 0x10 + device_idx * 0xBDEC;
        
        // 0x476F: call _strnicmp (through 0x4CC0)
        int cmp_result = _strnicmp(
            device + 0x00,                // device name
            device_desc + 0x21,           // the name looked for
            0x80                          // maximum comparison length
        );
        
        if (cmp_result == 0) {
            // name match found
            goto found_device;            // 0x4776 -> 0x47EB
        }
    }
    
add_new_device:
    // 0x4782-0x47E9: add a new device
    u8 new_device_idx = num_devices;
    void* new_device = bs_base + 0x10 + new_device_idx * 0xBDEC;
    
    // copy the device name (128 bytes with SSE)
    memcpy_sse(new_device + 0x00, device_desc + 0x21, 128);  // 0x4782-0x47E1
    
    void* target_device = new_device;
    goto register_streams;                // 0x47E9 -> 0x4861
    
found_device:
    // 0x47EB: device found
    u32 found_device_idx = device_idx;
    void* target_device = bs_base + 0x10 + found_device_idx * 0xBDEC;
    
register_streams:
    // 0x47F6-0x4836: register the input streams
    stream_index = 0;
    while (linked_list != NULL) {
        u32 array_idx = stream_index * 6;  // 0x47FC: lea rcx, [rax + rax*2]; shl
        
        // 0x480A, 0x4816: write into the buffers[] array of input_streams[0]
        target_device->input_streams[0].buffers[stream_index].address = linked_list->field_0x08;
        target_device->input_streams[0].buffers[stream_index].size = linked_list->field_0x10;
        
        stream_index++;
        linked_list = linked_list->next;  // 0x481E: mov rdi, [rdi]
    }
    
    // 0x4826: store the buffer count
    target_device->input_streams[0].num_buffers = stream_index;
    
    // 0x482D: set the stream active flag
    target_device->input_streams[0].active_flag = 1;  // offset 0x90 in the stream
    
    // Alternative branch for output streams (0x4838-0x4875)
    // [symmetric code, but writes to another offset - needs checking via r8]
    
    // 0x4875: increment the device counter
    bs_base[0x0C]++;
    
    // 0x4878-0x488B: call the notification function
    func_0x44E4(global, device_desc, param3, NULL);
    
    return STATUS_SUCCESS;
}
```

## Mapping to Linux audio.h

### BufferStruct fields (confirmed)

| Windows RVA | Offset | Linux field | Value |
|-------------|----------|------------|----------|
| 0x4728 | +0x04 | signature | 0x19870423 (T2AUDIO_SIG) |
| 0x473B | +0x00 | version | >= 2 (Linux requires >= 3) |
| 0x4744 | +0x08 | bs_offset | offset of BufferStruct in BAR1 |
| 0x474D | +0x0C | num_devices | number of devices |

### Device fields (confirmed)

| Windows offset | Linux offsetof | Field |
|----------------|----------------|------|
| +0x00 | +0x00 | name[128] |
| +0x80 | +0x80 | num_input_streams |
| +0x81 | +0x81 | num_output_streams |
| +0x84 | +0x84 | input_streams[5] |
| +0x5EF8 | +0x5EF8 | output_streams[5] |

**Stride**: 0xBDEC (48620 bytes), an exact match

### Stream fields

Offsets 0x98 and 0xA0 are written through the index `rcx*8`:
- `rcx = (stream_index * 3) * 2` (0x47FC, 0x4804)
- An array of 6 QWORDs per buffer: `address, size, pad[4]`

Computing the offset of buffers[] in the stream:
```
base_offset = 0x98 - (stream_index * 6 * 8)
With stream_index=0: base_offset = 0x98
```

**Problem**: Linux offsetof(stream, buffers) = **0x04**, but Windows writes to **0x98**.

### Analysis of the mismatch

Offset 0x98 from the start of the device:
```
0x98 = 0x84 + 0x14
       ^^^   ^^^
       |     offset inside the stream
       offsetof(device, input_streams)
```

Expected: `0x84 + 0x04 = 0x88` (if buffers start at +4 in the stream)

**0x98 = 0x84 + 0x14** means offset +0x14 inside the stream.

### Recomputing the stream structure (Windows layout)

Linux:
```c
struct stream {
    u8 num_buffers;              // +0x00
    struct buffer buffers[100];  // +0x04 (after padding to 4-byte alignment)
    char filler[32];
};
```

A possible Windows layout:
```c
struct stream {
    u8 num_buffers;              // +0x00
    u8 pad1[3];                  // +0x01
    u32 flags_or_reserved;       // +0x04
    u32 additional_field1;       // +0x08
    u32 additional_field2;       // +0x0C
    u8 active_flag;              // +0x10 (written at 0x482D as [rdx+0x90])
    u8 pad2[3];
    struct buffer buffers[100];  // +0x14
    char filler[32];
};
```

**Confirmation**: 
- 0x480A writes to `[rdx + rcx*8 + 0x98]`
- rdx = device base
- 0x98 = 0x84 (input_streams) + 0x14 (buffers in the stream)

### Linked list node fields (R9)

```c
struct buffer_node {
    struct buffer_node* next;  // +0x00 (read at 0x481E)
    size_t address;            // +0x08 (read at 0x4800, 0x480A)
    size_t size;               // +0x10 (read at 0x4812, 0x4816)
};
```

## Key conclusions

1. **Function 0x46F8** registers input streams for a device in BufferStruct
2. **Lookup by name** with _strnicmp (128-byte length)
3. **If the device is found**: updates buffers[] of the existing device
4. **If not found**: appends a new device to the array
5. **The linked list (R9)** holds buffer descriptors (address, size, next)
6. **Write offsets**: 0x98, 0xA0 = buffers[i].address, buffers[i].size

## Unknown

1. **Origin of the linked list**: where does parameter R9 come from? (The physical-range list built by `FUN_140001fa4` for input buffers.)
2. **Output streams**: is there symmetric code for output_streams (offset 0x5EF8)? (Output uses the T2-provided ring; `0x9D74` only reads it.)
3. **Function 0x44E4**: what does the call after registration do? (`SET_INPUT_STREAM_ADDRESS_RANGES`, msg 18.)
4. **Offset 0x90**: a stream active flag or another field? (`num_input_streams`.)
