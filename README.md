# T2AudioPort — Apple T2 Audio Driver for Windows (WaveRT)

> **Status (October 2026): experimental, diagnostic-session only — not usable as a daily audio driver.**
>
> * `START_IO` used to bugcheck the machine (`0x7E`). The cause has now been found statically: AppleUSBVHCI pushes bridgeOS messages to the audio device as `IOCTL 0x222020` without a FileObject, and those IRPs ended up in KS. The driver now intercepts that IOCTL, as AppleAudio does ([analysis](docs/analysis/START_IO_BSOD_ROOT_CAUSE.md)), but the fix has **not been verified on hardware**.
> * The driver only starts inside a **volatile** `TestSession`, so a normal boot (or the boot after a crash) never activates it.
> * The current source has been unit-tested on a host and checked by CI, but **no hardware run has validated it yet**.

📘 **Full documentation (Russian): [`docs/DRIVER_GUIDE.md`](docs/DRIVER_GUIDE.md)** — hardware, BCE protocol, architecture, streaming path, safety model, build, hardware testing, log reference.

`T2AudioPort` (`T2AudioMiniport.sys`) is a PortCls / WaveRT miniport for the Apple T2 audio controller (`PCI\VEN_106B&DEV_1803`) on Intel Macs with a T2 chip (MacBookPro15,x, 16,1, 16,2, 16,4, iMac Pro, Mac mini 2018; hardware testing so far only on MacBookPro16,1). The goal is to drive the internal **6-channel speaker array** directly. Boot Camp's `AppleAudio.sys` only exposes a stereo endpoint.

---

## How it works

```
 Windows audio engine
        │  32-bit PCM, 6 ch, 48 kHz (WAVEFORMATEXTENSIBLE 5.1)
        ▼
 WaveRT cyclic buffer (system RAM, mapped to the engine)
        │  copy DPC + GetPosition: consumed frames → S24_LE, +15 ms lead
        ▼
 BAR0 speaker ring (399,360 B = 16,640 frames, located via BufferStruct)
        │  read by bridgeOS once START_IO is sent
        ▼
 T2 audio DSP → speakers

 Control path: IOCTL 0x222018 on \Device\AppleUSBVHCI  (BCE "Audt" messages:
 GET_DEVICE_LIST, GET_PROPERTY, GET_OUTPUT_STREAM_LIST, SET_REMOTE_ACCESS,
 START_IO / STOP_IO)
```

* **Buffer discovery (`src/Device.c`).** The driver maps BAR0 (the ring memory) and finds the config BAR by its GPR signature `0x19870423` at `+0xC000`. It then walks the `BufferStruct` to the `"Speaker"` output buffer, bounds-checking everything against the mapped size. BAR0 is mapped write-combining when the BAR is prefetchable, as kaiT2en does.
* **Control (`src/BceTransport.c`, `src/Phase4.c`).** The speaker device is found by its UID. The driver then reads the stream's physical format (`'pft '`), latency (`'ltnc'`) and safety offset (`'saft'`) from the T2. It takes host control with `SET_REMOTE_ACCESS(ON)` and returns it on stop/remove.
* **Streaming (`src/WaveRTStream.c`).** PortCls cannot map BAR pages to user mode, so the engine writes to RAM and the driver mirrors it. The engine is only ever told a frame was played after that frame has been copied.
* **Formats (`src/WaveRTMiniport.c`).** The render pin offers a single format, 6 ch / 48 kHz / 32-bit. It is negotiated as `WAVEFORMATEXTENSIBLE` with a 5.1 mask.

### Safety gates

| Gate | Registry value (`HKLM\SYSTEM\CurrentControlSet\Services\T2AudioMiniport\TestSession`, **volatile**) | Default |
|---|---|---|
| Start at all | `EnableTestMode=1` | off → Code 10 |
| Stage | `DiagStage` = 1 RAM only · 2 map BARs · 3 BCE discovery + stream format · 4 wire speaker, `SET_REMOTE_ACCESS` · 5 MMIO copy | 1 |
| Write PCM into BAR0 | `EnableMmioCopy=1` (stage 5) **and** a supported `'pft '` format read from the T2 | off |
| Send `START_IO`/`STOP_IO` | `EnableBceIo=1` (stage 4+) | off |

Safe Mode never starts the driver. `tools/T2Audio-TestAgent.ps1` creates the session (`-Stage N`, `-Mmio`, `-BceIo`). The agent currently refuses `-BceIo` because of the open BSOD.

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
| `SET_REMOTE_ACCESS(ON)` before I/O, `OFF` on quiesce | ✅ (OFF new) | OFF is sent on PnP stop/remove. |
| `NOTIFICATION_ALIVE` handshake before commands | ❌ | Needs a receive path for T2→host messages through `AppleUSBVHCI`, which is not known yet. |
| Hardware position from `UPDATE_TIMESTAMP` (sampleTime/hostTime) | ❌ | Same blocker. The position is extrapolated from QPC since the anchor, which can drift against the T2 clock over long runs. |
| Exposing the bridge ring itself as the PCM buffer | ❌ | PortCls refuses BAR MDLs for WaveRT (0x8007001F), hence the RAM mirror. |
| Sleep: remote access OFF in `prepare`, GPR sleep marker `0x19770525` at `+0x10`, buffer reset on resume | ❌ | Power handling is not implemented. kaiT2en notes that bridgeOS stops answering if remote access is still ON while VHCI suspends, so do not sleep the machine during a stage-4+ session. |
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
tests\BceProtocolLogicTest.exe
tests\T2AudioBufferLogicTest.exe
```
The tests compile the same headers the driver uses (`src/BceProtocolLogic.h`, `src/T2AudioBufferLogic.h`). They cover BCE reply parsing, the `'pft '` descriptor captured on a MacBookPro16,1, ring/copy arithmetic and S24 sample conversion.

---

## Installing and testing on hardware

**Read [`docs/CURRENT_STATE.md`](docs/CURRENT_STATE.md) first.** Keep `AppleAudio.sys` restorable: `tools/Restore-AppleAudioDriver.ps1` and `tools/Rollback-T2AudioDriver.ps1` are provided.

1. Enable test signing and sign the package (`packaging/`).
2. Install with `tools/Install-T2AudioDriver.ps1`.
3. Run a staged session with `tools/T2Audio-TestAgent.ps1 -Stage 3` (discovery and stream format only, no audio I/O) and capture the log with `tools/Capture-T2AudioLog.ps1`.
4. Before any stage-5 run, the stage-3 log must show `pft: rate=48000 ... ch=6 bits=24` and `sample shift=8`.

---

## Project structure

* `src/` — PortCls WaveRT + topology miniports, BAR/BufferStruct discovery, BCE transport, stream mirror.
* `tests/` — host unit tests for the pure protocol and buffer helpers.
* `packaging/` — INF/CAT for test installs.
* `tools/` — install, rollback, staged test agent, log capture, WASAPI tone tests.
* `docs/` — [`DRIVER_GUIDE.md`](docs/DRIVER_GUIDE.md) (detailed guide), [`CURRENT_STATE.md`](docs/CURRENT_STATE.md) (authoritative status), [`AGENT_HANDOVER_GUIDE.md`](docs/AGENT_HANDOVER_GUIDE.md), [`INDEX.md`](docs/INDEX.md), reverse-engineering notes in `docs/analysis/`.

## Roadmap

1. Find the root cause of the `START_IO` bugcheck in `AppleUSBVHCI`'s forwarded IRP.
2. Find a receive path for T2 notifications (alive handshake, `UPDATE_TIMESTAMP`) to get a real hardware clock.
3. Add power management (remote access OFF before sleep, buffer reset on resume).
4. Add jack/headphone output and microphone input.

## Attributions

* **kaiT2en** — [kaiT2en/KaiT2en-Fedora](https://github.com/kaiT2en/KaiT2en-Fedora) `modules/t2bce_audio` (GPL-2.0), and the earlier [kekrby/linux-t2](https://github.com/kekrby/linux-t2) work. The protocol constants, BufferStruct layout and init/quiesce sequence come from there. The code here is an independent Windows implementation; see [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md).
* **osaul4ik** — [apple-t2-touchid-windows](https://github.com/osaul4ik/apple-t2-touchid-windows) (GPL-2.0). The WDK CI workflow design comes from there.
* **Apple Boot Camp** — `AppleAudio.sys` / `AppleUSBVHCI.sys`, analyzed for the transport protocol.
