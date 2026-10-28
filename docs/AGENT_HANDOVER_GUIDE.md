# T2AudioPort: guide for developers and agents (AGENT_HANDOVER_GUIDE.md)

> **Document status:** written in early October 2026, corrected on 28 October 2026. Parts of it describe the state before the hardware stages; on any conflict [`DRIVER_GUIDE.md`](DRIVER_GUIDE.md) and [`CURRENT_STATE.md`](CURRENT_STATE.md) win.  
> **Audience:** AI agents, reverse engineers and Windows kernel driver developers.  
> **Target hardware:** MacBook Pro 16,1 (2019), verified. Other T2 Macs (MacBook Pro 15,1 / 15,2 / 16,2 / 16,4, iMac Pro, Mac mini 2018) share the PCI function `VEN_106B&DEV_1803` but are **not supported yet**: their speaker layout and DSP profile differ (see `ROADMAP.md`).

---

## 1. Introduction and project context

The workspace (the private `imacorpoliz/mbp`, of which this repository is the `T2AudioPort\` part) contains the code, the reverse-engineering results and the documentation for sound on Apple T2 computers running Windows 10/11 x64.

### System components
1. **`T2AudioPort\`**: the open WDM / WaveRT kernel audio driver (`T2AudioMiniport.sys`) that talks to the T2 chip and the `AppleUSBVHCI.sys` bridge.
2. **`SpeakerDSP-APO\`**: a user-mode DSP system (the VST2 plugin `bankstown.dll` and the `generate_config.py` configurator for Equalizer APO) implementing crossovers, equalisation and delays for the 6 physical speakers of the MacBook Pro 16,1. Superseded by the in-driver DSP (`src/T2AudioDsp.h`); do not use both.
3. **`MBP-Audio-driver\`**: Apple's official Boot Camp driver (`AppleAudio.sys`), installed on the physical machine as the reference.
4. **`kait2en\`**: the full working Linux driver (`t2bce_audio`), used as the architectural reference for talking to the T2 BCE mailbox and BAR0.

---

## 2. Hardware architecture and protocol

### 2.1 PCI topology and the T2 register map
The Apple T2 audio controller appears on the PCI bus as:
```text
PCI\VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01   (test MacBookPro16,1)
```

The controller has these memory regions (BARs):
- **BAR0 (shared memory / `BufferStruct`)**:
  - Memory shared between the host (Intel CPU) and the Apple T2 coprocessor (ARM bridgeOS).
  - The offset of `BufferStruct` is given by register `GPR[2]`.
  - Header signature: `0x19870423` (`T2AUDIO_SIG`).
  - Holds the device descriptor table `T2AUDIO_DEVICE_METADATA` with a fixed stride of `0xBDEC` bytes.
  - The speaker device is identified by the name `"Speaker"`.
  - The speakers' output stream (`OutputStreams[0]`) has a hardware ring of **399 360 bytes (`0x61800` = 16 640 frames × 24 bytes)**.
- **BAR4 (or BAR2) + `0xC000` (General Purpose Registers / GPR)**:
  - `GPR[0]` (+0x00): GPR protocol version (must be ≥ 2).
  - `GPR[1]` (+0x04): GPR signature (exactly `0x19870423`).
  - `GPR[2]` (+0x08): offset of `BufferStruct` inside BAR0.

### 2.2 BCE message transport
On Windows the T2 firmware is reached through the `AppleUSBVHCI.sys` driver (`\Device\AppleUSBVHCI`):
- **IOCTL code**: **`0x222018`** for requests (`CTL_CODE(FILE_DEVICE_UNKNOWN, 0x0806, METHOD_BUFFERED, FILE_ANY_ACCESS)`). bridgeOS → host messages arrive as **`0x222020`** sent by AppleUSBVHCI to the audio driver, which must intercept them (the BSOD 0x7E fix).
- **BCE packet format**:
  ```c
  #pragma pack(push, 1)
  typedef struct _T2AUDIO_MSG_HEADER {
      ULONG Tag;          // request counter 1..127 (as in AppleAudio)
      UCHAR Type;         // 1 = COMMAND, 2 = RESPONSE, 3 = NOTIFICATION
      ULONG64 DeviceId;   // T2 sub-device id (0 for global commands)
  } T2AUDIO_MSG_HEADER;   // size: 13 bytes

  typedef struct _T2AUDIO_MSG_BASE {
      ULONG Message;      // command code / opcode
      ULONG Status;       // protocol status (0 = SUCCESS)
  } T2AUDIO_MSG_BASE;     // size: 8 bytes
  #pragma pack(pop)
  ```

---

## 3. Matrix of fixed critical defects

All defects found and fixed during the audit and the reverse engineering:

| # | Defect | Source file | Symptom / consequence | Cause and fix |
|---|---|---|---|---|
| **1** | Wrong `START_IO` / `STOP_IO` reply codes | [`Phase4.c`](../src/Phase4.c) | `STATUS_DEVICE_PROTOCOL_ERROR` when enabling sound | The driver expected replies `0` and `2`. REA proved that the T2 returns `1` (`START_IO_RESPONSE`) and `3` (`STOP_IO_RESPONSE`). Correct macros added in [`BceProtocolLogic.h`](../src/BceProtocolLogic.h). Verified on hardware (Stage D). |
| **2** | BSOD `0x7E` in `ks!DispatchDeviceIoControl` | [`BceTransport.c`](../src/BceTransport.c), [`Driver.c`](../src/Driver.c) | Immediate blue screen on `START_IO` | First attempt: removed the manual `irpStack->FileObject = FileObject` (in `AppleAudio.sys`, `FUN_140004a4c`, `FileObject` is `NULL`). That did **not** fix it (crash on 2026-10-08 01:35). Real cause: AppleUSBVHCI delivers bridgeOS messages as `IOCTL 0x222020` without a `FileObject` to the first Apple audio interface, and PortCls/KS dereferenced NULL. Fix: intercept `0x222020` in `T2AudioDeviceControl`. **Proven on hardware** (Stage D). See `analysis/START_IO_BSOD_ROOT_CAUSE.md`. |
| **3** | Windows Audio Engine refused to create the stream | [`WaveRTMiniport.c`](../src/WaveRTMiniport.c) | Error `0x80070057` / WASAPI refused to open pin 0 | The driver required `WAVE_FORMAT_PCM`. For streams of more than 2 channels the Windows Audio Engine requires `WAVEFORMATEXTENSIBLE` (`0xFFFE`). Full `KSDATAFORMAT_WAVEFORMATEXTENSIBLE` support added. (The endpoint is now stereo; 6 channels only in raw test mode.) |
| **4** | Static copy to offset 0 (silence / clicks) | [`WaveRTStream.c`](../src/WaveRTStream.c) | Sound vanished after 10 ms; a click every 347 ms | The driver copied 11.5 KB to offset 0 of the 399 KB buffer, and the T2 cyclically read 336 ms of silence. Fixed by `T2AudioCalculateCopyChunk`, which fills the ring cyclically ahead of the T2 cursor (lead 1000 frames, kept by the T2 clock). |
| **5** | Start refused on an ordinary boot (Code 10) | [`Driver.c`](../src/Driver.c) | `STATUS_UNSUCCESSFUL` without a helper script | The start is gated by the volatile `TestSession` key, deliberately (`DRIVER_GUIDE.md` §8). Since 2026-10-28 the installer's persistent mode (`Parameters\EnableProduction`, with the crash guard) starts the driver on every boot. |
| **6** | Race in the DPC timer | [`WaveRTStream.c`](../src/WaveRTStream.c) | Corrupted play cursor, desynchronisation | The 2 ms timer DPC and `SetState` read `AnchorQpc` and `AnchorFrames` without synchronisation. Added the `KSPIN_LOCK StreamLock`. |
| **7** | Looping noise after the stream stopped | [`WaveRTStream.c`](../src/WaveRTStream.c) | Buzz/hum after Pause/Stop | The ring was not zeroed on stop. The speaker ring is now zeroed on the transition to `KSSTATE_STOP`. |
| **8** | Dangerous `KeFlushQueuedDpcs()` at raised IRQL | [`WaveRTStream.c`](../src/WaveRTStream.c) | `BugCheck 0x4A` when stopping the timer | **Correction:** there is no such check in the code; `KeFlushQueuedDpcs()` is called only from PASSIVE callbacks (`SetState`, `FreeAudioBuffer`, teardown), which is what makes it correct. |

---

## 4. Cyclic DMA ring mirroring

### 4.1 The PortCls and system buffer problem
Windows PortCls (`PortWaveRT`) maps the buffer into the user process with `MmMapLockedPagesSpecifyCache(..., UserMode, ...)`. Mapping the physical pages of BAR0 into user space that way is rejected with `0x8007001F`.

### 4.2 The implemented solution
1. **System cyclic buffer (`SystemBufferVa`)**:
   The driver allocates ordinary RAM pages through `PortStream->AllocatePagesForMdl` of the client's requested size (usually 10–20 ms = 11 520 or 23 040 bytes).
2. **T2 hardware ring (`context->Bar1Mapped + context->SpeakerBufferOffset`)**:
   Size $S_{\text{hw}} = 399\,360$ bytes.
3. **The `T2AudioCalculateCopyChunk` algorithm**:
   The DPC timer (2 ms) and `GetPosition` compute the current client frame $F_{\text{play}}$ from QPC and copy all frames from $F_{\text{last}}$ up to it in linear chunks. Frame $F$ lands in ring frame $F + \text{DestOffset}$: a lead of ~1000 frames (21 ms) over the T2 read position, re-aimed from `UPDATE_TIMESTAMP` (window ±500):
   $$\text{SrcOffset} = (F \times B_{\text{client}}) \pmod{S_{\text{client}}},\quad B_{\text{client}} = 8\ \text{(stereo)}$$
   $$\text{DstOffset} = ((F + \text{DestOffset}) \times 24) \pmod{S_{\text{hw}}}$$
   Stereo is spread over the 6 speakers: by the DSP (`SpeakerDsp ≥ 1`, default 3) or, in the plain path, L → 0, 1 and through a HPF into 2; R → 3, 4 and through a HPF into 5 (see `docs/analysis/RE_speaker_layout_and_clock.md`).
   Wrap-around at the boundaries of both buffers is seamless.

---

## 5. Build, test and debugging instructions

### 5.1 Building the kernel driver (`T2AudioMiniport.sys`)
Built with the standard Visual Studio / MSBuild toolchain and the Windows Driver Kit (paths below are the owner's build machine; adjust):
```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
    "<repo>\src\T2AudioMiniport.vcxproj" `
    /p:Configuration=Release /p:Platform=x64 /t:Rebuild
```
The driver binary is produced at:
`<repo>\src\bin\Release\T2AudioMiniport.sys`

### 5.2 Building and running the unit tests
The unit tests do not depend on the kernel and are built with the ordinary MSVC compiler `cl.exe`:
```cmd
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d "<repo>"

for %t in (BceProtocolLogicTest T2AudioBufferLogicTest T2AudioSpeakerLogicTest T2AudioPipelineTest T2AudioDspTest T2AudioCrashGuardTest) do (
  cl /nologo /W4 /Fe:tests\%t.exe tests\%t.c && tests\%t.exe
)
```
Every test must end with `ALL PASSED`.

### 5.3 Safe testing on real hardware
1. **Test signing**: test signing must be on in Windows (`bcdedit /set testsigning on`).
2. **Emergency rollback**: `tools/T2Audio-Setup.ps1 -Action Uninstall`, or the proven [`tools/Restore-AppleAudioDriver.ps1`](../tools/Restore-AppleAudioDriver.ps1), which restores the original `AppleAudio.sys`.
3. **Debug output**: run DebugView (`..\DebugView\Dbgview.exe` in the `mbp` layout) with *Capture Kernel* on, and filter on the `T2Audio:` prefix.

---

## 6. Links and navigation

- **AppleAudio protocol analysis**: [`docs/analysis/AppleAudio_Reverse_Engineering_Report.md`](analysis/AppleAudio_Reverse_Engineering_Report.md)
- **Index of all reports**: [`docs/INDEX.md`](INDEX.md)
- **Project status**: [`CURRENT_STATE.md`](CURRENT_STATE.md); plan: [`ROADMAP.md`](ROADMAP.md)
- **Driver source**:
  - BCE transport: [`src/BceTransport.c`](../src/BceTransport.c)
  - Start/Stop commands: [`src/Phase4.c`](../src/Phase4.c)
  - WaveRT stream and DMA timer: [`src/WaveRTStream.c`](../src/WaveRTStream.c)
  - Speaker DSP: [`src/T2AudioDsp.h`](../src/T2AudioDsp.h)
  - Miniport and format validation: [`src/WaveRTMiniport.c`](../src/WaveRTMiniport.c)
  - PnP initialisation and dispatch: [`src/Driver.c`](../src/Driver.c)
  - BAR mapping and speaker lookup: [`src/Device.c`](../src/Device.c)
