# AppleAudio.sys T2 Protocol Analysis

## Critical Finding: AppleAudio.sys calls IOCTL 0x222018 at RVA 0x4A60

### Function at RVA 0x4A60 - T2 Message Send Function

**Disassembly (annotated)**:
```
0x4A60: lea rbp, [rax-0x57]        ; stack frame setup
0x4A65: sub rsp, 0xA0               ; allocate 160 bytes stack
0x4A6C: mov qword [rbp+0x7], 0     ; zero local variable
0x4A71: lea rcx, [rbp+0x0F]        ; rcx = device name buffer
0x4A75: mov qword [rbp-1], 0       ; zero another local
0x4A7A: mov rdi, rdx                ; rdi = 2nd parameter (message buffer?)
0x4A7D: lea rdx, [rip+0x8C2D]      ; rdx = L"\DosDevices\AppleUSBVHCI"
        ; --> points to RVA 0xD6B0 = Unicode string
0x4A84: mov rsi, r9                 ; rsi = 4th parameter
0x4A87: mov r14d, r8d               ; r14d = 3rd parameter (message length?)
0x4A8B: call 0x4448                 ; call helper (IoGetDeviceObjectPointer?)

; Error check
0x4A90: mov ebx, eax
0x4A92: test eax, eax
0x4A94: js 0x4B71                   ; jump if error

; Call CreateFile or ZwCreateFile
0x4A9A: lea r9, [rbp-1]             ; output handle pointer
0x4A9E: mov edx, 0x80               ; GENERIC_READ
0x4AA4: lea r8, [rbp+7]             ; object attributes?
0x4AA9: lea rcx, [rbp+0x0F]         ; device name
0x4AAD: call [rip+0x9681]           ; IAT call (ZwCreateFile or similar)
        ; --> IAT@0xE130

0x4AB3: mov ebx, eax
0x4AB5: test eax, eax
0x4AB7: js 0x4B71                   ; error check

; Calculate/adjust buffer size
0x4AB9: mov ecx, [rdi]              ; ecx = dword from message buffer
0x4ABC: mov eax, 0x02040811         ; magic constant for division
0x4AC1: mul ecx                     ; multiply
0x4AC3: mov eax, ecx
0x4AC5: xor r8d, r8d
0x4AC8: sub eax, edx
0x4ACA: shr eax, 1
0x4ACC: add eax, edx
0x4ACE: xor edx, edx
0x4AD0: shr eax, 6
0x4AD3: imul eax, eax, 0x7F
0x4AD6: sub ecx, eax
0x4AD8: inc ecx
0x4AD9: mov [rdi], ecx              ; store adjusted value back

; Prepare IOCTL call
0x4ADB: lea rcx, [rbp+0x2F]
0x4ADF: call [rip+0x958B]           ; IAT@0xE070 (KeInitializeEvent?)

; Set up DeviceIoControl parameters
0x4AE5: mov rdx, [rbp-1]            ; rdx = file handle
0x4AE9: lea rax, [rbp+0x1F]
0x4AED: mov [rsp+0x40], rax         ; parameter 8: I/O status block ptr
0x4AF2: mov r9d, r14d               ; r9 = message length (3rd param)
0x4AF5: lea rax, [rbp+0x2F]
0x4AF9: mov r8, rdi                 ; r8 = message buffer pointer
0x4AFC: mov [rsp+0x38], rax         ; parameter 7: event object
0x4B01: mov ecx, 0x222018           ; *** IOCTL CODE 0x222018 ***
0x4B06: mov eax, [rbp+0x7F]
0x4B0A: mov byte [rsp+0x30], 0      ; parameter 6: NULL
0x4B0F: mov [rsp+0x28], eax         ; parameter 5: output buffer size
0x4B14: mov [rsp+0x20], rsi         ; parameter 4: output buffer (4th param)
0x4B18: call [rip+0x9603]           ; *** IoBuildDeviceIoControlRequest ***
        ; --> IAT@0xE120

; Check if request created
0x4B1E: test rax, rax
0x4B20: jne 0x4B29
0x4B22: mov ebx, 0xC0000001         ; STATUS_UNSUCCESSFUL
0x4B27: jmp 0x4B6D

; Submit IRP
0x4B29: mov rcx, [rbp-1]            ; device object
0x4B2D: mov rdx, rax                ; IRP
0x4B30: call [rip+0x95F2]           ; IoCallDriver
        ; --> IAT@0xE128

0x4B36: mov ebx, eax
0x4B38: test eax, eax
0x4B3A: js 0x4B6D                   ; error check

; Check if STATUS_PENDING (0x103)
0x4B3C: cmp eax, 0x103
0x4B3F: jne 0x4B5B

; Wait for completion
0x4B41: mov qword [rsp+0x20], 0
0x4B4A: lea rcx, [rbp+0x2F]         ; event object
0x4B4E: xor r9d, r9d
0x4B51: xor r8d, r8d
0x4B54: lea edx, [r9+5]             ; timeout
0x4B58: call [rip+0x953B]           ; KeWaitForSingleObject
        ; --> IAT@0xE098

; Get status from I/O status block
0x4B5E: mov rax, [rbp+0x27]
0x4B62: mov ebx, 0xC0000001
0x4B67: neg rax
0x4B6A: sbb ecx, ecx
0x4B6C: not ecx
0x4B6E: and ebx, ecx                ; final status

; Cleanup: close handle
0x4B70: mov rcx, [rbp+7]
0x4B74: test rcx, rcx
0x4B76: je 0x4B7E
0x4B78: call [rip+0x95BA]           ; ZwClose
        ; --> IAT@0xE138

0x4B7E: lea r11, [rsp+0xA0]
0x4B86: mov eax, ebx                ; return status
0x4B88: mov rbx, [r11+0x10]
0x4B8D: mov rsi, [r11+0x18]
0x4B92: mov rdi, [r11+0x20]
0x4B97: mov r14, [r11+0x28]
0x4B9C: mov rsp, r11
0x4B9F: pop rbp
0x4BA0: ret
```

## Function Signature

```c
NTSTATUS SendT2Message(
    void* reserved,              // rcx (not used directly)
    void* messageBuffer,         // rdx - T2 request message
    ULONG messageLength,         // r8d - input buffer size
    void* replyBuffer            // r9 - output buffer for T2 reply
);
```

## Key Observations

### 1. Device Access Path
- Opens `\DosDevices\AppleUSBVHCI` at runtime
- Uses `IoGetDeviceObjectPointer` + `IoBuildDeviceIoControlRequest`
- **Kernel-mode path** - not CreateFile/DeviceIoControl from user mode
- Handle closed after each message (no persistent handle)

### 2. IOCTL Parameters
```c
IoBuildDeviceIoControlRequest(
    0x222018,                     // IOCTL code
    deviceObject,                 // from IoGetDeviceObjectPointer
    messageBuffer,                // input buffer (T2 request)
    messageLength,                // input buffer size
    replyBuffer,                  // output buffer (T2 reply)
    outputBufferSize,             // from [rbp+0x7F] - caller provided
    FALSE,                        // InternalDeviceIoControl = FALSE
    eventObject,                  // synchronization event
    &ioStatusBlock               // status/information return
);
```

### 3. Buffer Size Calculation (Mystery)
Lines 0x4AB9-0x4AD9 perform complex arithmetic on `messageBuffer[0]`:
```
value = messageBuffer[0];
result = (value - ((value * 0x02040811) >> 32)) / 2;
result = (result + ((value * 0x02040811) >> 32)) / 64;
result = result * 0x7F;
messageBuffer[0] = value - result + 1;
```

This looks like modulo or alignment calculation. **Purpose unknown** - may be adjusting message tag or size field.

### 4. Synchronous Operation
- Creates event object
- Submits IRP via `IoCallDriver`
- If STATUS_PENDING, waits with `KeWaitForSingleObject`
- Returns final status from I/O status block

## No Buffer Mapping Found

**Critical**: This function only sends T2 protocol messages via IOCTL 0x222018. 

**Does NOT**:
- Call IOCTLs 0x222004-0x222014
- Map memory buffers
- Access PCI BARs
- Allocate DMA memory
- Register buffer addresses with T2

## What This Means

AppleAudio.sys uses IOCTL 0x222018 for **command/control messages only**:
- Device enumeration
- Property queries
- Stream control (START_IO/STOP_IO likely)
- Status/timestamp notifications

**Audio buffer path is NOT through this function**.

## Next Analysis Required

1. Search for other AppleUSBVHCI access points in AppleAudio.sys
2. Find where PCM data is written (WaveSpeaker Pin callback)
3. Look for:
   - Calls to unknown IOCTLs (0x222004-0x222014)
   - Direct memory writes to shared buffers
   - DMA buffer allocation (`MmAllocateContiguousMemory`, `WdfCommonBufferCreate`)
   - Memory mapping (`MmMapIoSpace`, `MmMapLockedPages`)

## Hypothesis

AppleAudio.sys may use **two separate mechanisms**:
1. **Message path**: IOCTL 0x222018 for T2 protocol messages (confirmed)
2. **Data path**: Unknown - possibly:
   - Unknown IOCTL for buffer registration
   - Shared memory section created by AppleUSBVHCI
   - Direct write to pre-mapped region
   - PortCls/KS framework handles buffer transfer internally

The buffer path remains **UNKNOWN** and is the blocking issue.
