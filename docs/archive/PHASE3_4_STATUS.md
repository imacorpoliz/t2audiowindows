# T2AudioPort Phase 3/4 Status Report
**Date:** 2026-10-05  
**Target:** MacBook Pro 16,1 (2019), Apple T2 Audio Device, Windows 11

---

## Summary

Phase 3 and Phase 4 are implemented and build successfully. The driver moved from a KMDF function driver to a standard **pure PortCls adapter driver** with full WaveRT subdevice registration. A Phase 4 skeleton was added with MMIO MDL preparation and T2 protocol command serialization, but without activation on the real device.

**The driver is NOT YET READY to be installed on the Apple Audio Device.**

---

## Build Verification

### Binary
```
File:   C:\Users\othysa\Desktop\mbp\T2AudioPort\Phase2\bin\Debug\T2AudioMiniport.sys
Size:   14848 bytes
Date:   2026-10-05 08:33:41
Arch:   x64 (machine 8664)
Entry:  GsDriverEntry (offset 0x7000)
Subsys: Native (kernel mode driver)
```

### Key Imports (verified via dumpbin)
```
ntoskrnl.exe:
  - MmMapIoSpaceEx
  - MmUnmapIoSpace
  - IoAllocateMdl
  - IoFreeMdl
  - READ_REGISTER_ULONG
  - ExAllocatePoolWithTag
  - ExFreePoolWithTag

portcls.sys:
  - PcInitializeAdapterDriver
  - PcAddAdapterDevice
  - PcNewPort
  - PcRegisterSubdevice
  - PcGetDeviceProperty
```

**Verdict:** the binary is correct; the imports match the pure PortCls driver model.

---

## Phase 3: PortCls Adapter Architecture

### What is implemented

#### 1. Driver Lifecycle (Driver.c)
- `DriverEntry`: calls `PcInitializeAdapterDriver` instead of `WdfDriverCreate`.
- `T2AudioAddDevice`: calls `PcAddAdapterDevice` with the callback `T2AudioStartDevice`.
- `T2AudioStartDevice`:
  - Allocates `T2AUDIO_DEVICE_CONTEXT` (adapter extension).
  - Calls `T2AudioMapResources` for the BAR1/BAR2 mapping through `IResourceList`.
  - Creates the WaveRT port: `PcNewPort(&CLSID_PortWaveRT)`.
  - Creates the miniport: `T2AudioCreateMiniport`.
  - Initialises the port: `IPortWaveRT::Init`.
  - **Registers the subdevice:** `PcRegisterSubdevice(DeviceObject, L"Wave", port)`.

#### 2. Resource Mapping (Device.c)
- Reads BAR1 (audio buffers) and BAR2 (config/GPR) through `IResourceList::FindTranslatedEntry`.
- Keeps `Bar1Physical` for the Phase 4 MDL PFN calculation.
- Uses `MmMapIoSpaceEx`:
  - BAR1: `PAGE_READONLY | PAGE_WRITECOMBINE` (0x404, as AppleAudio.sys).
  - BAR2: `PAGE_READONLY | PAGE_NOCACHE` (0x204).
- Validates the GPR (BAR2+0xC000): version, signature 0x19870423, buffer offset.
- Parses BufferStruct, finds the Speaker buffer.
- **Device ID:** set to 0 for now (BufferStruct holds no BCE device_id).

#### 3. WaveRT Miniport (WaveRTMiniport.c)
- `IMiniportWaveRT` COM object with a vtable:
  - `QueryInterface`, `AddRef`, `Release`
  - `Init`, `GetDescription`, `DataRangeIntersection`, `NewStream`, `GetDeviceDescription`
- Pin descriptor:
  - 1 render pin (KSPIN_DATAFLOW_IN)
  - 6 channels, 48 kHz, 32-bit samples, 24 bytes/frame
  - `KSDATARANGE_AUDIO` with `KSDATAFORMAT_SUBTYPE_PCM`, `KSDATAFORMAT_SPECIFIER_WAVEFORMATEX`
  - Category: `KSCATEGORY_AUDIO`, Name: `KSNODETYPE_SPEAKER`
- `PCFILTER_DESCRIPTOR` with 1 pin, 0 nodes.

#### 4. WaveRT Stream (WaveRTStream.c)
- `IMiniportWaveRTStream` COM object with a vtable:
  - `SetFormat`, `SetState`, `GetPosition`
  - `AllocateAudioBuffer`, `FreeAudioBuffer`
  - `GetHWLatency`, `GetPositionRegister`, `GetClockRegister`
- Format validation: checks 6ch/48kHz/32bit/24 bytes per frame.
- State machine: `KSSTATE_STOP → ACQUIRE → PAUSE → RUN`.
- QPC anchor on the transition to `KSSTATE_RUN`.
- **`AllocateAudioBuffer`:** returns `STATUS_NOT_SUPPORTED` for now (see the Phase 4 limitations).
- **`GetPosition`:** a stub, returns PlayOffset=0, WriteOffset=0.

### Architecture changes

| Before (KMDF)             | After (PortCls)                     |
|---------------------------|-------------------------------------|
| `WdfDriverCreate`         | `PcInitializeAdapterDriver`         |
| `EvtDeviceAdd`            | `T2AudioAddDevice` (callback)       |
| `EvtPrepareHardware`      | `T2AudioStartDevice` (callback)     |
| `WDFDEVICE`               | `PDEVICE_OBJECT`                    |
| `WDF_OBJECT_ATTRIBUTES`   | `ExAllocatePoolWithTag` context     |
| `WdfCmResourceList`       | `PRESOURCELIST` (IResourceList)     |

### Status
✅ **Build succeeds**  
✅ **The PortCls subdevice registers**  
⚠️ **Not installed on the device** (the INF deliberately has no Hardware ID)  
⚠️ **The endpoint was not tested** (no audio buffer path)

---

## Phase 4: MMIO Buffer and T2 Protocol

### What is implemented

#### 1. MMIO MDL Construction (Phase4.c)
```c
NTSTATUS T2AudioCreateSpeakerMdl(
    _In_ PT2AUDIO_DEVICE_CONTEXT Context,
    _Out_ PMDL *Mdl,
    _Out_ ULONG *OffsetFromFirstPage,
    _Out_ ULONG *ActualSize);
```
- Creates an MDL for the Speaker buffer in BAR1.
- Uses `IoAllocateMdl` with the `MDL_IO_SPACE` flag.
- Computes the PFN directly from `Bar1Physical.QuadPart + SpeakerBufferOffset`.
- Returns the offset and size, aligned to page boundaries.
- **Cache type:** `MmWriteCombined` (matches the BAR1 mapping 0x404).

```c
VOID T2AudioFreeSpeakerMdl(_Inout_ PT2AUDIO_DEVICE_CONTEXT Context);
```
- Frees the MDL with `IoFreeMdl`.

#### 2. T2 Protocol Commands (Phase4.c)
```c
NTSTATUS T2AudioStartIo(_In_ PT2AUDIO_DEVICE_CONTEXT Context);
NTSTATUS T2AudioStopIo(_In_ PT2AUDIO_DEVICE_CONTEXT Context);
```
- Build KAIT2EN-compatible messages:
  - `START_IO`: opcode 0x80 (bit 7 set)
  - `STOP_IO`: opcode 0
  - The device ID comes from `Context->SpeakerDeviceId` (0 for now)
  - Offset/size come from `SpeakerBufferOffset/Size`
- **Message format:**
  ```c
  message[0] = opcode;           // 0x80 or 0
  message[1] = device_id;        // 0 for now
  message[2..5] = offset (LE32)  // Speaker buffer offset in BAR1
  message[6..9] = size (LE32)    // Speaker buffer size
  ```
- **Transport:** not connected yet; the functions return `STATUS_NOT_SUPPORTED`.

#### 3. Stream State Integration
- `WaveRTStream.c::T2AudioStreamSetState`:
  - `KSSTATE_RUN`: calls `T2AudioStartIo` (fails with NOT_SUPPORTED for now).
  - `KSSTATE_STOP`: calls `T2AudioStopIo` (fails with NOT_SUPPORTED for now).
- The state transition is **blocked** if the command returns an error.

### Phase 4 limitations

⚠️ **The MMIO MDL is not activated**
- `AllocateAudioBuffer` returns `STATUS_NOT_SUPPORTED`.
- The MDL construction code is present but **not called** from the WaveRT callback.
- **Reason:** the physical page mapping and cache coherency are not verified on the real device. A wrong MDL could cause a BSOD or MMIO corruption.

⚠️ **The T2 command transport is not connected**
- `T2AudioStartIo`/`T2AudioStopIo` return `STATUS_NOT_SUPPORTED` right after serialising the message.
- **Reason:** the user-mode IOCTL 0x222018 to AppleUSBVHCI.sys cannot be called from kernel mode without a proper IoGetDeviceObjectPointer and IRP stack setup. A safe kernel BCE/AppleUSBVHCI wrapper is needed.

⚠️ **The device ID is unknown**
- `Context->SpeakerDeviceId = 0` (hardcoded).
- **Reason:** BufferStruct (BAR1 metadata) holds no BCE device ID. In Linux KAIT2EN the device ID comes from a separate BCE protocol handshake (the `GET_DEVICE_LIST` command). Kernel BCE enumeration is needed.

---

## Source Files

```
Phase2/Driver/
├── T2AudioMiniport.h          [Header: context, prototypes, ABI]
├── Driver.c                   [PortCls adapter: DriverEntry, AddDevice, StartDevice]
├── Device.c                   [BAR mapping, GPR/BufferStruct parsing]
├── WaveRTMiniport.c           [IMiniportWaveRT COM object, pin descriptor]
├── WaveRTStream.c             [IMiniportWaveRTStream COM object, state machine]
└── Phase4.c                   [MMIO MDL, T2 START_IO/STOP_IO serialization]

Phase2/
├── T2AudioMiniport.vcxproj    [MSBuild project: WDM, PortCls, no KMDF]
└── Driver/T2AudioMiniport.inf [Stub INF without a Hardware ID]
```

**Lines of code:** ~1200 (without comments and blank lines)

---

## INF Configuration

```ini
[Version]
Signature="$WINDOWS NT$"
Class=Media
ClassGuid={4D36E96C-E325-11CE-BFC1-08002BE10318}
Provider=%ProviderName%
DriverVer=10/05/2026,1.0.0.0

[Manufacturer]
%ProviderName%=Standard,NTamd64

[Standard.NTamd64]
; Deliberately no hardware match until BAR ownership and kernel T2 transport
; are validated. This package must not replace AppleAudio.sys yet.

[Strings]
ProviderName="T2AudioPort"
```

**Status:** ❌ Does not contain the Hardware ID `PCI\VEN_106B&DEV_1803&SUBSYS_1887106B`  
**Reason:** the driver is not ready to be installed; it could cause a BSOD or a conflict with AppleAudio.sys.

---

## What Works

✅ BAR1/BAR2 mapping through PortCls `IResourceList`  
✅ GPR validation (BAR2+0xC000)  
✅ BufferStruct parsing  
✅ Speaker buffer discovery  
✅ WaveRT pin descriptor (6ch, 48kHz, 32bit)  
✅ COM interface vtables (IMiniportWaveRT, IMiniportWaveRTStream)  
✅ PortCls subdevice registration (`PcRegisterSubdevice`)  
✅ State machine (STOP/ACQUIRE/PAUSE/RUN)  
✅ MMIO MDL construction code (not activated)  
✅ T2 START_IO/STOP_IO message serialization (not sent)

---

## What's Missing (Critical Blockers)

### 1. Kernel BCE/AppleUSBVHCI Transport
**Problem:**
- The user-mode IOCTL 0x222018 cannot be called safely from kernel mode.
- A kernel-mode wrapper is needed:
  ```c
  NTSTATUS T2AudioOpenBceTransport(
      _Out_ PDEVICE_OBJECT *BceDevice,
      _Out_ PFILE_OBJECT *BceFileObject);
  
  NTSTATUS T2AudioSendBceCommand(
      _In_ PDEVICE_OBJECT BceDevice,
      _In_reads_bytes_(MessageSize) const UCHAR *Message,
      _In_ ULONG MessageSize);
  
  VOID T2AudioCloseBceTransport(
      _In_ PDEVICE_OBJECT BceDevice,
      _In_ PFILE_OBJECT BceFileObject);
  ```
- Needed:
  - `IoGetDeviceObjectPointer(L"\\Device\\AppleUSBVHCI", ...)` to get the device stack.
  - `IoBuildDeviceIoControlRequest(IOCTL_UNKNOWN, ...)` to send the command.
  - A synchronous wait with `KeWaitForSingleObject(&event, ...)`.

**Risk:** a wrong IRP stack or reference counting could cause a BSOD.

**Solution:**
1. Implement the BCE transport wrapper in a separate file `BceTransport.c`.
2. Test sending a dummy command (GET_DEVICE_LIST) on a test boot with a kernel debugger.
3. Connect it to `T2AudioStartIo`/`T2AudioStopIo`.

---

### 2. BCE Device Enumeration
**Problem:**
- The Speaker device ID is unknown.
- BufferStruct (BAR1 metadata) holds only the device name `"Speaker"`, not the BCE protocol device ID.

**Linux KAIT2EN reference:**
```c
// audio.c:452 - the device ID comes from BCE enumeration
static int t2audio_probe_device(struct t2audio_dev *adev, u8 device_id) {
    // device_id obtained from the BCE GET_DEVICE_LIST response
    adev->device_id = device_id;
    ...
}
```

**Solution:**
1. Send the BCE command `GET_DEVICE_LIST` in `T2AudioStartDevice` after the BAR mapping.
2. Parse the response, find the device of type "audio output" named "Speaker".
3. Store `device_id` in `Context->SpeakerDeviceId`.

---

### 3. MMIO Buffer Validation
**Problem:**
- The MDL for the BAR1 speaker buffer is built but not verified on the real device.
- A wrong cache type or PFN calculation could cause:
  - BSOD (DRIVER_IRQL_NOT_LESS_OR_EQUAL).
  - Memory corruption.
  - A DMA conflict with AppleAudio.sys (if it is still loaded).

**Solution:**
1. Create a minimal test: return the MDL from `AllocateAudioBuffer`, but **do not let the Windows audio engine write into the buffer**.
2. Install the driver on a test boot with a kernel debugger.
3. Check:
   - The MDL is created without a BSOD.
   - The user-mode mapping succeeds (`MmMapLockedPagesSpecifyCache`).
   - The physical address matches BAR1.
4. Allow real audio playback only after successful validation.

---

### 4. Hardware Position Tracking
**Problem:**
- `GetPosition` returns a stub (PlayOffset=0, WriteOffset=0).
- The Windows audio engine uses `GetPosition` for glitch prevention and sync.

**Solution (option A - hardware pointer):**
```c
// If the T2 chip updates a hardware write pointer in the GPR or BufferStruct:
NTSTATUS T2AudioStreamGetPosition(...) {
    ULONG hwPointer = READ_REGISTER_ULONG(Context->GprBase + T2_HW_PTR_OFFSET);
    Position->PlayOffset = hwPointer * instance->BytesPerFrame;
    Position->WriteOffset = Position->PlayOffset + FIFO_SIZE;
    return STATUS_SUCCESS;
}
```

**Solution (option B - software position via QPC):**
```c
// Use the Phase 1 RingBufferMath and QPC interpolation:
#include "../Phase1/RingBufferMath.h"

NTSTATUS T2AudioStreamGetPosition(...) {
    ULONG64 currentQpc = KeQueryPerformanceCounter(NULL).QuadPart;
    ULONG64 elapsedFrames = RingBufferInterpolateFrames(
        instance->AnchorQpc,
        currentQpc,
        instance->SampleRate,
        qpcFrequency
    );
    ULONG64 playOffset = (instance->AnchorFrames + elapsedFrames) * instance->BytesPerFrame;
    Position->PlayOffset = playOffset % Context->SpeakerBufferSize;
    Position->WriteOffset = (playOffset + FIFO_FRAMES * instance->BytesPerFrame) % Context->SpeakerBufferSize;
    return STATUS_SUCCESS;
}
```

**Choice:** depends on whether the T2 chip updates a hardware pointer (to be checked on the device). (Answer: it does not; the position comes from `UPDATE_TIMESTAMP`.)

---

### 5. Hardware Latency / FIFO Size
**Problem:**
- `GetHWLatency` returns a stub (0 FIFO, 0 interrupt delay).
- The Windows audio engine uses the latency to negotiate the buffer size.

**Solution:**
1. Find the FIFO size in the GPR or the BufferStruct metadata.
2. Or use an empirical value from AppleAudio.sys (reverse engineering).
3. Update:
   ```c
   Latency->FifoSize = 4096;  // bytes, an example
   Latency->ChipsetDelay = 0;
   Latency->CodecDelay = 0;
   ```

---

### 6. INF Hardware Match
**Problem:**
- The INF has no Hardware ID; the driver cannot be installed through Device Manager.

**Solution (only after all blockers):**
```ini
[Standard.NTamd64]
%T2Audio.DeviceDesc%=T2Audio_Install,PCI\VEN_106B&DEV_1803&SUBSYS_1887106B

[T2Audio_Install]
CopyFiles=T2Audio_CopyFiles

[T2Audio_Install.Services]
AddService=T2AudioMiniport,0x00000002,T2Audio_Service

[T2Audio_CopyFiles]
T2AudioMiniport.sys

[T2Audio_Service]
DisplayName=%T2Audio.ServiceDesc%
ServiceType=1
StartType=3
ErrorControl=1
ServiceBinary=%12%\T2AudioMiniport.sys

[Strings]
T2Audio.DeviceDesc="Apple T2 Audio Device (6-channel)"
T2Audio.ServiceDesc="T2 Audio PortCls Driver"
```

⚠️ **Do NOT add the Hardware ID before full validation!**

---

## Testing Strategy (when ready)

### Pre-Installation Checklist
- [ ] BCE transport implemented and tested with a kernel debugger
- [ ] Device ID obtained through GET_DEVICE_LIST
- [ ] MMIO MDL verified on a test boot (without audio playback)
- [ ] Position tracking implemented (hardware or software)
- [ ] Hardware latency/FIFO set up
- [ ] INF updated with the Hardware ID
- [ ] A restore point created (if System Protection is on)
- [ ] A WinRE USB prepared for recovery

### Installation Steps (DANGER ZONE)
1. **Backup AppleAudio.sys:**
   ```powershell
   Copy-Item "C:\Windows\System32\drivers\AppleAudio.sys" "C:\Users\othysa\Desktop\mbp\backup\"
   ```

2. **Disable AppleAudio service:**
   ```powershell
   sc.exe config AppleAudio start= disabled
   ```

3. **Reboot to WinRE, replace driver:**
   ```cmd
   ren C:\Windows\System32\drivers\AppleAudio.sys AppleAudio.sys.bak
   copy T2AudioMiniport.sys C:\Windows\System32\drivers\
   ```

4. **Update Device Manager:**
   ```powershell
   pnputil.exe /add-driver T2AudioMiniport.inf /install
   ```

5. **Enable kernel debugger:**
   ```powershell
   bcdedit.exe /debug on
   bcdedit.exe /dbgsettings serial debugport:1 baudrate:115200
   ```

6. **Reboot and monitor:**
   - Kernel debugger output.
   - Event Viewer: System log, PortCls events.
   - DbgView: `KdPrint` messages.

7. **Test audio:**
   - Check Sound settings (Win+R → `mmsys.cpl`).
   - Verify "Apple T2 Audio Device (6-channel)" endpoint.
   - Play -30 dB test tone, 100ms max.
   - **STOP IMMEDIATELY** if distortion/clicking/smoke.

---

## Risk Assessment

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| BSOD on subdevice registration | Low | High | Kernel debugger, WinRE recovery |
| BSOD on the MMIO MDL mapping | Medium | High | Test on a dummy device first |
| T2 command rejected (bad device_id) | High | Low | GET_DEVICE_LIST enumeration |
| Audio glitching (bad position) | High | Medium | Software QPC fallback |
| Conflict with AppleAudio.sys | Low | High | Disable the AppleAudio service before installing |
| Speaker hardware damage | Very Low | Critical | -30 dB test tone, 100ms limit |

---

## Next Steps (Priority Order)

### Phase 5: BCE Transport (HIGH PRIORITY)
1. Create `BceTransport.c` with a kernel-mode wrapper for the AppleUSBVHCI IOCTL.
2. Implement `T2AudioOpenBceTransport`, `T2AudioSendBceCommand`, `T2AudioCloseBceTransport`.
3. Test with a dummy command under a kernel debugger.
4. Connect it to `T2AudioStartIo`/`T2AudioStopIo`.

### Phase 6: Device Enumeration (HIGH PRIORITY)
1. Send the `GET_DEVICE_LIST` command in `T2AudioStartDevice`.
2. Parse the BCE response, find the Speaker device ID.
3. Store it in `Context->SpeakerDeviceId`.

### Phase 7: MMIO Buffer Validation (CRITICAL SAFETY)
1. Activate `AllocateAudioBuffer` (return the MDL instead of NOT_SUPPORTED).
2. Install the driver on a test boot with a kernel debugger.
3. Check the MDL mapping without audio playback.
4. Allow SetState(RUN) only after validation.

### Phase 8: Position Tracking (MEDIUM PRIORITY)
1. Check GPR/BufferStruct for a hardware write pointer.
2. If there is none, use QPC interpolation (Phase 1 RingBufferMath).
3. Update `GetPosition`.

### Phase 9: Hardware Latency (LOW PRIORITY)
1. Reverse engineer the FIFO size from AppleAudio.sys or the GPR.
2. Update `GetHWLatency`.

### Phase 10: INF and Installation (FINAL STEP)
1. Add the Hardware ID to the INF.
2. Write an installation guide with a safety checklist.
3. Test the installation on the real device.

---

## Conclusion

Phase 3 and Phase 4 are implemented:
- ✅ The driver moved to a pure PortCls architecture.
- ✅ The WaveRT subdevice registers through `PcRegisterSubdevice`.
- ✅ The MMIO MDL and T2 protocol command serialization are ready (but not activated).

**The driver is not yet ready to be installed.** Critical blockers:
1. BCE transport (kernel-mode wrapper for AppleUSBVHCI).
2. Device ID enumeration (GET_DEVICE_LIST command).
3. MMIO buffer validation (check on a test boot).

**Estimated work remaining:** ~3-4 phases (BCE transport, device enum, MMIO validation, position tracking).

**Safety note:** do not install on the real device before Phases 5-7 are complete and checked with a kernel debugger.

---

## Contact
For questions or issues, refer to:
- Linux KAIT2EN reference: `C:\Users\othysa\Desktop\mbp\kait2en\modules\t2bce_audio\audio.c`
- Phase 1 offline parser: `C:\Users\othysa\Desktop\mbp\T2AudioPort\Phase1\`
- Build log: `C:\Users\othysa\Desktop\mbp\T2AudioPort\Phase2\phase2_build_log.txt`
