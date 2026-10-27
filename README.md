# T2AudioPort — Apple T2 Audio Driver for Windows (WaveRT)

> **Status (October 2026): experimental, diagnostic-session only — not usable as a daily audio driver.**
>
> * `START_IO` used to bugcheck the machine (`0x7E`). The cause has now been found statically: AppleUSBVHCI pushes bridgeOS messages to the audio device as `IOCTL 0x222020` without a FileObject, and those IRPs ended up in KS. The driver now intercepts that IOCTL, as AppleAudio does ([analysis](docs/analysis/START_IO_BSOD_ROOT_CAUSE.md)), but the fix has **not been verified on hardware**.
> * Windows sees a **stereo** endpoint; the driver lays it over the six speakers itself (woofers 0/1 and 3/4, tweeters 2/5 behind a 3 kHz high-pass). Apple's speaker EQ is not reproduced, so the first audible tests must be at **very low volume**.
> * The driver only starts inside a **volatile** `TestSession`, so a normal boot (or the boot after a crash) never activates it.
> * The current source has been unit-tested on a host and checked by CI, but **no hardware run has validated it yet**.

📘 **Full documentation (Russian): [`docs/DRIVER_GUIDE.md`](docs/DRIVER_GUIDE.md)** — hardware, BCE protocol, architecture, streaming path, safety model, build, hardware testing, log reference.

`T2AudioPort` (`T2AudioMiniport.sys`) is a PortCls / WaveRT miniport for the Apple T2 audio controller (`PCI\VEN_106B&DEV_1803`) on Intel Macs with a T2 chip (MacBookPro15,x, 16,1, 16,2, 16,4, iMac Pro, Mac mini 2018; hardware testing so far only on MacBookPro16,1). The goal is to drive the internal **6-channel speaker array** directly. Boot Camp's `AppleAudio.sys` only exposes a stereo endpoint.

---

## How it works

```
 Windows audio engine
        │  32-bit PCM, stereo, 48 kHz (WAVEFORMATEXTENSIBLE)
        ▼
 WaveRT cyclic buffer (system RAM, mapped to the engine)
        │  copy DPC + GetPosition: consumed frames → S24_LE,
        │  L → woofers 0,1 + tweeter 2 (3 kHz high-pass), R → 3,4 + 5;
        │  ~21 ms lead, kept on the T2 clock (UPDATE_TIMESTAMP)
        ▼
 BAR0 speaker ring (399,360 B = 16,640 frames, located via BufferStruct)
        │  read by bridgeOS once START_IO is sent
        ▼
 T2 audio DSP → speakers

 Control path: IOCTL 0x222018 on \Device\AppleUSBVHCI  (BCE messages:
 GET_DEVICE_LIST, GET_PROPERTY, GET_OUTPUT_STREAM_LIST,
 START_IO / STOP_IO)
```

* **Buffer discovery (`src/Device.c`).** The driver maps BAR0 (the ring memory) and finds the config BAR by its GPR signature `0x19870423` at `+0xC000`. It then walks the `BufferStruct` to the `"Speaker"` output buffer, bounds-checking everything against the mapped size. BAR0 is mapped write-combining when the BAR is prefetchable, as kaiT2en does.
* **Control (`src/BceTransport.c`, `src/Phase4.c`).** The speaker device is found by its UID. The driver then reads the stream's physical format (`'pft '`), latency (`'ltnc'`) and safety offset (`'saft'`) from the T2. Remote access is left to AppleUSBVHCI, which manages it itself.
* **Streaming (`src/WaveRTStream.c`).** PortCls cannot map BAR pages to user mode, so the engine writes to RAM and the driver mirrors it. The engine is only ever told a frame was played after that frame has been copied.
* **Formats (`src/WaveRTMiniport.c`).** The render pin offers stereo 48 kHz / 32-bit (`WAVEFORMATEXTENSIBLE`). Windows does not upmix stereo to 5.1, so the driver itself lays stereo over the six speakers (`src/T2AudioSpeakerLogic.h`, layout from kaiT2en's MacBookPro16,1 DSP profile): ring channels 0/1 and 3/4 are the left/right woofer pairs, 2/5 the tweeters, which get a 4th-order 3 kHz high-pass (`Parameters\TweeterMode=0` mutes them). Apple's speaker EQ is not reproduced; Equalizer APO works on the stereo endpoint. `TestSession\RawSpeakerChannels=1` exposes the raw 6 channels for per-speaker diagnostics (no tweeter protection).
* **Clock.** bridgeOS sends `UPDATE_TIMESTAMP` (via IOCTL `0x222020`, handled by `T2AudioDeviceControl`) with the T2 read position and host QPC time. As AppleAudio does, the driver keeps its write position 1000 frames ahead of it and re-aims when the lead drifts out of ±500 frames. Analysis: `docs/analysis/RE_speaker_layout_and_clock.md`.

### Safety gates

| Gate | Registry value (`HKLM\SYSTEM\CurrentControlSet\Services\T2AudioMiniport\TestSession`, **volatile**) | Default |
|---|---|---|
| Start at all | `EnableTestMode=1` | off → Code 10 |
| Stage | `DiagStage` = 1 RAM only · 2 map BARs · 3 BCE discovery + stream format · 4 wire speaker (+ `START_IO` with `EnableBceIo`) · 5 MMIO copy | 1 |
| Write PCM into BAR0 | `EnableMmioCopy=1` (stage 5) **and** a supported `'pft '` format read from the T2 | off |
| Send `START_IO`/`STOP_IO` | `EnableBceIo=1` (stage 4+) | off |

Safe Mode never starts the driver. `tools/T2Audio-TestAgent.ps1` creates the session (`-Stage N`, `-Mmio`, `-BceIo`, `-RawChannels`). The agent still refuses `-BceIo` until the `0x222020` fix has been confirmed on hardware.

Other options: `Parameters\TweeterMode` (1 = high-passed tweeters, default; 0 = woofers only) and `TestSession\RawSpeakerChannels=1` (raw 6-channel endpoint for per-speaker checks — **no tweeter protection**).

---

## Parity with kaiT2en (Linux `t2bce_audio`)

[kaiT2en](https://github.com/kaiT2en/KaiT2en-Fedora) (`modules/t2bce_audio`) is the reference implementation. Here is what this driver does the same way, and what it cannot do yet:

| kaiT2en behaviour | Here | Notes |
|---|---|---|
| BufferStruct via GPR `+0xC000` (version, signature, offset) | ✅ | |
| Speaker found by UID, not by index | ✅ | |
| `'pft '` → sample layout (`S24_LE` for the speaker: flags `0x4`, 24 bits in a 4-byte container) | ✅ new | Samples are shifted `>> 8` while copying. The earlier raw 32-bit copy put the wrong bytes in front of the DSP. MMIO copy stays off if the format cannot be read or is unsupported. |
| Stream + device latency, safety offset | ✅ new | Reported through `GetHWLatency`. |
| Ring zeroed before a run; clock anchored **before** `START_IO` | ✅ new | |
| `pci_iomap_wc` for a prefetchable BAR | ✅ new | WC mapping + `sfence` after each copy. |
| `SET_REMOTE_ACCESS(ON)` before I/O, `OFF` on quiesce | n/a on Windows | AppleUSBVHCI sends it itself (`mode 1` at init/resume, `0` at power-down) and AppleAudio never does, so this driver no longer sends it ([RE](docs/analysis/RE_appleaudio_protocol.md)). |
| `NOTIFICATION_ALIVE` handshake before commands | n/a on Windows | AppleUSBVHCI sends it itself at init/resume. |
| Receiving T2→host messages | ✅ new | AppleUSBVHCI delivers them as `IOCTL 0x222020` to the audio device; `T2AudioDeviceControl` handles it (this was the `START_IO` bugcheck). |
| Ring position from `UPDATE_TIMESTAMP` (sampleTime + hostTime, one per ring pass) | ✅ new | The ring write position is kept 1000 frames ahead of the T2 read position and re-aimed outside ±500 (AppleAudio's model). hostTime is host QPC ns; an implausible one falls back to the receive time, and timestamps from before this run's `START_IO` are dropped (as kaiT2en `6128f86`). |
| Speaker DSP (EQ, crossovers, XTC) in PipeWire, `dsp/profiles/16_1` | partial | The channel layout and the need to high-pass the tweeters come from that profile; only a 4th-order tweeter high-pass is applied. Equalizer APO can run on the stereo endpoint. |
| Zeroing a stalled output ring (writer stopped across two timestamps) | ❌ | Not needed while the copy runs from QPC; revisit if stale loops are heard. |
| Exposing the bridge ring itself as the PCM buffer | ❌ | PortCls refuses BAR MDLs for WaveRT (0x8007001F), hence the RAM mirror. |
| Sleep: STOP_IO before D3, re-init after resume | ❌ | AppleAudio stops I/O in `QueryPowerChangeState(D3)` and re-registers only input buffers on D0 ([R8](docs/analysis/RE_appleaudio_protocol.md)); there is no sleep marker on Windows. This driver has no power interface yet — do not sleep the machine during a stage-4+ session. |
| Jack detection, mic / codec input, volume properties | ❌ | Speaker output only. |

---

## Building

### CI
[`.github/workflows/build.yml`](.github/workflows/build.yml) runs on every push and pull request. It is modeled on [osaul4ik/apple-t2-touchid-windows](https://github.com/osaul4ik/apple-t2-touchid-windows).
* **Windows (windows-2022):** installs the WDK, builds `src/T2AudioMiniport.vcxproj` (Release x64), builds and runs the host tests with MSVC, and uploads the **unsigned** `.sys`.
* **Linux:** builds and runs the host tests with gcc and clang using `-Werror`.

### Locally
```powershell
msbuild src\T2AudioMiniport.vcxproj /p:Configuration=Release /p:Platform=x64 /t:Rebuild
# -> src\bin\Release\T2AudioMiniport.sys  (test-sign before installing)

cl /nologo /W4 /Fe:tests\BceProtocolLogicTest.exe tests\BceProtocolLogicTest.c
cl /nologo /W4 /Fe:tests\T2AudioBufferLogicTest.exe tests\T2AudioBufferLogicTest.c
cl /nologo /W4 /Fe:tests\T2AudioSpeakerLogicTest.exe tests\T2AudioSpeakerLogicTest.c
tests\BceProtocolLogicTest.exe
tests\T2AudioBufferLogicTest.exe
tests\T2AudioSpeakerLogicTest.exe
```
The tests compile the same headers the driver uses (`src/BceProtocolLogic.h`, `src/T2AudioBufferLogic.h`, `src/T2AudioSpeakerLogic.h`). They cover BCE request tags and reply parsing, incoming `UPDATE_TIMESTAMP`, the `'pft '` descriptor captured on a MacBookPro16,1, ring/copy arithmetic, S24 conversion, the stereo → six-speaker layout, the tweeter filter's response and the T2 clock correction.

---

## Installing and testing on hardware

**Read [`docs/CURRENT_STATE.md`](docs/CURRENT_STATE.md) first.** Keep `AppleAudio.sys` restorable: `tools/Restore-AppleAudioDriver.ps1` and `tools/Rollback-T2AudioDriver.ps1` are provided.

1. Enable test signing and sign the package (`packaging/`).
2. Install with `tools/Install-T2AudioDriver.ps1`.
3. Run a staged session with `tools/T2Audio-TestAgent.ps1 -Stage 3` (discovery and stream format only, no audio I/O) and capture the log with `tools/Capture-T2AudioLog.ps1`.
4. Before any stage-5 run, the stage-3 log must show `pft: rate=48000 ... ch=6 bits=24` and `sample shift=8`.
5. After updating from an older 6-channel build, Windows may keep the cached 6-channel endpoint format, which this driver rejects (`Validate: 6 channels not offered`). Select 2 channels / 32-bit / 48 kHz in the device's Advanced properties, or reinstall the device.
6. First audible run: stereo, volume 1–5 %. Check the speaker order only with `-RawChannels`, with a quiet tone of 1 kHz or above, one channel at a time.

---

## Project structure

* `src/` — PortCls WaveRT + topology miniports, BAR/BufferStruct discovery, BCE transport, stream mirror.
* `tests/` — host unit tests for the pure protocol and buffer helpers.
* `packaging/` — INF/CAT for test installs.
* `tools/` — install, rollback, staged test agent, log capture, WASAPI tone tests.
* `docs/` — [`DRIVER_GUIDE.md`](docs/DRIVER_GUIDE.md) (detailed guide), [`CURRENT_STATE.md`](docs/CURRENT_STATE.md) (authoritative status), [`AGENT_HANDOVER_GUIDE.md`](docs/AGENT_HANDOVER_GUIDE.md), [`INDEX.md`](docs/INDEX.md), reverse-engineering notes in `docs/analysis/`.

## Roadmap

1. ~~Root cause of the `START_IO` bugcheck~~ — found statically (`0x222020`), handler added; confirm on hardware.
2. ~~Receive path for T2 notifications / hardware clock~~ — done (`UPDATE_TIMESTAMP`), confirm on hardware.
3. First sound: stage 5 + `START_IO` at low volume; confirm the speaker order (`-RawChannels`).
4. Power management (`IAdapterPowerManagement`: STOP_IO before D3).
5. Optional: closer speaker DSP (EQ/crossover per kaiT2en `dsp/profiles/16_1`), hardware volume (msg 28 controls), jack/headphone output, microphone input.

## Attributions

* **kaiT2en** — [kaiT2en/KaiT2en-Fedora](https://github.com/kaiT2en/KaiT2en-Fedora) `modules/t2bce_audio` (GPL-2.0), and the earlier [kekrby/linux-t2](https://github.com/kekrby/linux-t2) work. The protocol constants, BufferStruct layout, init/quiesce sequence and timestamp handling come from there. The MacBookPro16,1 speaker channel layout was read from its DSP profile (`dsp/profiles/16_1`, © André Eikmeyer, GPL-3.0-or-later); no filter coefficients or FIR data were copied. The code here is an independent Windows implementation; see [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md).
* **osaul4ik** — [apple-t2-touchid-windows](https://github.com/osaul4ik/apple-t2-touchid-windows) (GPL-2.0). The WDK CI workflow design comes from there.
* **Apple Boot Camp** — `AppleAudio.sys` / `AppleUSBVHCI.sys`, analyzed for the transport protocol.
