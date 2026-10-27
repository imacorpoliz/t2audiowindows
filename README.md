# T2AudioPort — Apple T2 Audio Driver for Windows (WaveRT)

> **Status (October 2026):** **Experimental, diagnostic-session only.**  
> The driver has been audited against the Linux reference (`kaiT2en`) and REA reverse-engineering analysis of `AppleAudio.sys`.  
> The BSOD `0x7E` on `START_IO` is **not proven fixed**: both the `FileObject = NULL` and the `FileObject` = transport variants crashed on hardware on 2026-10-08 (see `docs/CURRENT_STATE.md`). No hardware run has validated the current source.  
> The driver still starts **only** inside a volatile `TestSession` (see below). Host unit tests pass with 0 failures.

---

## Overview

`T2AudioPort` (`T2AudioMiniport.sys`) is an open-source Windows Kernel-Mode WDM / WaveRT audio miniport driver for the Apple T2 audio coprocessor (`PCI\VEN_106B&DEV_1803`) on Apple Intel Macs (MacBookPro16,1, MacBookPro15,x, 16,2, 16,4, iMac Pro, Mac mini 2018).

It interfaces directly with:
1. **BAR0/BAR1 Shared Memory (`BufferStruct`)** — for hardware audio buffers and device discovery.
2. **AppleUSBVHCI IPC Gateway (`IOCTL 0x222018`)** — for BridgeOS Mailbox communication (device enumeration, `SET_REMOTE_ACCESS`, `START_IO`, `STOP_IO`).

For complete developer onboarding and hardware details, see:
👉 **[`docs/AGENT_HANDOVER_GUIDE.md`](docs/AGENT_HANDOVER_GUIDE.md)**

---

## Key Breakthroughs & Resolved Defects

### 1. BSOD `0x7E` in `AppleUSBVHCI.sys` / `ksthunk` (hypothesis, unverified)
* **Root Cause:** In earlier builds, assigning `FileObject` to `IoGetNextIrpStackLocation(irp)->FileObject` caused `AppleUSBVHCI.sys` to delegate the IRP to `ksthunk!DispatchDeviceIoControl`, which accessed `FileObject->FsContext` without a NULL guard on a system worker thread, triggering bugcheck `0x7E`.
* **Current source:** Disassembly of official `AppleAudio.sys` via REA shows that it leaves `FileObject = NULL` in `IoBuildDeviceIoControlRequest`, and `BceTransport.c` does the same. **This is not a validated fix:** the 2026-10-08 01:35 crash happened with `FileObject = NULL`, and the 01:48 crash with `FileObject` set; both dumps are identical. The forwarded-IRP path inside `AppleUSBVHCI` remains the open root cause.

### 2. Circular DMA Ring Buffer Mirroring (Fix for 347 ms Stutter / Silence)
* **Root Cause:** T2 hardware plays speaker audio from a fixed 399,360-byte (`0x61800` = 16,640 frames $\times$ 24 bytes) cyclic buffer in BAR1, completing a full cycle every 346.6 ms at 48 kHz. Earlier builds repeatedly copied client PCM to offset 0, causing 10 ms of sound followed by 336 ms of silence/clicks.
* **Resolution:** [`src/WaveRTStream.c`](src/WaveRTStream.c) mirrors the client buffer into the BAR1 ring with `T2AudioCalculateCopyChunk` ([`src/T2AudioBufferLogic.h`](src/T2AudioBufferLogic.h)). Each source frame is copied once it has been consumed at the sample rate and written `T2AUDIO_LEAD_FRAMES` (720 = 15 ms) ahead in the hardware ring. The play position reported to the audio engine is the consumed (already copied) cursor, and `GetPosition` advances the copy itself, so the engine can never overwrite a frame before it was mirrored. (The 2026-10-28 version copied 15 ms *ahead* of the reported position, i.e. frames the client had not written yet, from a 10 ms client buffer.)

### 3. Startup gate (unchanged, intentional)
* `Driver.c` still refuses to start (Code 10) unless the **volatile** `TestSession\EnableTestMode=1` key exists; stage, MMIO copy and BCE I/O are opt-in through `DiagStage`, `EnableMmioCopy` and `EnableBceIo`. Earlier versions of this README claimed a "Normal Operational Mode" default; that change was never in the source, and with the `0x7E` bugcheck unresolved it is deliberately not enabled, so a crash cannot turn into a boot loop.

### 4. BCE Response Opcode Alignment
* **Root Cause:** `Phase4.c` expected response message IDs 0 and 2, but BridgeOS returns `START_IO_RESPONSE (1)` and `STOP_IO_RESPONSE (3)`, triggering `STATUS_DEVICE_PROTOCOL_ERROR`.
* **Resolution:** Aligned opcodes in `src/BceProtocolLogic.h` and verified via unit tests.

### 5. Multi-Channel `WAVEFORMATEXTENSIBLE`
* **Root Cause:** Windows Audio Engine (WASAPI) rejected formats with >2 channels when exposed as basic `WAVE_FORMAT_PCM`.
* **Resolution:** `DataRangeIntersection` in `src/WaveRTMiniport.c` now returns a `KSDATAFORMAT_WAVEFORMATEXTENSIBLE` (6 channels, 48 kHz, 32-bit, 5.1 Surround mask) instead of deferring to the PortCls default; format validation accepts both 5.1 masks (`0x60F` and `0x3F`).

### 6. Hardware Buffer Zeroing on Stop
* BAR1 speaker buffer memory is zeroed (`RtlZeroMemory`) on `KSSTATE_STOP` in `WaveRTStream.c` to prevent residual audio loops and buzzing.

### 7. Stream State Synchronization
* Synchronized `AnchorQpc`, `AnchorFrames`, and stream state between the 2 ms copy DPC and `SetState` using a `KSPIN_LOCK`.

---

## Building and Testing

### 1. Compiling the Driver
```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' `
  'src\T2AudioMiniport.vcxproj' /p:Configuration=Release /p:Platform=x64 /t:Rebuild
```
Output: `src/bin/Release/T2AudioMiniport.sys`

### 2. Running Host Unit Tests
```powershell
# Compile and run protocol & buffer logic tests
cl /nologo /W4 /Fe:tests\BceProtocolLogicTest.exe tests\BceProtocolLogicTest.c
cl /nologo /W4 /Fe:tests\T2AudioBufferLogicTest.exe tests\T2AudioBufferLogicTest.c

& 'tests\BceProtocolLogicTest.exe'
& 'tests\T2AudioBufferLogicTest.exe'
```

---

## Project Structure

* **`src/`**: PortCls WaveRT miniport, DMA circular buffer, BCE transport, PnP device logic.
* **`tests/`**: Host-side unit tests for protocol serialization and circular DMA math.
* **`packaging/`**: INF, CDF, and CAT packaging files for driver deployment.
* **`tools/`**: Diagnostic, test signal, and validation scripts (`Test-WasapiTone.ps1`, `Set-DefaultAudioDevice.ps1`).
* **`docs/`**: Comprehensive documentation, including:
  * [`docs/AGENT_HANDOVER_GUIDE.md`](docs/AGENT_HANDOVER_GUIDE.md) — Master developer and AI agent handover guide.
  * [`docs/INDEX.md`](docs/INDEX.md) — Documentation index.
  * [`docs/CURRENT_STATE.md`](docs/CURRENT_STATE.md) — Detailed development log and state.
  * [`docs/analysis/`](docs/analysis/) — REA reverse-engineering reports and disassembly analysis of `AppleAudio.sys`.

---

## Attributions & References

* **[kaiT2en](https://github.com/kaiT2en)**: Reference Linux driver for Apple T2 audio (`t2bce_audio`).
* **Apple Bootcamp**: Official `AppleAudio.sys` analyzed for hardware registers and transport protocol.
