# Installing T2AudioPort

## Requirements

* MacBookPro16,1 (MacBook Pro 16", 2019) with Windows 10/11 x64 and the Boot Camp drivers (`AppleAudio`).
* Test signing: `bcdedit /set testsigning on`, then reboot. Secure Boot must be off (macOS Recovery → Startup Security Utility).
* A signed package: either a release zip (`packaging\`, `tools\`, `certificate\`; made by `tools\Make-ReleasePackage.ps1`) — the installer offers to trust the included test certificate — or your own build in `packaging\` (build and signing: `GEMINI.md` section 2).

## Install

PowerShell **as Administrator**, in the repository folder:

```powershell
.\tools\T2Audio-Setup.ps1
```

Menu:

1. **Install / update T2AudioPort** — asks for a preset, binds the driver, turns persistent mode on, unmutes the T2 speakers at 30 % volume.
2. **Change the sound preset** — restarts the device (sound stops for a few seconds).
3. **Reset the crash guard** — if the driver stopped loading after two failures.
4. **Restore AppleAudio** — removes T2AudioPort; the device goes back to Apple's driver.
5. **Adjust the tone** — bass / treble / clarity on top of the preset.
6. **Optional features** — headphones, microphones, sound after sleep, 16/24-bit.

The same without the menu:

```powershell
.\tools\T2Audio-Setup.ps1 -Action Install -Preset macos
.\tools\T2Audio-Setup.ps1 -Action Preset -Preset bright
.\tools\T2Audio-Setup.ps1 -Action Tune -BassDb 3 -TrebleDb -2 -ClarityDb 1
.\tools\T2Audio-Setup.ps1 -Action Features -Headphones 1 -Microphone 1 -ResumeAfterSleep 1 -ExtraFormats 1
.\tools\T2Audio-Setup.ps1 -Action Balance -Volume 0.3
.\tools\T2Audio-Setup.ps1 -Action Status
.\tools\T2Audio-Setup.ps1 -Action ResetGuard
.\tools\T2Audio-Setup.ps1 -Action Uninstall
```

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
