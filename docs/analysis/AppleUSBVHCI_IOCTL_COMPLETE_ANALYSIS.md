# AppleUSBVHCI.sys Complete IOCTL Analysis
**Binary**: AppleUSBVHCI.sys 6.1.7500.5  
**SHA256**: 0438292090fc5267ca6d28829fee7d16da8b73e7657bdb64250cf8df832955c1  
**Analysis**: Static/offline only

---

## Executive Summary

All five IOCTLs (0x222004, 0x222008, 0x22200C, 0x222010, 0x222014) enter a runtime-created object managed by virtual dispatch. The analysis proves descriptor allocation, MDL preparation, list lookup/removal, and deeper virtual calls; it does not prove the complete external transport contract.

1. `0x222008` allocates a small kernel descriptor but prepares an MDL for a supplied address/size; the descriptor may describe a larger buffer.
2. `0x22200C` looks up an entry, calls its query method, copies descriptor fields, then unlocks/frees its MDL and entry.
3. Several operations are reached through CFG dispatch and later vtable targets; their exact semantics remain unresolved statically.
4. No conclusion that these paths are unrelated to audio resources is justified from the upper handlers alone.

The required async notifications from kaiT2en (ALIVE, UPDATE_TIMESTAMP, PROPERTY_CHANGED) are not proven in this slice. The visible bodies do not contain the BCE queue path, but unresolved CFG targets and deeper vtables prevent a categorical exclusion.

---

## Global Architecture

### Runtime Object at RVA 0x27488
**Initialization** (RVA 0xDBA8):
- Allocates 0x30 bytes for global object
- Sets vtable pointer to 0x23F78
- Stores at global location 0x27488 (.data section, NULL in static binary)
- Calls constructor vtable[+0] = 0xE7C0

**Key finding**: Global 0x27488 is NULL in static binary but populated during driver initialization with a 0x30-byte object containing at +0x00 the vtable pointer, and additional fields managing request queues.

### Vtable at RVA 0x23F78
```
+0x00 -> 0xE7C0   Constructor (initializes lists, allocates sub-objects)
+0x08 -> 0xE850   Destructor (releases references, frees sub-objects)
+0x10 -> 0xE1C0   Used by IOCTL 0x222010
+0x18 -> 0xE180   Used by IOCTL 0x222014
+0x20 -> 0xE090   List insertion operation
+0x28 -> 0xE310   Used by IOCTL 0x222008 (complex multi-parameter)
+0x30 -> 0xDE30   Used by IOCTL 0x22200C (query with output)
+0x38 -> 0xE150   Used by IOCTL 0x222004 (simple release/cleanup)
```

### CFG Dispatch Slot 0x1E140
PE Load Config contains `GuardCFDispatchFunctionPointer = VA 0x14001E140`, hence RVA `0x1E140` is a CFG dispatch pointer slot, not executable code and not an ordinary IAT import. A call through this slot dispatches to the target currently in `RAX`.

**Static content at 0x1E140**: `40 D3 01 40 01 00 00 00` (not executable code)

**Implication**: each call must be analyzed by resolving the preceding `RAX` load. For example, `0xE3F4` uses `entry.vtable[+0x50]` and `0xE445` uses `manager.vtable[+0x58]`; the values of those slots are not direct call targets in the static instruction stream.

---

## IOCTL Handlers Detail

### IOCTL 0x222004 - Simple Release/Cleanup
**Input**: 8 bytes  
**Handler**: RVA 0xDB7C  
**Vtable slot**: +0x38 (index 7, function 0xE150)

**Flow**:
- Skip first 4 bytes of input
- Call wrapper 0x8A54 (wrapper/context creation; locking is not proven)
- Load global object at 0x27488
- Call vtable[+0x38] = 0xE150

**Vtable function 0xE150**:
- Call helper 0xE618: walk list at object[+0x20], match input
- Get entry vtable and call entry->vtable[+0x38] (release method)

**Purpose**: Locate an entry using the helper and dispatch its entry-vtable method at +0x38. The exact release/completion semantics are unresolved.

---

### IOCTL 0x222008 - Complex Multi-Parameter Operation
**Input**: 32 bytes (28-byte header + 4-byte parameter)  
**Handler**: RVA 0xDC5C  
**Vtable slot**: +0x28 (index 5, function 0xE310)

**Flow**:
- Skip 28-byte header
- Call wrapper 0x8A54
- Pass context fields +0, +8, +16 as parameters
- Call vtable[+0x28] = 0xE310

**Vtable function 0xE310** (complete `.pdata` range `[0xE310,0xE4D5)`):
- Allocate a 0x60-byte descriptor from NonPagedPool with tag `ABCE`.
- Consume caller-supplied values: `R8` from input[0..7], `R9` from input[8..15], and the fifth argument from input[16..23].
- At `0xE3A3`, call `ntoskrnl.exe!IoAllocateMdl` with `RCX = [RSP+0xA8]` and `EDX = (DWORD)R12`.
- At `0xE3C3`, call `ntoskrnl.exe!MmProbeAndLockPages` on that MDL.
- At `0xE3D0`, call `0x7A68`; it creates a 0x28-byte wrapper containing the manager/context pointer and MDL-related pointer.
- At `0xE3F4`, dispatch to `entry.vtable[+0x50]`; save its returned pointer in descriptor[+0x18].
- Save the caller address/size and MDL at descriptor[+0x38], [+0x40], [+0x58].
- At `0xE445`, dispatch to `manager.vtable[+0x58]` with manager, entry, descriptor, size, and zero callback/context argument.

**Proven chain**: `0x222008 -> 0xDC5C -> 0xE310 -> IoAllocateMdl -> MmProbeAndLockPages -> 0x7A68 -> entry.vtable[+0x50] -> manager.vtable[+0x58]`. The exact insertion/submission semantics of the final manager method are unresolved because its target is loaded from `manager.vtable[+0x58]` immediately before CFG dispatch at `0xE445`.

---

### IOCTL 0x22200C - Query Operation
**Input**: 8 bytes  
**Output**: 32 bytes  
**Handler**: RVA 0xDAF0  
**Vtable slot**: +0x30 (index 6, function 0xDE30)

**Flow**:
- Skip 4 bytes
- Output buffer at input[+16]
- Call vtable[+0x30] = 0xDE30

**Vtable function 0xDE30**:
- Call helper 0xE618: locate entry
- Call entry->vtable[+0x60] (query method)
- Copy entry fields +0x10, +0x28, +0x30 to output buffer
- Get MDL pointer at entry[+0x58]
- Call MmUnlockPages, IoFreeMdl, ExFreePoolWithTag

**Purpose**: Query a previously allocated entry, copy fields to output buffer, unlock/free associated MDL, then free the entry.  
**MDL handling**: `IoAllocateMdl` and `MmProbeAndLockPages` prove that a memory range was described and locked according to the supplied MDL arguments. They do not by themselves prove user-mode origin, DMA use, or absence of a BAR/resource mapping.

---

### IOCTL 0x222010 - DWORD Parameter Operation
**Input**: 16 bytes (12-byte skip + 4-byte DWORD)  
**Handler**: RVA 0xDC24  
**Vtable slot**: +0x10 (index 2, function 0xE1C0)

**Flow**:
- Skip 12 bytes
- DWORD parameter from input[0..3]
- Call vtable[+0x10] = 0xE1C0

**Vtable function 0xE1C0**:
- Call helper 0xE618: locate entry
- Call entry->vtable[+0x40] with DWORD parameter

**Purpose**: Locate entry, call entry-specific method with DWORD parameter (possibly state/flags update).

---

### IOCTL 0x222014 - QWORD Parameter Operation
**Input**: 16 bytes (12-byte skip + 8-byte QWORD)  
**Handler**: RVA 0xDBEC  
**Vtable slot**: +0x18 (index 3, function 0xE180)

**Flow**:
- Skip 12 bytes
- QWORD parameter from input[0..7]
- Call vtable[+0x18] = 0xE180

**Vtable function 0xE180**:
- Call helper 0xE618: locate entry
- Call entry->vtable[+0x48] with QWORD parameter

**Purpose**: Similar to 0x222010 but accepts 8-byte parameter instead of 4-byte.

---

## Supporting Functions

### Wrapper 0x8A54
- Allocate 0x20 bytes
- Initialize lock object
- Set wrapper vtable at 0x1F158
- Call wrapper->vtable[+0x30] (lock acquisition method)

**Purpose**: Create a 0x20-byte wrapper/context and invoke its virtual method. Locking/serialization is not proven until the wrapper vtable target is resolved.

### Helper 0xE618
- Load list head at object[+0x20]
- Walk linked list (link at entry offset +0x78)
- Match entries by comparing IDs

**Purpose**: Walk the linked list at global object[+0x20], match entries by comparing IDs.

---

## Key Operations Identified

### List Management (vtable +0x20, function 0xE090)
- Allocate entry
- Entry link at offset +0x78
- Doubly-linked list insertion (prev/next pointers)

**Evidence**: Doubly-linked list insertion at offset +0x78 in entry structure. Confirms queue/list management pattern.

### Memory Management
- **Allocation tag**: 'ABCE' (0x45434241) used in function 0xE310
- **MDL handling**: MmUnlockPages/IoFreeMdl in function 0xDE30 release the stored MDL; origin and DMA purpose are UNKNOWN
- **Pool usage**: NonPagedPool (0x200) for queue entries

### Not established by this slice
- PCI BAR/resource mapping is not directly visible in these functions.
- A 0x60-byte allocation is a descriptor and may refer to a larger range via address/size fields.
- Event/callback or async completion semantics are not established or excluded by the upper handlers; deeper CFG targets must be resolved.
- MDL calls do not establish user-mode origin or DMA purpose on their own.

---

## Constructor/Destructor Analysis

### Constructor 0xE7C0
Object layout:
```
+0x00: vtable pointer
+0x08: sub-object 1
+0x10: sub-object 2
+0x18: sub-object 3
+0x20: list head (prev/next pointers, initially head->next = head->prev = head)
```

### Destructor 0xE850
- Release sub-object 3
- Release sub-object 2 reference and object
- Zero sub-object pointer at +0x10
- Release sub-object 1 reference

**Purpose**: Clean teardown of three managed sub-objects and their references.

---

## Comparison with kaiT2en Requirements

### kaiT2en async notifications (protocol_bce.c:177-202)
```c
static void t2audio_bce_in_queue_completion(struct t2bce_core_queue_sq *sq)
{
    // Receive messages from T2 via BCE input queue
    while ((c = t2bce_core_next_completion(sq))) {
        msg.data = (u8 *) q->data + q->data_head * q->el_size;
        t2audio_bce_in_queue_handle_msg(dev, &msg);
    }
}

static void t2audio_bce_in_queue_handle_msg(struct t2audio_device *a, struct t2audio_msg *msg)
{
    if (header->type == T2AUDIO_MSG_TYPE_RESPONSE) {
        t2audio_handle_reply(&a->bcem, msg);  // Match reply with pending request
        return;
    }
    // Defer COMMAND/NOTIFICATION to workqueue
    work = kzalloc(sizeof(*work), GFP_ATOMIC);
    work->msg.data = kmalloc(msg->size, GFP_ATOMIC);
    memcpy(work->msg.data, msg->data, msg->size);
    INIT_WORK(&work->ws, t2audio_deferred_msg_work);
    queue_work(a->msg_wq, &work->ws);
}
```

**Required pattern**: Async messages arrive via BCE input queue completion, dispatched to handlers via workqueue.

**AppleUSBVHCI IOCTLs 0x222004-0x222014**: The visible code manages descriptors and list lookup/removal/query. Async completion or notification semantics are UNKNOWN where execution continues through CFG targets and deeper vtables.

**Conclusion**: These five IOCTLs are not proven to implement the kaiT2en BCE notification path. They are also not proven unrelated to an audio/resource path; the unresolved manager and entry methods are the boundary.

---

## Findings Summary Table

| IOCTL    | Handler | Vtable | Implementation | Input/Output | Purpose                        | Async Notify | Resource Map | Audio Relevance |
|----------|---------|--------|----------------|--------------|--------------------------------|--------------|--------------|-----------------|
| 0x222004 | 0xDB7C  | +0x38  | 0xE150         | 8B in | Find entry/list record and dispatch entry method | UNKNOWN | UNKNOWN | UNKNOWN |
| 0x222008 | 0xDC5C  | +0x28  | 0xE310         | 32B in | Build descriptor, lock MDL, dispatch manager method | UNKNOWN | MDL-backed range; mapping semantics UNKNOWN | UNKNOWN |
| 0x22200C | 0xDAF0  | +0x30  | 0xDE30         | 8B in, 32B out | Query entry, copy fields, unlock/free MDL and descriptor | UNKNOWN | MDL-backed range; origin UNKNOWN | UNKNOWN |
| 0x222010 | 0xDC24  | +0x10  | 0xE1C0         | 16B in (DWORD) | Dispatch entry method with DWORD | UNKNOWN | UNKNOWN | UNKNOWN |
| 0x222014 | 0xDBEC  | +0x18  | 0xE180         | 16B in (QWORD) | Dispatch entry method with QWORD | UNKNOWN | UNKNOWN | UNKNOWN |

**Key**: The visible layer manages entries/descriptors. Final operation type, notification behavior, resource mapping, and audio relevance remain UNKNOWN at unresolved CFG/vtable targets.

---

## Unknown / Cannot Determine Statically

1. **Runtime target of 0x1E140**: Function pointer populated at runtime. Likely WDF dispatch or internal completion routine.
2. **Entry vtable methods**: Each queued entry has its own vtable with methods at +0x38, +0x40, +0x48, +0x60, +0x70, +0x78. Cannot determine semantics without runtime context.
3. **Relationship to IOCTL 0x222018**: The proven GET_DEVICE_LIST/GET_PROPERTY path uses 0x222018. Unknown if it also delivers async notifications or if Windows uses a different mechanism entirely.
4. **AppleAudio.sys interaction**: AppleAudio.sys does not reference IOCTLs 0x222004-0x222014 in reviewed binary. Unknown if these are used by other components or deprecated.

---

## Architectural Implications

### For Option A (Keep AppleUSBVHCI)
**Proven**: IOCTL 0x222018 supports synchronous GET operations for device/stream enumeration.  
**Unknown**: Async notification delivery mechanism (ALIVE, UPDATE_TIMESTAMP, PROPERTY_CHANGED).  
**Missing**: PCM buffer mapping contract, timestamp event delivery to WaveRT/PortCls.

IOCTLs 0x222004-0x222014 provide a visible descriptor/list/MDL path, but do not yet establish the async notification or audio buffer contract.

### For Option B (Port BCE)
kaiT2en demonstrates complete ownership: PCI/BAR, BCE queues, async notification dispatch via workqueue, and buffer mapping. Windows port requires implementing BCE queue polling and notification dispatch, but the conceptual model is proven.

**Status**: IOCTLs 0x222004-0x222014 provide a concrete resource-descriptor candidate, but the unresolved CFG targets prevent a final audio-transport classification. Option A remains open pending that resolution and the AppleAudio contract.
