# Apple T2 Audio Driver for Windows (MacBook Pro 16", 2019)

[![Status](https://img.shields.io/badge/status-stable%20release-brightgreen)](#)
[![Model](https://img.shields.io/badge/Mac-MacBookPro16%2C1-lightgrey?logo=apple)](#)
[![Windows](https://img.shields.io/badge/Windows-10%20%7C%2011%20x64-0078D6?logo=windows)](#)
[![License](https://img.shields.io/badge/license-GPL--3.0--or--later-green)](#)

A high-performance native Windows audio driver for the Apple T2 Security Chip on **MacBook Pro 16" (2019)**. 

Replaces the quiet, flat, and buggy Boot Camp audio driver with full **macOS-grade sound quality**, deep virtual bass, and automatic headphone switching.

---

## ✨ Features

- 🔊 **6 Physical Speakers:** Uses all 4 force-cancelling woofers and 2 tweeters simultaneously.
- 🎵 **macOS Sound Quality:** Built-in hardware DSP with Apple-matched EQ curves and James Calligeros' virtual sub-bass generator.
- 🔄 **Auto-Switching Headphones:** Plug in your 3.5 mm headphones and the sound moves to headphones instantly; unplug and it returns to speakers.
- 🎙️ **Built-in Microphones:** Full 3-microphone studio array with beamforming noise reduction.
- 🎬 **5.1 Virtual Surround:** Expands movies and games across the 6 physical speakers with acoustic crosstalk cancellation.
- 🛡️ **Crash Guard:** Automatically protects against boot loops and recovers smoothly.
- ⚡ **Zero Bloat:** Minimal package — just the signed driver and a setup script. No background telemetry or heavy software.

---

## 🚀 Quick Install (2 Steps)

### Step 1: Enable Test Mode (One-Time)
Because this driver is self-signed, Windows must be in test mode:

1. Turn off Secure Boot on your Mac (in macOS Recovery → Startup Security Utility → set to "No Security").
2. Download and extract the [latest release zip](https://github.com/imacorpoliz/t2audiowindows/releases).
3. Open **PowerShell as Administrator** in the extracted folder and run:
   ```powershell
   .\tools\T2Audio-Setup.ps1 -Action Prepare
   ```
4. **Reboot your Mac.**

### Step 2: Install Driver
Open **PowerShell as Administrator** in the folder and run:
```powershell
.\tools\T2Audio-Setup.ps1 -Action Install -Preset macos
```

**That's it!** Your speakers, headphones, and microphone are now active.

---

## 🎛️ Sound Tuning & Presets

You can change sound profiles anytime with one command:

```powershell
# Interactive menu:
.\tools\T2Audio-Setup.ps1

# Quick preset switch:
.\tools\T2Audio-Setup.ps1 -Action Preset -Preset macos     # Original Apple sound (Recommended)
.\tools\T2Audio-Setup.ps1 -Action Preset -Preset loud      # Louder with rattle protection
.\tools\T2Audio-Setup.ps1 -Action Preset -Preset bass      # Extra bass (+4 dB)
.\tools\T2Audio-Setup.ps1 -Action Preset -Preset bright    # More treble & air (+3 dB)
.\tools\T2Audio-Setup.ps1 -Action Preset -Preset voice     # Clear speech (podcasts/calls)
.\tools\T2Audio-Setup.ps1 -Action Preset -Preset movie     # Cinema soundstage with even dialogue

# Custom equalizer (+/- 12 dB):
.\tools\T2Audio-Setup.ps1 -Action Tune -BassDb 3 -TrebleDb 2
```

---

## ❓ Frequently Asked Questions

#### Why do I see both "Speakers" and "Headphones" in Windows?
In Windows, they are configured as separate endpoints so that headphones get clean stereo sound (with optional crossfeed) without the heavy speaker correction filters. When you plug in headphones, Windows switches playback automatically.

#### How do I uninstall?
To restore the default Apple Boot Camp driver at any time:
```powershell
.\tools\T2Audio-Setup.ps1 -Action Uninstall
```

#### How do I check driver health?
```powershell
.\tools\T2Audio-Setup.ps1 -Action Doctor
```

---

## 📚 Technical Documentation

For developers, contributors, and detailed architecture notes:
- [Installation Reference (docs/INSTALL.md)](docs/INSTALL.md)
- [Architecture & Driver Guide (docs/DRIVER_GUIDE.md)](docs/DRIVER_GUIDE.md)
- [Hardware Verification & Changelog (docs/CURRENT_STATE.md)](docs/CURRENT_STATE.md)
- [Development Roadmap (docs/ROADMAP.md)](docs/ROADMAP.md)

---

## ⚖️ License & Credits

- Licensed under **GPL-3.0-or-later**.
- Speaker & microphone acoustic profiles from the [KAIT2EN](https://github.com/kaiT2en/KaiT2en-Fedora) project, (C) André Eikmeyer.
- Virtual bass harmonic generation after Bankstown, (C) James Calligeros (MIT).
- See [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for full attribution.
