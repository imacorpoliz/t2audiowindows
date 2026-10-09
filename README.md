# T2AudioPort — experimental Apple T2 audio driver for Windows

Experimental WDM/PortCls WaveRT miniport for the Apple T2 audio function
(`PCI\VEN_106B&DEV_1803`) on MacBookPro16,1. This is a **BSOD reproducer, not a
working audio replacement**. Keep the AppleAudio driver for everyday playback.

## Status (October 2026)

- DriverEntry, AddDevice, PortCls Topology/WaveRT registration, BAR mapping and
  BCE discovery work in diagnostic sessions. The Speaker device ID is `0x39`
  on the tested MacBookPro16,1. A stage-4 test **without** BCE I/O completed.
- A stage-4 render test **with BCE `START_IO` enabled** repeatedly bugchecks:
  `SYSTEM_THREAD_EXCEPTION_NOT_HANDLED (0x7E)`, `0xC0000005`, read of `0x18`
  in `ks!DispatchDeviceIoControl+0x15`. The observed stack is
  `AppleUSBVHCI -> ksthunk -> ks`. Setting `FileObject` on our outgoing BCE
  IRP did not fix this; the forwarded IRP's origin and handling remain unknown.
- The signed **1.0.3.15** package in `packaging/` is the binary that reproduced
  the bugcheck. Its `T2AudioMiniport.sys` SHA256 is
  `7DDB8237DE367669B98B603782F0DEF39EABFBEB3F79D33532C8D1F11BD32039`.
  `src/Phase4.c` retains the failing `START_IO` path for investigation. Normal
  boot is inert: without a volatile `TestSession` key the driver refuses to
  start before mapping resources or opening BCE. The manual test agent refuses
  `-BceIo` even with `-Force` to prevent reproducing the known bug by mistake.
- The test machine was restored to **AppleAudio** (`oem16.inf`, Started/OK).
  No custom driver is installed on it. No full crash dump or Apple binary is
  included in this repository.

See [`docs/CURRENT_STATE.md`](docs/CURRENT_STATE.md) for the timeline and
diagnostics. The sanitized captures in `docs/logs/EXCERPT_*.md` do not include
full kernel logs; local minidumps are not published. The latest bugcheck was
at 2026-10-08 01:48:20; its minidump is local to the test machine.

## Why it bugchecks (probable cause)

What is proven from the two minidumps:

- The bugcheck is `SYSTEM_THREAD_EXCEPTION_NOT_HANDLED (0x7E)` with
  `0xC0000005` (access violation) reading address `0x18`, at
  `ks!DispatchDeviceIoControl+0x15`, on a System worker thread.
- The observed stack is `AppleUSBVHCI -> ksthunk -> ks`.
- Disassembly shows `ks.sys` dereferencing
  `IoGetCurrentIrpStackLocation(Irp)->FileObject->FsContext` (`_FILE_OBJECT`
  field at `+0x18`) **without a NULL guard**; the `FileObject` on that IRP is
  `NULL`.
- Assigning our transport `FileObject` to `IoGetNextIrpStackLocation` of the
  IRP we build did **not** fix the fault, so the faulting IRP is a *different,
  forwarded* IRP, not our outgoing one.

Probable cause: the BCE control path builds an IRP with
`IoBuildDeviceIoControlRequest` and sends it to `\Device\AppleUSBVHCI`. That
stack (or the `ksthunk`/`ks` proxy bound in the AppleAudio device stack) then
routes an IRP through Kernel Streaming, and KS receives an IRP that never had a
file object attached. `ks.sys` assumes `FileObject` is always present and
faults on the missing one.

Unproven hypotheses (a small dump does not contain the IRP, so none of these is
confirmed):

- Our transport handle is not a KS file object, so a request that reaches KS
  from our session has no valid `FileObject`.
- A wrong target or control code causes `AppleUSBVHCI` to forward the request
  into the KS proxy.
- The KS filter/pin was created without a bound file object.
- `AppleUSBVHCI` reuses an internal notification IRP that KS never normally
  sees.

Because the mechanism is not proven, no blind fix is shipped: the manual test
agent refuses `-BceIo` to avoid reproducing the bug by accident.

## Project stage

Working (verified in diagnostic sessions): `DriverEntry`/`AddDevice`, PortCls
Topology + WaveRT registration and the physical connection, BAR mapping,
content-based device discovery, BCE transport open, device enumeration and
Speaker discovery (`0x39` on MacBookPro16,1), `SET_REMOTE_ACCESS`, and a
host-side WaveRT buffer lifecycle. All of this is gated so a normal boot is
inert.

Not working: **no audio reaches the hardware.** The BCE `START_IO` command — the
one that actually starts the DAC/amp path — bugchecks. There is no MMIO/DMA PCM
path, no AVE session service, and no volume, power or hotplug handling.

In short: this is a diagnostic build at the **transport + discovery** stage,
roughly where the Linux reference (kaiT2en) was before its audio path worked.

## What remains for full sound (kaiT2en parity)

The Linux project [kaiT2en](https://github.com/kaiT2en) reaches full, usable
internal-speaker sound with a layered stack: a BCE audio kernel driver, an AVE
session service, a host-side DSP graph per model, and normal audio-device
plumbing. A Windows port needs an equivalent of each:

1. **Reliable BCE transport and enable sequence (the current blocker).**
   kaiT2en uses the `t2bce_audio` kernel driver plus the `t2bce_ave`/`t2-ave`
   service that owns the AVE (Apple Voice Engine) sessions and the start/stop
   messages. On Windows, the `ks!DispatchDeviceIoControl` fault must be
   root-caused and the correct control IRP, target and file-object binding
   found. Nothing downstream can be validated until `START_IO` stops bugchecking.
2. **A PCM path to the device.** Either the MMIO/DMA path (map BAR1 and point
   the device at a buffer) or BCE streaming, with the fixed 48 kHz /
   6-channel / 32-bit (24-byte frame) format. Today the WaveRT buffer is a
   host-side system buffer and the copy DPC is gated off, so no PCM is written.
3. **A real endpoint and routing.** Expose the speaker as a render endpoint and
   wire the discovered Speaker device id into the audio path (it stays
   diagnostic today).
4. **Model-specific host-side speaker DSP**, the equivalent of kaiT2en's
   `t2-dsp` profiles: equalization + crossovers (biquads), multiband compressor
   + limiters, crosstalk cancellation and virtual bass. kaiT2en does this in
   PipeWire/WirePlumber; on Windows the equivalent role is Equalizer APO. This
   is what makes the speakers sound like a Mac instead of thin and raw. As an
   interim, the standalone Equalizer APO preset (e.g. the
   `bananakid/apple-t2-audio-controller-boot-camp` FIR set) already provides
   this processing without the custom driver.
5. **Volume, power management and hotplug/headphone handling.**

Parity therefore requires, in order: (1) the crash fixed, (2) a working PCM
path, (3) a real endpoint, (4) a per-model DSP graph, (5) volume/power/hotplug.

## Build and package

Requirements: Visual Studio with the Windows kernel-mode driver toolset,
Windows Driver Kit 10.0.28000.0, and x64 Windows. On the development machine:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' `
  'src\T2AudioMiniport.vcxproj' /p:Configuration=Debug /p:Platform=x64 `
  /p:SignMode=Off /t:Build
```

Unsigned output: `src/bin/Debug/T2AudioMiniport.sys`. Rebuilding does **not**
reproduce the hash of the signed package: signing changes the binary. If
building your own package, sign the SYS with your **own** local test certificate,
generate a catalog from `packaging/t2audio.cdf` with `makecat`, and sign the
catalog after copying the new SYS. Never pair a rebuilt SYS with an old catalog.
No private signing keys are distributed here. After compiling the host-side
tests with MSVC (`cl /nologo /W4 /Fe:tests\BceProtocolLogicTest.exe
tests\BceProtocolLogicTest.c` and likewise for
`tests\T2AudioBufferLogicTest.c`), run:

```powershell
& 'tests\BceProtocolLogicTest.exe'
& 'tests\T2AudioBufferLogicTest.exe'
```

## Diagnostic safeguards and recovery

`tools/Install-T2AudioDriver.ps1` stages the package by default; `-Bind`
replaces the current AppleAudio binding and may require a reboot. Do not use
the signed package for daily sound. `tools/T2Audio-TestAgent.ps1` creates a
volatile session and disables the test device afterward; it rejects `-BceIo`
because that path is known to crash. Stage 5 / `-Mmio` writes to BAR memory and
has not been validated as a fix. See the agent script and
[`docs/CURRENT_STATE.md`](docs/CURRENT_STATE.md) before any hardware test.

To restore stock sound after a test or reboot, first remove the volatile
session, then remove the **currently published T2AudioMiniport INF** (look it up
with `pnputil /enum-drivers`) using `pnputil /delete-driver oemNNN.inf /uninstall`;
rescan devices and enable `PCI\VEN_106B&DEV_1803` if needed. Verify its service
is `AppleAudio` and its status is `OK`. Do not remove the AppleAudio `oem16.inf`.

## Layout

- `src/`: PortCls miniport, WaveRT stream, BAR discovery, BCE transport and
  command serialization.
- `tests/`: host-side protocol and buffer logic tests.
- `packaging/`: signed **known-crashing** test SYS, INF and catalog.
- `tools/`: staging, manual diagnostic agent and AppleAudio recovery scripts.
- `docs/CURRENT_STATE.md`: authoritative implementation and test history.

See `THIRD_PARTY_LICENSES.md` for protocol research attributions.
