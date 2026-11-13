# T2AudioPort — sound driver for the MacBook Pro 16" (2019) on Windows

A replacement for Apple's Boot Camp audio driver (AppleAudio) on the
**MacBookPro16,1**. It drives all six speakers with a sound profile close to
macOS, gives the 3.5 mm headphone jack its own output that Windows switches to
automatically, and records from the built-in microphones.

Only the MacBookPro16,1 is supported. Other T2 Macs are not tested and the
sound profile is not tuned for them.

## What works

| Feature | State |
|---|---|
| All 6 speakers, macOS-like sound (EQ, crossover, virtual bass, limiter) | works, on by default |
| Headphones as a separate output, automatic switching on plug / unplug | works, on by default |
| Built-in microphones (3-mic array) | works, off by default |
| Microphone low cut at 100 Hz (fan rumble) | on by default (`MicLowCut 0` turns it off) |
| Microphone noise suppression (removes steady background noise, also under speech) | off by default |
| Sound back after sleep without a restart | works, off by default |
| 16/24-bit formats and event-driven mode for exclusive-mode apps (DAWs) | off by default |
| All 3 built-in capsules, level-matched (lower self-noise) | beta, off by default |
| 3-mic speaker-sound canceller for the built-in mics | beta, off by default |
| Headset microphone on the 3.5 mm jack | experimental, not tested on hardware |

Volume is controlled by Windows as usual. There are no extra background apps:
the package is the driver plus one PowerShell script.

## Before you start

- Windows 10 or 11, 64-bit, installed with Boot Camp.
- **Secure Boot off.** The driver is signed with a test certificate, and
  Windows only loads such drivers in test mode, which needs Secure Boot off.
  On the Mac: restart into macOS Recovery (hold Cmd+R) → Utilities → Startup
  Security Utility → Secure Boot: **No Security**.
- In test mode Windows shows a small "Test Mode" text in the corner of the
  desktop. That is expected.
- If BitLocker is on, have the recovery key at hand; the script offers to
  pause it for one reboot.

## Install

1. Download the zip of the [latest release](https://github.com/imacorpoliz/t2audiowindows/releases)
   and extract it.
2. Open **PowerShell as administrator** in the extracted folder (Start →
   type `powershell` → right-click → Run as administrator, then `cd` to the folder).
3. Turn on test mode (once per computer), then **reboot**:
   ```powershell
   .\tools\T2Audio-Setup.ps1 -Action Prepare
   ```
4. After the reboot, again in PowerShell as administrator:
   ```powershell
   .\tools\T2Audio-Setup.ps1 -Action Install
   ```
   Type `INSTALL` when asked and answer `y` to trust the test certificate.

That is all. The speakers and the headphone output appear in Sound settings.

If PowerShell refuses to run the script ("running scripts is disabled"), run
this first in the same window: `Set-ExecutionPolicy -Scope Process Bypass`.

## Update

Extract the new release and run `-Action Install` again. Your sound settings,
switched-on features and volume are kept. Older copies of the driver are removed from
Windows' driver store automatically.

## Uninstall

```powershell
.\tools\T2Audio-Setup.ps1 -Action Uninstall
```

This brings back Apple's AppleAudio driver. The script then offers to turn
test mode off.

## The setup script

Run `.\tools\T2Audio-Setup.ps1` without arguments to get a menu. Every item
is also available as `-Action <name>`.

| Menu | `-Action` | What it is for |
|---|---|---|
| 1 | `Prepare` | Once: checks Secure Boot and turns test mode on. |
| 2 | `Install` | Installs or updates the driver. On an update, Enter keeps your sound. |
| 3 | `Uninstall` | Goes back to Apple's driver. |
| 4 | `Preset` | Changes the sound profile (see below). |
| 5 | `Tune` | Bass / treble / clarity in dB on top of the profile. |
| 6 | `Features` | Turns optional features on and off (table above). |
| 7 | `Switch` | Switches the default output between speakers and headphones by hand. |
| 8 | `SoundTest` | Plays quiet left / right / both test tones. |
| 9 | `MicTest` | Records 5 seconds from a microphone, shows the level, plays it back. |
| D | `Doctor` | Checks everything (test mode, driver, endpoints, volume) and says how to fix problems. |
| R | `Report` | Makes a zip on the desktop for a bug report. |
| B / L | `Backup` / `Restore` | Saves your settings to a file / loads them back. |
| K | `Shortcut` | Desktop shortcut Ctrl+Alt+H for speakers ↔ headphones. |
| G | `UpdateGuard` | Stops Windows Update from putting AppleAudio back. |
| C | `ResetGuard` | Lets the driver start again after the crash guard stopped it. |
| X | `Restart` | Restarts the audio device (applies settings, fixes a stuck sound). |

Also: `-Action Status` (short status), `-Action Balance -Volume 0.3`
(unmute and even out left/right).

### Sound profiles

```powershell
.\tools\T2Audio-Setup.ps1 -Action Preset -Preset macos
```

`macos` (recommended, the default), `macos-precise`, `bright`, `loud`,
`maxvolume`, `even`, `bass`, `voice`, `movie`, `night`, `warm`, `natural`,
`raw` (no processing, for comparison). Only `macos` is tuned by ear; the
others are starting points.

Fine tuning, for example a little more bass and less treble:

```powershell
.\tools\T2Audio-Setup.ps1 -Action Tune -BassDb 3 -TrebleDb -2
```

### Optional features without the menu

```powershell
# Built-in microphones and the noise gate
.\tools\T2Audio-Setup.ps1 -Action Features -Microphone 1 -MicNoiseSuppression 1
```

Switches: `-Headphones`, `-Microphone`, `-MicNoiseSuppression`,
`-MicBeam`, `-MicCanceller`, `-HeadsetMic`, `-ResumeAfterSleep`, `-ExtraFormats`,
`-HeadphoneCrossfeed`, `-DspLoudness`, `-AntiRattle` (each `1` = on,
`0` = off). The device restarts once to apply them; the sound stops for a
couple of seconds.

## If something goes wrong

- **No sound / no T2 device in Sound settings:** run `-Action Doctor` and
  follow what it says.
- **The driver stopped starting after a crash:** this is the crash guard.
  After two unclean shutdowns in a row (BSOD, hang, holding the power button)
  the driver stays off so the Mac cannot get stuck. Find the cause with
  `-Action Report`, then `-Action ResetGuard`.
- **Windows Update put AppleAudio back:** `-Action Install` again, then
  `-Action UpdateGuard -Block 1`.
- **Sound device found disabled after booting macOS and back to Windows:**
  turn off Windows Fast Startup (`powercfg /h off` as administrator, or
  Control Panel → Power Options → Choose what the power buttons do). With it,
  Windows does not really shut down, and the T2 reset by macOS no longer
  matches what Windows resumes. `-Action Doctor` warns about it.
- **Safe Mode:** the driver never starts in Safe Mode, so you can always get
  in and uninstall from there.

## For developers

- [docs/DRIVER_GUIDE.md](docs/DRIVER_GUIDE.md) — how the driver works
- [docs/CURRENT_STATE.md](docs/CURRENT_STATE.md) — what was tested on hardware, changelog
- [docs/ROADMAP.md](docs/ROADMAP.md) — what is left
- [docs/INSTALL.md](docs/INSTALL.md) — building and signing the package

## License and credits

GPL-3.0-or-later. Speaker and microphone sound profiles come from
[KAIT2EN](https://github.com/kaiT2en/KaiT2en-Fedora) (C) André Eikmeyer;
the virtual bass follows Bankstown (C) James Calligeros, MIT. Full list in
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
