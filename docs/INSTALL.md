# Installing T2AudioPort for daily use

> **Status:** experimental. Persistent mode and the installer were built on 2026-10-28.
> **Not yet tested on hardware**; the test plan is in `GEMINI.md` ("▶ NEXT RUN").
> The sound path (`SpeakerDsp=3`, preset "Как на Mac" / "Like on a Mac") was verified by the owner on 1.0.11.0.
>
> The installer's menus and messages are in Russian. This guide gives the English meaning.

## Requirements

* A MacBookPro16,1 running Windows 10/11 x64, with Apple's Boot Camp driver (`AppleAudio`) installed.
* `testsigning` on (`bcdedit /set testsigning on`, reboot; Secure Boot off in the Startup Security Utility).
* A signed package in `packaging\`: `T2AudioMiniport.inf`, `T2AudioMiniport.sys`, `t2audiominiport.cat` (build and signing: `GEMINI.md` §2), and the project's test certificate in the trusted stores.

## Install

PowerShell **as Administrator**, from the repository root:

```powershell
.\tools\T2Audio-Setup.ps1
```

The menu offers:

1. **Install / update** («Установить / обновить»): asks for a preset, binds the driver to the device, turns persistent mode on, unmutes the T2 speakers and sets the volume to 30 %.
2. **Change the sound preset** («Сменить пресет звука»): restarts the device (sound drops for a couple of seconds).
3. **Reset the crash guard** («Сбросить защиту от падений»): if the driver stopped starting after two failures.
4. **Restore the Apple driver** («Вернуть драйвер Apple»): removes T2AudioPort; the device goes back to `AppleAudio`.

Without the menu:

```powershell
.\tools\T2Audio-Setup.ps1 -Action Install -Preset macos
.\tools\T2Audio-Setup.ps1 -Action Preset -Preset bright
.\tools\T2Audio-Setup.ps1 -Action Status
.\tools\T2Audio-Setup.ps1 -Action ResetGuard
.\tools\T2Audio-Setup.ps1 -Action Uninstall
```

## Presets

| Preset | Contents | Purpose |
|---|---|---|
| `macos` ★ | `SpeakerDsp=3`, `DspGainDb=6` | Apple's EQ for the 6 speakers, virtual bass, compressor. **Verified by the owner.** |
| `bright` | `macos` + `TweeterTrimDb=3` | More "air", if the sound seems muffled |
| `loud` | `SpeakerDsp=4`, `DspGainDb=9`, `WooferLimitDb=3` | Louder; woofer rattle protection |
| `maxvolume` | `SpeakerDsp=4`, `DspGainDb=6`, `WooferLimitDb=6` | Loud music with bass, without hum |
| `macos-precise` | `macos` + `CompStyle=1` | The compressor measures the level like macOS (RMS, soft knee): gentler on peaks |
| `even` | `macos` + `DspAutoGain=1` | Even loudness (K-weighted, LUFS): quiet recordings up to +6 dB louder, loud ones up to −12 dB quieter |
| `bass` | `SpeakerDsp=4`, `WooferLimitDb=2`, `BassDb=4`, `CompStyle=1` | More bass at medium volume, with rattle protection |
| `voice` | `macos` + `BassDb=-3`, `ClarityDb=4`, `DspAutoGain=1` | Podcasts and calls: clearer speech, even loudness |
| `movie` | `macos` + `BassDb=2`, `ClarityDb=2`, `DspAutoGain=1`, `CompStyle=1` | Films: even loudness, dialogue and bass a little up |
| `night` | `SpeakerDsp=4`, `DspGainDb=3`, `WooferLimitDb=6`, `DspAutoGain=1`, `BassDb=-4`, `ClarityDb=2`, `CompStyle=1` | Quiet and even: less bass and peaks, speech stays clear |
| `warm` | `SpeakerDsp=2` | No compressor: more dynamics, slightly quieter |
| `natural` | `SpeakerDsp=1` | Apple's EQ and crossover only |
| `raw` | `SpeakerDsp=0` | No processing, for comparison (flat, no bass) |

Only `macos` is verified on hardware; the others are starting points for
listening. `macos-precise`, `bass`, `voice`, `movie` and `night` use the
parameters added in 1.0.12.0 (`BassDb`, `TrebleDb`, `ClarityDb`, `CompStyle`);
a 1.0.11.0 driver ignores them.

### One side quieter than the other

`.\tools\T2Audio-Setup.ps1 -Action Status` shows the T2 endpoint's per-channel
levels (`L=… R=…`) and warns if they differ. `-Action Balance -Volume 0.3`
sets both channels equal (Windows keeps a balance per device, under Sound
settings → device properties → Levels → Balance, even across reinstalls).
`-Action Install` does the same at the end.

### Experimental features (driver 1.0.13.0+, off by default, not yet verified on hardware)

```powershell
.\tools\T2Audio-Setup.ps1 -Action Features -Headphones 1       # headphone output + jack tracking + speaker auto-mute
.\tools\T2Audio-Setup.ps1 -Action Features -Microphone 1       # built-in microphones
.\tools\T2Audio-Setup.ps1 -Action Features -ResumeAfterSleep 1 # sound comes back after sleep by itself
.\tools\T2Audio-Setup.ps1 -Action Features -ExtraFormats 1     # 16/24-bit for exclusive-mode apps
.\tools\T2Audio-Setup.ps1 -Action Features -Headphones 0 -Microphone 0 -ResumeAfterSleep 0 -ExtraFormats 0   # all off
```

Menu item 6 does the same interactively; `-Action Status` lists what is on. The
registry values behind them are described in `docs/DRIVER_GUIDE.md` §8.3
(`HeadphoneOutput`, `JackEvents`, `HeadphoneAutoMute`, `MicInput`,
`ResumeAfterSleep`, `ExtraFormats`).

### Tone on top of a preset

`-Action Tune` (menu item 5) changes only the tone values and keeps the rest
of the preset. Whole dB from −12 to +12; a parameter left out is not changed.

```powershell
.\tools\T2Audio-Setup.ps1 -Action Tune -BassDb 3 -TrebleDb -2 -ClarityDb 1
.\tools\T2Audio-Setup.ps1 -Action Tune -CompStyle 1
```

`BassDb` is a shelf at 150 Hz, `TrebleDb` a shelf at 6 kHz, `ClarityDb` a peak
at 2.5 kHz (speech). Choosing a preset again resets all four. After a change the
installer restarts the device, as with `-Action Preset`. `-Action Status` shows
the current tone.

The values live in `HKLM\SYSTEM\CurrentControlSet\Services\T2AudioMiniport\Parameters`
(each one is described in `docs/DRIVER_GUIDE.md` §8.3).

## Crash guard

Before starting a hardware session the driver sets the registry flag `CrashGuardArmed=1` (and flushes it to disk). It clears the flag on a clean end: device stop, a transition to D1–D3 (idle, sleep, hibernation), Windows shutdown. If the flag is still set at boot, the previous session was cut short (BSOD, hang, power button): that is a "strike". **After two strikes in a row the driver stops starting**; there is no sound, but Windows boots normally. Then either:

1. `.\tools\T2Audio-Setup.ps1 -Action Uninstall` to go back to AppleAudio, or
2. find the cause (minidump in `C:\Windows\Minidump`) and run `-Action ResetGuard`.

In Safe Mode the driver never starts.

## Known limitations

* **Headphones are not supported.** Sound always goes to the speakers. The driver only reads the jack state so far (`docs/HEADPHONES.md`). For headphones use Bluetooth/USB, or "Restore the Apple driver".
* Microphones are not supported.
* A Windows Update may bring `AppleAudio` back (it is Microsoft-signed). Run the installer again in that case.
* Sleep and wake with the driver running are not yet tested on hardware.
* Volume is software only (the Windows slider scales the samples); hardware volume is planned (`docs/ROADMAP.md`).
