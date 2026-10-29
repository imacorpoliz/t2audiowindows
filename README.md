# T2AudioPort — Apple T2 audio driver for Windows

[![Build & Test](https://github.com/imacorpoliz/t2audiowindows/actions/workflows/build.yml/badge.svg?branch=diagnostics)](https://github.com/imacorpoliz/t2audiowindows/actions/workflows/build.yml)

A Windows (WaveRT / PortCls) driver for the audio of the **MacBook Pro 16" 2019 (MacBookPro16,1)** with the Apple T2 chip, as an alternative to Apple's Boot Camp driver (`AppleAudio.sys`). It brings the speaker processing of the Linux project [KAIT2EN](https://github.com/kaiT2en/KaiT2en-Fedora) to Windows: equaliser for the six speakers, crosstalk cancellation, virtual bass, multiband compressor and limiter.

**Status: beta, one machine.** Everything below was tested on a single MacBookPro16,1 by the project owner. Other T2 Macs are not supported.

## What works (verified on hardware)

| Feature | Since | Notes |
|---|---|---|
| Internal speakers, stereo 48 kHz | 1.0.8 | All six speakers, through the KAIT2EN 16_1 processing; the default `macos` preset was approved by ear |
| Sound presets and tone controls | 1.0.12 | 13 presets, bass / treble / clarity ±12 dB; only `macos` is owner-approved |
| Start on every boot, crash guard | 1.0.12 | After two unclean ends in a row the driver stops loading until reset |
| 3.5 mm headphones (separate output) | 1.0.13 | Plays; **no automatic switching yet** (see below) |
| Built-in microphone array (mono) | 1.0.13 | Records speech; simple KAIT2EN mic processing, no noise suppression |
| Sound returns after sleep | 1.0.13 | `ResumeAfterSleep` |
| 16/24-bit for exclusive-mode apps | 1.0.13 | `ExtraFormats` |

## What does not work yet

* **Automatic headphone ↔ speaker switching.** The T2 does not report the jack the way the driver reads it; you pick the output in Windows by hand. Under investigation.
* Headset microphone on the 3.5 mm jack.
* Hardware volume (Windows' software volume is used; it works).
* 44.1 / 96 kHz (48 kHz only; Windows converts in shared mode).
* Any Mac other than MacBookPro16,1.
* A Microsoft signature: the driver is **test-signed**. You need `testsigning` on and Secure Boot off.

## Install

Requirements: MacBookPro16,1 with Windows 10/11 x64 and Boot Camp drivers; `bcdedit /set testsigning on` and a reboot; Secure Boot off (Startup Security Utility); a signed package in `packaging\`.

```powershell
# PowerShell as Administrator, in the repository folder
.\tools\T2Audio-Setup.ps1                     # menu
.\tools\T2Audio-Setup.ps1 -Action Install -Preset macos
.\tools\T2Audio-Setup.ps1 -Action Features -Headphones 1 -Microphone 1 -ResumeAfterSleep 1
.\tools\T2Audio-Setup.ps1 -Action Uninstall   # back to Apple's driver
```

Details, presets and troubleshooting: [`docs/INSTALL.md`](docs/INSTALL.md).

## Where everything comes from

* **BCE protocol, BufferStruct layout, speaker layout, clock handling:** the Linux driver `modules/t2bce_audio` of [KAIT2EN](https://github.com/kaiT2en/KaiT2en-Fedora) (earlier [linux-t2](https://github.com/kekrby/linux-t2)). The Windows code is new, but the protocol knowledge is theirs.
* **Speaker and microphone processing:** coefficients generated from KAIT2EN's `dsp/profiles/16_1` (`graph.json`, `XTC-side-48k.wav`, `mic.json`) by `tools/dsp/gen_kait2en_dsp.py`. The fixed-point engine (`src/T2AudioDsp.h`) is original code.
* **Virtual bass:** the algorithm of [bankstown](https://github.com/chadmed/bankstown) (MIT), re-implemented in fixed point.
* **Compressor and autogain options:** modelled on the defaults of [lsp-plugins](https://lsp-plug.in/) `mb_compressor` / `autogain` as used by KAIT2EN; no lsp code.
* **Windows-specific behaviour** (how AppleUSBVHCI delivers T2 messages, the microphone registration format, the sample-rate property, power handling): reverse engineering of Apple's `AppleAudio.sys` and `AppleUSBVHCI.sys` for interoperability, documented in [`docs/analysis/`](docs/analysis/).
* **How it was built:** the code was written by Claude (Anthropic), builds and hardware tests were run by Gemini (Google) on the owner's Mac, and the owner made every decision and every listening judgement. The full history is in [`docs/CURRENT_STATE.md`](docs/CURRENT_STATE.md).

Full credits: [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md).

## Licence

The repository has **no top-level licence yet**. Because the driver compiles DSP tables derived from KAIT2EN's GPL-3.0-or-later profile, a built `T2AudioMiniport.sys` must be distributed under **GPL-3.0-or-later** (with KAIT2EN's attribution term). The owner still has to add the licence before a public release.

## For developers

* [`docs/DRIVER_GUIDE.md`](docs/DRIVER_GUIDE.md) — how the driver works, every registry parameter, the log lines.
* [`docs/ROADMAP.md`](docs/ROADMAP.md) — what is left.
* [`docs/CURRENT_STATE.md`](docs/CURRENT_STATE.md) — dated log of every change and hardware test.
* [`GEMINI.md`](GEMINI.md) — build, signing and hardware test procedures.

Build: Visual Studio 2022 + WDK, `msbuild src\T2AudioMiniport.vcxproj /p:Configuration=Release /p:Platform=x64`. Host tests: `tests\*.c` (any C compiler, `-lm`). CI builds the driver and runs all tests on every push.
