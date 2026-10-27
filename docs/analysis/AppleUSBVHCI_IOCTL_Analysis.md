# AppleUSBVHCI.sys IOCTL Handler Analysis
# Static analysis results - RVA patterns and vtable calls

## Summary of Findings

All five unknown IOCTLs (0x222004, 0x222008, 0x22200C, 0x222010, 0x222014) follow a common pattern:
1. Call wrapper function at RVA 0x8A54 (adjusts input pointer by offset)
2. Load global object pointer from RVA 0x27488 (runtime-initialized, zero in static binary)
3. Dereference vtable from global object
4. Call virtual function via indirect jump/call through IAT@0x1E140

### Key Pattern: All handlers use same global object at RVA 0x27488
This is **NOT** initialized in the static binary (all zeros). The object is created at runtime, likely during driver initialization.

## Detailed Handler Analysis

### IOCTL 0x222004 (input: 8 bytes)
**Handler RVA**: 0xDB7C

**Disassembly**:
```
sub rsp, 0x28              ; stack frame
add rcx, 4                 ; skip first 4 bytes of input
call 0x8A54                ; wrapper (likely WDF lock acquisition)
mov rcx, [rip+0x198F8]     ; load global@0x27488
mov r8, rax                ; move adjusted input ptr to r8
mov rdx, [rcx]             ; rdx = vtable ptr
mov rax, [rdx+0x38]        ; rax = vtable[+0x38]
mov rdx, r8                ; rdx = adjusted input
jmp [IAT@0x1E140]          ; tail call to WDF function
```

**Structure**: 
- Input offset +0: unknown (skipped by +4)
- Input offset +4: 4-byte parameter passed to vtable function

**Vtable offset**: +0x38 (index 7 in 8-byte pointer table)

**Operation**: Calls virtual method with single 4-byte parameter from input[4..7]

---

### IOCTL 0x222008 (input: 32 bytes)
**Handler RVA**: 0xDC5C

**Disassembly**:
```
sub rsp, 0x30
mov rbx, rcx               ; save IOCTL context
add rcx, 0x1C              ; skip 28 bytes header
call 0x8A54                ; wrapper
mov rcx, [rip+0x19813]     ; global@0x27488
mov r10, rax               ; r10 = adjusted input ptr
mov r9, [rbx+8]            ; r9 = context[+8]
mov r8, [rbx]              ; r8 = context[+0]
mov rdx, [rcx]             ; vtable
mov rax, [rdx+0x28]        ; vtable[+0x28]
mov rdx, [rbx+0x10]        ; rdx = context[+16]
mov [rsp+0x20], rdx        ; 5th parameter on stack
mov rdx, r10               ; rdx = adjusted input
call [IAT@0x1E140]
```

**Structure**:
- Input offset +0..27: context/header
- Input offset +28..31: 4-byte parameter
- Uses context fields at offsets +0, +8, +16

**Vtable offset**: +0x28 (index 5)

**Operation**: Complex call with multiple context parameters and 4-byte input value. Not a simple tail call - may return data.

---

### IOCTL 0x22200C (input: 8 bytes, output: 32 bytes)
**Handler RVA**: 0xDAF0

**Disassembly**:
```
sub rsp, 0x40
add rcx, 4                 ; skip 4 bytes
mov rbx, rcx               ; save context
call 0x8A54
mov rcx, [rip+0x1997F]     ; global@0x27488
lea r10, [rbx+0x10]        ; r10 = &input[+16] (output buffer ptr?)
mov [rsp+0x28], 0          ; null 5th parameter
lea r9, [rbx+8]            ; r9 = &input[+8]
mov rdx, rax               ; rdx = wrapper result
mov [rsp+0x20], r10        ; 5th param = output ptr
mov r8, [rcx]              ; r8 = vtable
mov rax, [r8+0x30]         ; vtable[+0x30]
mov r8, rbx                ; r8 = input context
call [IAT@0x1E140]
mov [rbx+0x18], eax        ; store return value at input[+24]
xor eax, eax
```

**Structure**:
- Input offset +4..7: input parameter
- Input offset +8..15: used as pointer/reference
- Input offset +16: possibly output buffer start
- Output written to input[+24] (32-bit result)

**Vtable offset**: +0x30 (index 6)

**Operation**: Query operation - takes 8-byte input, writes result to output buffer. Return value stored at specific offset.

---

### IOCTL 0x222010 (input: 16 bytes)
**Handler RVA**: 0xDC24

**Disassembly**:
```
sub rsp, 0x20
mov rbx, rcx
add rcx, 0x0C              ; skip 12 bytes
call 0x8A54
mov rcx, [rip+0x1984B]     ; global@0x27488
mov r9, rax                ; r9 = adjusted input
mov r8d, [rbx]             ; r8d = dword from input[0..3]
mov rdx, [rcx]             ; vtable
mov rax, [rdx+0x10]        ; vtable[+0x10]
mov rdx, r9
jmp [IAT@0x1E140]          ; tail call
```

**Structure**:
- Input offset +0..3: 4-byte parameter (dword, moved as r8d)
- Input offset +12..15: adjusted input pointer

**Vtable offset**: +0x10 (index 2)

**Operation**: Simple call with dword parameter and adjusted pointer.

---

### IOCTL 0x222014 (input: 16 bytes)
**Handler RVA**: 0xDBEC

**Disassembly**:
```
sub rsp, 0x20
mov rbx, rcx
add rcx, 0x0C              ; skip 12 bytes
call 0x8A54
mov rcx, [rip+0x19883]     ; global@0x27488
mov r9, rax                ; r9 = adjusted input
mov r8, [rbx]              ; r8 = qword from input[0..7]
mov rdx, [rcx]             ; vtable
mov rax, [rdx+0x18]        ; vtable[+0x18]
mov rdx, r9
jmp [IAT@0x1E140]          ; tail call
```

**Structure**:
- Input offset +0..7: 8-byte parameter (qword)
- Input offset +12..15: adjusted input pointer

**Vtable offset**: +0x18 (index 3)

**Operation**: Similar to 0x222010 but takes 8-byte (qword) parameter instead of dword.

---

## Vtable Structure at RVA 0x23F78

Static vtable contents (8-byte entries):
```
+0x00 → 0x0140E7C0  (index 0)
+0x08 → 0x0140E850  (index 1)
+0x10 → 0x0140E1C0  (index 2) ← used by IOCTL 0x222010
+0x18 → 0x0140E180  (index 3) ← used by IOCTL 0x222014
+0x20 → 0x0140E090  (index 4)
+0x28 → 0x0140E310  (index 5) ← used by IOCTL 0x222008
+0x30 → 0x0140DE30  (index 6) ← used by IOCTL 0x22200C
+0x38 → 0x0140E150  (index 7) ← used by IOCTL 0x222004
```

**Important**: This vtable is for a base class. The actual object at global@0x27488 may use a derived class with different vtable.

## Global Object at RVA 0x27488

- Location: .data section
- Static content: all zeros (uninitialized in binary)
- **Created at runtime** during driver initialization
- Accessed by all five IOCTL handlers
- Contains pointer to vtable (first 8 bytes when initialized)

## IAT Entry at 0x1E140

All handlers perform indirect calls through IAT@0x1E140. This resolves to a WDF (Windows Driver Framework) function, likely:
- `WdfRequestComplete`
- `WdfIoQueueRetrieveNextRequest`
- or similar request handling function

## Critical Unknowns

### 1. Purpose of IOCTLs
**Cannot determine without runtime analysis**:
- Are these buffer mapping operations?
- Queue management?
- Device state control?
- Memory registration?

**Evidence against buffer mapping hypothesis**:
- No calls to `MmMapIoSpace`, `ZwMapViewOfSection`, or similar memory mapping APIs visible in pattern
- All operations go through WDF request completion path (IAT@0x1E140)
- Input sizes too small for buffer descriptors (8-32 bytes)

### 2. Global Object Type
The object at 0x27488 is runtime-created. Possible types:
- WDF Queue object (WDFQUEUE)
- WDF Device object (WDFDEVICE)
- Custom C++ object managing T2 communication state
- Bridge Control Engine (BCE) context

### 3. Virtual Function Implementations
Vtable entries point to functions at:
- 0xE7C0, 0xE850, 0xE1C0, 0xE180, 0xE090, 0xE310, 0xDE30, 0xE150

These require deeper analysis but preliminary inspection shows:
- 0xE7C0: Constructor-like pattern (initializes list head, allocates sub-objects)
- 0xE850: Destructor-like pattern (releases references, zeroes pointers)
- 0xE1C0: Query operation (checks object state, returns error if null)

**None show obvious PCI BAR mapping or large buffer allocation**.

## Comparison with Linux KAIT2EN

Linux driver uses:
- Direct PCI BAR0 mapping via `ioremap()`
- GPR registers at BAR4+0xC000 for BufferStruct location
- DMA coherent memory allocation for input buffers
- Direct memory writes to mapped BAR space

**Windows path appears different**:
- No visible PCI BAR mapping in these IOCTLs
- All operations abstracted through WDF framework
- Possibly T2 audio uses different mechanism on Windows (message-based vs. DMA?)

## Recommendations

### To identify IOCTL purpose:

1. **Runtime analysis required**:
   - Attach kernel debugger to AppleAudio.sys
   - Set breakpoints on AppleUSBVHCI IOCTLs
   - Observe what AppleAudio.sys calls during playback

2. **Import table analysis**:
   - Examine IAT@0x1E140 to identify exact WDF function
   - This reveals whether operations are queue/request/memory related

3. **Global object initialization**:
   - Find driver entry point and DriverEntry function
   - Trace initialization of object at 0x27488
   - Identify object type from allocation size and vtable assignment

4. **Cross-reference with 0x222018 (audio message handler)**:
   - Check if audio message handler also uses global@0x27488
   - If same object, these IOCTLs manage same resource as T2 protocol

### Next concrete steps:

1. Analyze AppleAudio.sys to find IOCTL calls
2. Identify IAT@0x1E140 from import directory
3. Disassemble vtable functions (0xE7C0, 0xE850, etc.) for deeper insight
4. Search for PCI BAR mapping elsewhere in AppleUSBVHCI.sys

**Conclusion**: These IOCTLs manage a single runtime-created object through virtual function dispatch. Purpose remains unknown without runtime context or AppleAudio.sys analysis showing their usage.
