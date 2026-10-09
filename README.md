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
