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
- 🔄 **Auto-Switching Headphones:** Optional jack tracking moves sound between the separate headphone and speaker endpoints on plug/unplug.
- 🎙️ **Built-in Microphones:** Optional 3-microphone studio array with beamforming noise reduction.
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

**That's it!** Your T2 speakers and the separate headphone endpoint are active.
Automatic jack switching and microphone capture can be enabled later from the
installer menu.

---

## 🎛️ Sound Tuning & Presets

You can change sound profiles anytime with one command. The installer writes
the setting and restarts the T2 audio device once so the driver picks it up:

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

## 🎚️ Feature switchboard

Run the installer without arguments as Administrator:

```powershell
.\tools\T2Audio-Setup.ps1
```

Choose **6. Optional features**. The switchboard shows every feature as
`ON`, `OFF`, or `PARTIAL`. Enter feature numbers to toggle several items,
then choose **A** to apply them together, **R** to reset the pending choices,
or **Q** to cancel. The device is restarted only once after applying changes.

The switchboard controls:

- **Headphones** — separate 3.5 mm endpoint, jack detection, automatic
  speaker muting, and Windows endpoint switching;
- **Built-in microphones** — microphone array capture;
- **Resume sound after sleep** — restore active streams after power wake;
- **16/24-bit formats** — extra exclusive-mode PCM formats;
- **Headphone bs2b crossfeed** — optional binaural crossfeed (off by default);
- **Low-volume loudness compensation** and **extra woofer anti-rattle
  protection**.

The same settings can be changed without the menu:

```powershell
# Enable headphones and microphone; one device restart applies both.
.\tools\T2Audio-Setup.ps1 -Action Features -Headphones 1 -Microphone 1

# Disable every experimental feature.
.\tools\T2Audio-Setup.ps1 -Action Features `
    -Headphones 0 -Microphone 0 -ResumeAfterSleep 0 -ExtraFormats 0 `
    -HeadphoneCrossfeed 0 -DspLoudness 0 -AntiRattle 0
```

Feature changes are deliberately applied with a device restart rather than
claiming they are all live. This prevents stale state in the kernel driver and
ensures dependent settings (for example, headphone auto-mute) are changed
together. A short interruption of audio is expected.

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
