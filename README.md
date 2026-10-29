<p align="center">
  <img src="assets/banner.svg" alt="T2AudioPort — Apple T2 audio for Windows" width="100%">
</p>

<p align="center">
  <a href="https://github.com/imacorpoliz/t2audiowindows/actions/workflows/build.yml"><img src="https://github.com/imacorpoliz/t2audiowindows/actions/workflows/build.yml/badge.svg?branch=diagnostics" alt="Build &amp; Test"></a>
  <img src="https://img.shields.io/badge/status-beta-orange" alt="Status: beta">
  <img src="https://img.shields.io/badge/Mac-MacBookPro16%2C1-lightgrey?logo=apple" alt="MacBookPro16,1">
  <img src="https://img.shields.io/badge/Windows-10%20%7C%2011%20x64-0078D6?logo=windows" alt="Windows 10/11 x64">
  <img src="https://img.shields.io/badge/driver-WaveRT%20%2F%20PortCls-blue" alt="WaveRT / PortCls">
  <img src="https://img.shields.io/badge/license-GPL--3.0--or--later-green" alt="GPL-3.0-or-later">
</p>

<p align="center">
  <b>A Windows audio driver for the MacBook Pro 16" (2019) with the Apple T2 chip</b><br>
  an alternative to Apple's Boot Camp driver, with the speaker sound of the Linux project <a href="https://github.com/kaiT2en/KaiT2en-Fedora">KAIT2EN</a>
</p>

<p align="center">
  <a href="docs/GUIDE.md">🚀 Install guide</a> ·
  <a href="docs/INSTALL.md">📖 Reference</a> ·
  <a href="docs/ROADMAP.md">🗺️ Roadmap</a> ·
  <a href="docs/CURRENT_STATE.md">📋 Changelog &amp; tests</a>
</p>

---

> [!NOTE]
> **Beta, one machine.** Everything marked ✅ was tested on a single MacBookPro16,1 by the project owner. Other T2 Macs are not supported yet.

## ✨ What works

| | Feature | Since | Notes |
|:-:|---|:-:|---|
| ✅ | 🔊 **Six speakers**, stereo 48 kHz | 1.0.8 | KAIT2EN 16_1 processing: EQ per speaker, crosstalk cancellation, virtual bass, multiband compressor, limiter |
| ✅ | 🎚️ **Presets and tone** | 1.0.12 | 13 presets, bass / treble / clarity ±12 dB; `macos` is the owner-approved default |
| ✅ | 🛡️ **Starts with Windows**, crash guard | 1.0.12 | Two unclean ends in a row and the driver steps aside until you reset it |
| ✅ | 🎧 **Headphones** (3.5 mm) | 1.0.13 | Separate output, −12 dB safety cut |
| ✅ | 🎙️ **Built-in microphones** | 1.0.13 | Mono array, KAIT2EN mic processing (no noise suppression) |
| ✅ | 🌙 **Sleep and wake** | 1.0.13 | Playback continues after wake |
| ✅ | 🎛️ **16/24-bit** for exclusive-mode apps | 1.0.13 | Shared mode stays 32-bit |

## 🚧 Not yet

| | What | Why |
|:-:|---|---|
| ⏳ | Automatic headphone ↔ speaker switching | The T2 does not report the jack the way we read it — under investigation; pick the output by hand meanwhile |
| ⏳ | Headset microphone | Comes after the jack |
| ⏳ | Hardware volume | Windows' software volume works fine meanwhile |
| ⏳ | 44.1 / 96 kHz | 48 kHz only; Windows converts in shared mode |
| ⏳ | Other T2 Macs | KAIT2EN has profiles for 12 models, each needs a tester |
| 🚫 | Microsoft signature | Decided against: the driver stays **test-signed** (test mode on, Secure Boot off) |

## 🚀 Quick start

```powershell
# PowerShell as Administrator, in the unpacked release folder
.\tools\T2Audio-Setup.ps1 -Action Prepare                        # once: test mode, then reboot
.\tools\T2Audio-Setup.ps1 -Action Install -Preset macos
.\tools\T2Audio-Setup.ps1 -Action Features -Headphones 1 -Microphone 1 -ResumeAfterSleep 1
```

Step by step, including turning Secure Boot off on the Mac: **[docs/GUIDE.md](docs/GUIDE.md)**. Back to Apple's driver at any time: `-Action Uninstall`.

## 🎛️ How the sound is made

```mermaid
flowchart LR
    A[Windows audio<br>stereo 48 kHz] --> B[Tone controls<br>and autogain]
    B --> C[Virtual bass]
    C --> D[EQ + crosstalk<br>cancellation]
    D --> E[Multiband<br>compressor]
    E --> F[Crossover + EQ<br>per speaker]
    F --> G[Lookahead<br>limiter]
    G --> H((6 speakers<br>via the T2))
```

Everything runs inside the driver, in fixed point, about 50 µs per 2 ms of audio.

## 🙏 Where everything comes from

| Part | Source |
|---|---|
| T2 protocol, buffer layout, speaker layout, clock | Linux driver `modules/t2bce_audio` of [KAIT2EN](https://github.com/kaiT2en/KaiT2en-Fedora) (earlier [linux-t2](https://github.com/kekrby/linux-t2)) — our Windows code is new, the knowledge is theirs |
| Speaker and microphone sound | KAIT2EN `dsp/profiles/16_1`, converted by `tools/dsp/gen_kait2en_dsp.py`; the fixed-point engine is ours |
| Virtual bass | [bankstown](https://github.com/chadmed/bankstown) (MIT), re-implemented in fixed point |
| Compressor / autogain options | modelled on [lsp-plugins](https://lsp-plug.in/) defaults as used by KAIT2EN (no lsp code) |
| Windows-specific behaviour | reverse engineering of Apple's `AppleAudio.sys` / `AppleUSBVHCI.sys` for interoperability ([`docs/analysis/`](docs/analysis/)) |
| The work itself | code by Claude (Anthropic), builds and hardware tests by Gemini (Google) on the owner's Mac, every decision and listening verdict by the owner |

Full credits: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

## ⚖️ Licence

**GPL-3.0-or-later** ([LICENSE](LICENSE)), because the driver contains DSP tables derived from KAIT2EN's GPL-3.0-or-later profile. Its attribution term (GPL-3.0 §7b) applies: *speaker and microphone DSP from the KAIT2EN 16_1 profile, Copyright (C) 2026 André Eikmeyer*. Virtual bass after bankstown, MIT, Copyright (C) 2023 James Calligeros.

## 🛠️ For developers

* [docs/DRIVER_GUIDE.md](docs/DRIVER_GUIDE.md) — architecture, every registry parameter, log lines
* [docs/ROADMAP.md](docs/ROADMAP.md) — what is left · [GEMINI.md](GEMINI.md) — build, signing, hardware test procedures
* Build: Visual Studio 2022 + WDK, `msbuild src\T2AudioMiniport.vcxproj /p:Configuration=Release /p:Platform=x64`
* Tests: `tests\*.c` with any C compiler (`-lm`); CI builds the driver and runs all tests on every push
