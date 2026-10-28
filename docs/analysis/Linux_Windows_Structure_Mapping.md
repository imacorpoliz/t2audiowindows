# Structure mapping: Linux KAIT2EN ↔ AppleAudio.sys

**Date**: 2026-10-04  
**Goal**: map the T2 hardware structures between the Linux driver and Windows AppleAudio.sys

> **Status note (2026-10-28):** an early document. Its "critical difference" in §3 was a misreading (the BufferStruct offset is in GPR[2] on both systems; T2AudioPort uses that and finds the ring on hardware). The PCM write path it could not find is AppleAudio's copy loop (`RE_appleaudio_protocol.md` §3). The recommended hybrid approach is essentially what T2AudioPort became, except that the WaveRT buffer is in system memory and copied into the BAR (a BAR cannot be mapped into user mode by PortCls).

---

## 1. Hardware Layout (PCI BARs)

### Linux (KAIT2EN)

```c
// audio.c:100-104
t2audio->reg_mem_bs_dma = pci_resource_start(dev, 0);  // BAR0 physical address
t2audio->reg_mem_bs = pci_iomap(dev, 0, 0);            // BAR0 mapped (buffer struct region)
t2audio->reg_mem_cfg = pci_iomap(dev, 4, 0);           // BAR4 mapped (config registers)

t2audio->reg_mem_gpr = (u32 __iomem *) ((u8 __iomem *) t2audio->reg_mem_cfg + 0xC000);
```

**BAR0**: buffer memory region (DMA buffers, metadata)  
**BAR4**: configuration registers  
**BAR4+0xC000**: GPR (General Purpose Registers), pointer to BufferStruct

### Windows (AppleAudio.sys)

```c
// Function 0x16FF0 (reconstruction)
// Iterates over the CM_RESOURCE_LIST, filtering type == 3 (CmResourceTypeMemory)
// First memory resource:
MmMapIoSpaceEx(phys1, size1, 0x404);  // -> [rbx+0x68]  // Cached|WriteCombined
// Second memory resource:
MmMapIoSpaceEx(phys2, size2, 0x204);  // -> [rbx+0x70]  // NonCached|WriteCombined

// Function 0x45F0: written into the global object 0x14690
[global+0x28] = [rbx+0x68]              // BAR0 mapped
[global+0x30] = [rbx+0x70] + 0xC000     // BAR4+0xC000 mapped
```

**Mapping**:

| Linux                  | Windows                        | Purpose                             |
|------------------------|--------------------------------|-------------------------------------|
| `reg_mem_bs` (BAR0)    | `[global+0x28]` ([rbx+0x68])   | Buffer memory (DMA buffers)         |
| `reg_mem_gpr` (BAR4+0xC000) | `[global+0x30]` ([rbx+0x70]+0xC000) | GPR region (BufferStruct pointer) |

---

## 2. GPR Structure (BAR4+0xC000)

### Linux (KAIT2EN)

```c
// audio.c:581-592
ver = ioread32(&a->reg_mem_gpr[0]);          // GPR[0] = version (must be >= 3)
sig = ioread32(&a->reg_mem_gpr[1]);          // GPR[1] = signature (0x19870423)
bs_base = ioread32(&a->reg_mem_gpr[2]);      // GPR[2] = BufferStruct offset relative to BAR0
a->bs = (struct t2audio_buffer_struct *) ((u8 *) a->reg_mem_bs + bs_base);
```

**GPR layout**:
```
Offset  Size  Field
------  ----  -----------------------
0x00    4     Version (>= 3)
0x04    4     Signature (0x19870423)
0x08    4     BufferStruct offset (relative to BAR0)
0x0C    ?     (reserved/unknown)
```

### Windows (AppleAudio.sys)

```c
// Function 0x4718-0x4750
rax = [rcx+0x30];                          // rcx = global @ 0x14690
                                           // rax = BAR4+0xC000 (GPR region)
if ([rax+4] != 0x19870423) return ERROR;   // magic check
if ([rax+0] < 2) return ERROR;             // version check (>= 2, not >= 3!)
ebx = [rax+8];                             // buffer offset
rbx = [rcx+0x28] + ebx;                    // rbx = BAR0 + offset
```

**GPR mapping**:

| Offset | Linux field     | Windows check (RVA 0x4728-0x4744) | Match |
|--------|-----------------|-----------------------------------|-------|
| +0x00  | version         | `[rax]` >= 2                      | ✓ (different minimum versions) |
| +0x04  | signature       | `[rax+4]` == 0x19870423           | ✓     |
| +0x08  | bs_base         | `[rax+8]` = buffer offset         | ✓     |

**Difference**: Linux requires version >= 3, Windows >= 2.

---

## 3. BufferStruct (in BAR0 at the offset from GPR[2])

### Linux (KAIT2EN)

```c
// audio.h:27-51
struct t2audio_buffer_struct_buffer {
    size_t address;        // +0x00: offset relative to BAR0
    size_t size;           // +0x08: buffer size
    size_t pad[4];         // +0x10: padding
};  // sizeof = 0x30 (48 bytes)

struct t2audio_buffer_struct_stream {
    u8 num_buffers;                                    // +0x00
    struct t2audio_buffer_struct_buffer buffers[100];  // +0x01
    char filler[32];                                   // stride compensation
};  // stride ~= 0x12E0

struct t2audio_buffer_struct_device {
    char name[128];                                     // +0x000: device UID (e.g. "Speaker")
    u8 num_input_streams;                               // +0x080
    u8 num_output_streams;                              // +0x081
    struct t2audio_buffer_struct_stream input_streams[5];   // +0x082
    struct t2audio_buffer_struct_stream output_streams[5];  // offset varies
    char filler[128];
};  // stride ~= 0xBDEC (Linux measurement from 0x4757: imul rax, r12, 0xBDEC)

struct t2audio_buffer_struct {
    u32 version;                                        // +0x00
    u32 signature;                                      // +0x04 = 0x19870423
    u32 flags;                                          // +0x08
    u8 num_devices;                                     // +0x0C
    struct t2audio_buffer_struct_device devices[20];    // +0x10
};
```

### Windows (AppleAudio.sys)

```c
// Function 0x4744-0x4750 (reconstructed from the disassembly)
ebx = [rax+8];                          // rax = GPR region
                                        // ebx = BufferStruct offset
rbx = [rcx+0x28] + ebx;                 // rbx = BAR0 + offset = BufferStruct base

// BufferStruct (partly reconstructed):
[rbx+0x00]  ?     version (check: >= 2 at 0x473B)
[rbx+0x04]  u32   signature = 0x19870423 (check at 0x4728)
[rbx+0x08]  ?     offset (used at 0x4744)
[rbx+0x0C]  u8    count (used at 0x474D)
[rbx+0x10+] ?     array of 0xBDEC-byte structures (0x475B: imul rax, r12, 0xBDEC)
```

**BufferStruct mapping**:

| Offset | Linux field       | Windows usage (RVA 0x4728-0x4750) | Match |
|--------|-------------------|-----------------------------------|-------|
| +0x00  | version           | check `[rbx] >= 2` (0x473B)       | ✓     |
| +0x04  | signature         | check `[rbx+4] == 0x19870423`     | ✓     |
| +0x08  | flags (Linux)     | `[rbx+8]` used as an offset       | ⚠ DIFFERENCE |
| +0x0C  | num_devices       | `[rbx+0xC]` = count (0x474D)      | ✓     |
| +0x10+ | devices[]         | array of structures, stride 0xBDEC | ✓    |

**CRITICAL DIFFERENCE** (later shown to be a misreading: the checks at 0x4728–0x4744 are on the GPR block, `rax`, not on BufferStruct): 
- **Linux**: the BufferStruct offset is in **GPR[2]** (BAR4+0xC000+8)
- **Windows**: reads the offset from **BufferStruct[0x08]** (the `flags` field of the Linux structure)

**Possible explanations** (at the time):
1. Windows uses **another protocol version** (version 2 vs 3)
2. Field +0x08 has a **dual purpose** (flags + offset)
3. The code at 0x4744 reads **not BufferStruct directly** but another structure at BAR4+0xC000 (this is the right one)

---

## 4. Device Entry Structure (BufferStruct.devices[])

### Linux (KAIT2EN)

```c
// audio.h:37-44
struct t2audio_buffer_struct_device {
    char name[128];                      // +0x000: "Speaker", "Microphone", etc.
    u8 num_input_streams;                // +0x080
    u8 num_output_streams;               // +0x081
    struct t2audio_buffer_struct_stream input_streams[5];
    struct t2audio_buffer_struct_stream output_streams[5];
    char filler[128];
};
// stride = 0xBDEC (48620 bytes)
```

### Windows (AppleAudio.sys)

```c
// Function 0x4757-0x4762
lea rdx, [rbx+0x10]                     // rdx = base of the structure (rbx = BufferStruct)
imul rax, r12, 0xBDEC                   // rax = index * 0xBDEC
add rdx, rax                            // rdx = devices[index]
mov r8d, 0x80                           // size = 0x80 (128 bytes)
lea rcx, [r14+0x21]                     // rcx = dest
call 0x4CC0                             // memcpy(dest, devices[i]+?, 128)
```

**Stride 0xBDEC**: ✓ MATCHES Linux

**Windows copies 128 bytes** → probably the `name[128]` field (+0x00 of the device entry). (Later: it is `_strnicmp` of the name, see `Function_46F8_Analysis.md`.)

---

## 5. Stream Buffer Access (PCM data path)

### Linux (KAIT2EN)

```c
// audio.c:710-732 (t2audio_init_bs_stream)
for (i = 0; i < strm->buffer_cnt; i++) {
    strm->buffers[i].dma_addr = a->reg_mem_bs_dma + bs_strm->buffers[i].address;
    strm->buffers[i].ptr = a->reg_mem_bs + bs_strm->buffers[i].address;
    strm->buffers[i].size = bs_strm->buffers[i].size;
    strm->buffers[i].type = T2AUDIO_DMA_BUF_IOMEM;
}

// pcm.c:224-226 (t2audio_pcm_hw_params)
// ALSA uses the T2's mapped memory directly:
substream->runtime->dma_area = astream->buffers[0].ptr;      // CPU pointer
substream->runtime->dma_addr = astream->buffers[0].dma_addr; // DMA address
substream->runtime->dma_bytes = astream->buffers[0].size;
```

**Mechanism**: ALSA **writes directly into the mapped BAR0 memory** (zero-copy).

**Linux data path**:
```
User space PCM write
    ↓
ALSA kernel copy_to_user
    ↓
substream->runtime->dma_area (= mapped BAR0 + offset)
    ↓
T2 hardware reads from its own memory (shared memory, not a DMA transfer)
```

### Windows (AppleAudio.sys)

**NOT FOUND** (at the time): the mechanism writing PCM samples into the mapped buffer.

**Operations on mapped memory found**:
1. **Function 0x16FF0**: initialising the mapped addresses (startup)
2. **Function 0x4700**: **reading metadata** from BufferStruct (not PCM writing)
3. **Function 0x45F0**: storing the mapped addresses in the global object
4. **347 accesses** to `[global+0x28]` and `[global+0x30]` in the code

**Function 0x4700-0x4750 does**:
```
1. Reads GPR[2] (BufferStruct offset)
2. Checks the BufferStruct magic/version
3. Iterates over devices[] (stride 0xBDEC)
4. Copies the device name (128 bytes) into a local buffer
```

**This is NOT the PCM data path**; it reads device metadata.

---

## 6. Missing Piece: Windows PCM Write Path

### What should exist (by analogy with Linux):

```c
// Hypothetical Windows code (NOT FOUND in AppleAudio.sys at the time)
void WritePCMSamples(DEVICE_CONTEXT* ctx, void* samples, size_t size) {
    void* mapped_buffer = ctx->global->bar0_mapped;  // [global+0x28]
    size_t offset = GetCurrentBufferOffset(ctx);
    
    // Direct write into mapped memory:
    memcpy((u8*)mapped_buffer + offset, samples, size);
    // or through write-combining stores for performance
}
```

### Why it was not found:

**Hypothesis 1**: it uses the **PortCls framework**
- A KS pin callback (WaveSpeaker) provides the buffer through `IMiniportWaveRTStream::GetBuffer()`
- PortCls copies the data directly through the mapped pointer
- The copy happens in **portcls.sys**, not in AppleAudio.sys

**Hypothesis 2**: it uses **WaveRT shared memory mode**
- The mapped BAR0 region is exported as the WaveRT buffer
- The user-mode application writes directly through a memory mapping
- No kernel-side copy is needed

**Hypothesis 3**: dispatch through a **vtable callback** (no xref found)
- Function 0x16FF0 is called through an interface pointer (context->vtable[N])
- The PCM write too, but on another interface
- Static analysis does not find indirect calls

(Answer, found later: none of these; AppleAudio has its own copy loop `0x14000A715`–`0x14000B2C0` that converts, filters and writes the samples into the ring.)

---

## 7. Comparison table: full reconstruction

| Component | Linux (KAIT2EN) | Windows (AppleAudio.sys) | Status |
|-----------|-----------------|--------------------------|--------|
| **BAR0 mapping** | `pci_iomap(dev, 0)` | `MmMapIoSpaceEx(phys1, size1, 0x404)` | ✓ FOUND |
| **BAR4 mapping** | `pci_iomap(dev, 4)` | `MmMapIoSpaceEx(phys2, size2, 0x204)` | ✓ FOUND |
| **GPR access** | `reg_mem_cfg + 0xC000` | `[global+0x30]` (= BAR4+0xC000) | ✓ FOUND |
| **GPR[0] version** | `ioread32(&gpr[0])` >= 3 | `[rax]` >= 2 | ✓ FOUND (different versions) |
| **GPR[1] signature** | `ioread32(&gpr[1])` == 0x19870423 | `[rax+4]` == 0x19870423 | ✓ FOUND |
| **GPR[2] bs_offset** | `ioread32(&gpr[2])` | `[rax+8]` (?) | ⚠ UNCERTAIN (later ✓) |
| **BufferStruct base** | `BAR0 + GPR[2]` | `BAR0 + [GPR+8]` | ⚠ UNCERTAIN (later ✓) |
| **BufferStruct.signature** | `bs->signature` == 0x19870423 | `[rbx+4]` == 0x19870423 | ✓ FOUND |
| **BufferStruct.num_devices** | `bs->num_devices` | `[rbx+0xC]` | ✓ FOUND |
| **Device entry stride** | 0xBDEC | 0xBDEC | ✓ FOUND |
| **Device name copy** | `strcpy(dev->name)` | `memcpy(dest, src, 0x80)` | ✓ FOUND |
| **Stream buffer setup** | `strm->buffers[i].ptr = BAR0 + offset` | ? | ✗ NOT FOUND (later: `0x9D74`) |
| **ALSA DMA area** | `runtime->dma_area = buffers[0].ptr` | ? | ✗ NOT FOUND |
| **PCM write** | ALSA → dma_area (zero-copy) | ? | ✗ NOT FOUND (later: copy loop) |

---

## 8. The missing mechanism on Windows

### What was NOT FOUND:

1. **The function reading the stream buffer offsets** from `BufferStruct.devices[i].output_streams[j].buffers[k]`
2. **The function copying PCM samples** into `BAR0 + buffer_offset`
3. **WaveRT buffer registration**: a call to `portcls!PcRegisterWaveRTBuffer` or similar
4. **KS pin SetFormat callback**: setting the stream buffer address
5. **Direct write** through the mapped pointer in an IRQ/DPC handler

### Where to look next:

1. **Consumers of the global object 0x14690**: 347 accesses to fields +0x28/+0x30
   - Analyse the functions reading `[global+0x28]` (the BAR0 base)
   - Find functions with `BAR0 + offset` arithmetic + data writes

2. **PortCls callbacks**: check the vtable dispatch functions
   - `IMiniportWaveRTStream::AllocateAudioBuffer`
   - `IMiniportWaveRTStream::GetHWLatency`
   - `IMiniportWaveCyclic::NewStream`

3. **Functions with audio format parameters** (sample rate, channels, bit depth)
   - Find the code computing buffer size = `bytes_per_sample * channels * frame_count`
   - Follow where the computed size goes

4. **DMA/MMIO write loops**: patterns like
   ```asm
   mov [rdi], eax
   add rdi, 4
   loop ...
   ```

5. **Runtime debugging**:
   - A breakpoint on read access of `[global+0x28]`
   - Trace the call stacks of functions using the BAR0 pointer
   - Monitor memory writes into the BAR0 region during audio playback

---

## 9. Practical conclusion: Windows implementation strategy

### Option A: reverse engineer Apple's existing interface

**Pros**:
- Reuses the existing WDF/PortCls components
- Compatible with existing user-mode components (if any)

**Cons**:
- **Blocker**: the PCM write path was not found by static analysis
- Needs runtime debugging (WinDbg, kernel breakpoints)
- May depend on closed WDF/PortCls internal APIs
- Hard reverse engineering of vtable dispatch and PortCls callbacks

**Effort estimate**: 
- 40-80 hours of runtime debugging
- 20-40 hours of reimplementation after understanding the protocol
- Risk: the critical logic may turn out to be in the T2 firmware (inaccessible)

### Option B: port the Linux logic to Windows

**Pros**:
- **Full source code** of the Linux driver is available
- **Proven protocol**: works on real hardware
- Direct control of the whole chain (BAR mapping → PCM write)
- Only the audio part needs adapting (not the whole T2BCE stack)

**Cons**:
- The ALSA part has to be rewritten for WaveRT/WaveCyclic
- Porting the Linux DMA API → Windows (IoAllocateMdl, MmMapLockedPages)
- Porting workqueue/spinlock → Windows (KDPC, KeAcquireSpinLock)
- Testing on real hardware (risk of damaging the device)

**Effort estimate**:
- 20-40 hours to port the core audio logic (pcm.c → Windows)
- 10-20 hours to adapt the WaveRT interface
- 10-20 hours of debugging on real hardware
- Risk: lower, since the logic is known

### Option C: hybrid approach (recommended)

**Strategy**:
1. **Take from Linux** (proven):
   - BufferStruct layout and parsing (audio.c:574-642)
   - Stream buffer setup (audio.c:710-741)
   - PCM timing logic (pcm.c:150-160, timestamp handling)

2. **Take from AppleAudio.sys** (already works):
   - BAR mapping with MmMapIoSpaceEx (function 0x16FF0)
   - Global object structure (0x14690)
   - Device initialisation sequence (function call order)

3. **Write anew** (minimum):
   - WaveRT buffer registration:
     ```c
     NTSTATUS SetupWaveRTBuffer(DEVICE_CTX* ctx) {
         // Use the mapped BAR0 pointer as in AppleAudio.sys
         void* bar0 = ctx->global->bar0_mapped;
         size_t offset = GetStreamBufferOffset(ctx);  // from BufferStruct
         
         // Export it to WaveRT as user-accessible memory
         MmMapIoSpace(bar0_phys + offset, size, MmNonCached);
     }
     ```
     (Did not work in practice: PortCls cannot map BAR pages into user mode, `0x8007001F`; the driver uses a system-memory buffer and copies.)

   - PCM write callback (from the Linux logic):
     ```c
     void WritePCMData(STREAM_CTX* stream, void* samples, size_t frames) {
         void* dma_area = stream->buffer_ptr;  // mapped BAR0 + offset
         size_t bytes = frames * stream->bytes_per_frame;
         
         // Direct write (as in Linux pcm.c:224)
         memcpy(dma_area, samples, bytes);
         wmb();  // write barrier
     }
     ```

**Effort estimate**:
- 10 hours: studying the existing AppleAudio.sys device init
- 15 hours: porting the BufferStruct parsing from Linux
- 20 hours: implementing the WaveRT interface over mapped memory
- 15 hours: testing and debugging
- **TOTAL: ~60 hours** (a realistic estimate for an experienced Windows kernel developer)

---

## 10. Concrete recommendations

### Immediate next steps (runtime analysis):

1. **A WinDbg session on a live system**:
   ```
   kd> bp AppleAudio+0x16FF0  ".echo MmMapIoSpaceEx init; g"
   kd> ba r8 AppleAudio!global_object+0x28  "!analyze -v; kv"
   ```
   Goal: catch the functions reading the BAR0 pointer after initialisation

2. **Memory monitoring during playback**:
   ```
   kd> !address <BAR0_address>
   kd> ba w4 <BAR0_address>+<offset>  "kv; g"
   ```
   Goal: find the function writing PCM data

3. **PortCls trace**:
   ```
   kd> !devnode 0 1 AppleAudio
   kd> !devstack <PDO>
   kd> bp portcls!PcNewMiniport
   kd> bp portcls!PcRegisterSubdevice
   ```
   Goal: intercept the PortCls callbacks and the vtable setup

### If runtime analysis is blocked (no test hardware):

**Recommendation**: port the Linux logic.

**Minimal viable implementation**:
1. Copy the `t2audio_buffer_struct` definitions from audio.h
2. Port `t2audio_init_bs()` (audio.c:574-642) to Windows
3. Port `t2audio_init_bs_stream()` (audio.c:710-741)
4. Write a WaveRT miniport with direct memory access:
   ```c
   runtime->dma_area = mapped_bar0 + stream_offset;  // Linux pcm.c:224
   ```
5. Protocol minimum: playback only, Speaker device only

**Expected result**: a proof-of-concept driver playing audio through the T2 without the full protocol.

**Risk mitigation**: start with read-only operations (enumerate devices, read BufferStruct); do not write into BAR0 before the protocol is understood.

---

## 11. Final verdict

| Criterion | Apple interface RE | Linux port | Hybrid |
|----------|-------------------|------------|--------|
| Effort | 100-160 h | 60-80 h | **40-60 h** |
| Risk (hardware) | Medium | High | Medium |
| Risk (software) | High | Low | **Low** |
| Feature completeness | 100% (if found) | 90% (audio only) | **80%** (enough) |
| Maintainability | Low (black box) | Medium | **High** |

**Recommendation**: **Option C (hybrid)**

**Reasons**:
1. The Linux code gives **guaranteed-working logic** for BufferStruct
2. AppleAudio.sys gives a **proven initialisation** of the BAR mapping on Windows
3. The combination minimises **reverse engineering effort** and **implementation risk**
4. It can start with **safe read-only operations** (enumerate, parse metadata)
5. The PCM write is a simple `memcpy()` after setup and needs no complex RE

**Next action** (at the time): 
1. Port the `t2audio_buffer_struct` definitions → Windows headers
2. Write a BufferStruct parser based on Linux audio.c:574-642
3. Create a minimal WaveRT miniport with mapped memory (without the PortCls abstraction layer)
4. Test on a virtual machine with mock hardware (if no real MacBook is available)
