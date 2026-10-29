# Installing T2AudioPort

## Requirements

* MacBookPro16,1 (MacBook Pro 16", 2019) with Windows 10/11 x64 and the Boot Camp drivers (`AppleAudio`).
* Test mode (the driver is test-signed and will stay so): `.\tools\T2Audio-Setup.ps1 -Action Prepare` (menu 8) checks Secure Boot, offers to suspend BitLocker for one reboot, runs `bcdedit /set testsigning on`; then reboot. Secure Boot must be off (macOS Recovery → Startup Security Utility → "No Security"). Windows shows "Test Mode" on the desktop while it is on.
* A signed package: either a release zip (`packaging\`, `tools\`, `certificate\`; made by `tools\Make-ReleasePackage.ps1`) — the installer offers to trust the included test certificate — or your own build in `packaging\` (build and signing: `GEMINI.md` section 2).

## Install

PowerShell **as Administrator**, in the repository folder:

```powershell
.\tools\T2Audio-Setup.ps1
```

Menu (grouped; letters for maintenance):

| Key | Action | Command |
|---|---|---|
| 1 | Prepare Windows (test mode, once) | `-Action Prepare` |
| 2 | Install / update | `-Action Install -Preset macos` |
| 3 | Restore AppleAudio | `-Action Uninstall` |
| 4 | Sound preset | `-Action Preset -Preset bright` |
| 5 | Tone (bass / treble / clarity) | `-Action Tune -BassDb 3 -TrebleDb -2` |
| 6 | Optional features | `-Action Features -Headphones 1 -Microphone 1 -ResumeAfterSleep 1` |
| 7 | Switch speakers ↔ headphones | `-Action Switch [-To Speakers\|Headphones\|Toggle]` *(no admin needed)* |
| 8 | Sound test: left, right, both | `-Action SoundTest` *(no admin needed)* |
| 9 | Microphone test: record 5 s, level, playback | `-Action MicTest` *(no admin needed)* |
| D | Doctor: checks everything, prints a fix for each problem | `-Action Doctor` |
| R | Bug report zip on the desktop | `-Action Report` |
| B / L | Back up / load your settings (JSON) | `-Action Backup [-File x.json]`, `-Action Restore [-File x.json]` |
| K | Desktop shortcut **Ctrl+Alt+H** to switch outputs | `-Action Shortcut` *(no admin needed)* |
| G | Windows Update driver guard | `-Action UpdateGuard -Block 1` |
| C | Reset the crash guard | `-Action ResetGuard` |
| — | Status / L-R balance | `-Action Status`, `-Action Balance -Volume 0.3` |

`-Force` skips the confirmation prompts.

## Presets

| Preset | What it does |
|---|---|
| `macos` ★ | Apple-style EQ for the six speakers, virtual bass, multiband compressor. **The one the owner approved.** |
| `macos-precise` | Same, compressor measures like macOS (RMS, soft knee); the owner preferred `macos` |
| `bright` | Tweeters +3 dB |
| `loud` | More gain, woofer rattle protection |
| `maxvolume` | Woofers limited 6 dB lower, for loud music with bass |
| `even` | Slow loudness autogain: quiet tracks up, loud ones down |
| `bass` | Bass +4 dB; **rattles on kick drums at high volume** on this Mac |
| `voice` | Clearer speech, less bass, even loudness |
| `movie` | Even loudness, dialogue and bass slightly up |
| `night` | Quiet and even, less bass |
| `warm` | No compressor |
| `natural` | EQ and crossover only |
| `raw` | No processing (comparison only) |

`-Action Tune` changes only bass (shelf 150 Hz), treble (shelf 6 kHz), clarity (peak 2.5 kHz) and the compressor style; choosing a preset resets them.

## Optional features

All verified on hardware (2026-10-29), all off until you turn them on:

| Feature | Switch | Notes |
|---|---|---|
| Headphones | `-Headphones 1` | A separate "Headphones" output (−12 dB safety cut). Windows does **not** switch automatically yet: pick it in the sound menu. |
| Microphones | `-Microphone 1` | "Microphone Array", mono 48 kHz. |
| Sound after sleep | `-ResumeAfterSleep 1` | Playback continues after wake without restarting the player. |
| 16/24-bit | `-ExtraFormats 1` | For exclusive-mode apps; shared mode stays 32-bit. |

`0` turns a feature off; `-Action Status` lists them.

## Crash guard

The driver marks each hardware session in the registry and clears the mark on a clean end (device stop, sleep, shutdown). If the mark is still there at boot, the previous session ended abnormally (BSOD, hang, power loss). **After two such ends in a row the driver stops loading**; Windows boots normally, without T2 sound. Then:

1. `-Action Uninstall` to go back to AppleAudio, or
2. look at the cause (minidump in `C:\Windows\Minidump`) and run `-Action ResetGuard`.

The driver never loads in Safe Mode.

## Troubleshooting

* **One side quieter:** `-Action Status` shows the L/R levels; `-Action Balance -Volume 0.3` evens them (Windows keeps a balance per device, even across reinstalls).
* **No sound after install:** check that "Speakers (Apple T2 Audio Device …)" is the default output and not muted.
* **Windows Update brought AppleAudio back:** `-Action Status` warns about it; run `-Action Install` again. `-Action UpdateGuard -Block 1` (menu 7) stops Windows Update from installing drivers — for **all** devices — and `-Block 0` restores the previous settings.
* **Everything else:** `-Action Uninstall` restores Apple's driver; `tools\Restore-AppleAudioDriver.ps1` downloads it from Boot Camp if needed.

Registry values behind all of this: `HKLM\SYSTEM\CurrentControlSet\Services\T2AudioMiniport\Parameters`, described in `docs/DRIVER_GUIDE.md` section 8.3.
