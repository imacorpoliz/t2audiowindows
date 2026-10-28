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
| `even` | `macos` + `DspAutoGain=1` | Even loudness: quiet recordings up to +6 dB louder, loud ones up to −12 dB quieter |
| `warm` | `SpeakerDsp=2` | No compressor: more dynamics, slightly quieter |
| `natural` | `SpeakerDsp=1` | Apple's EQ and crossover only |
| `raw` | `SpeakerDsp=0` | No processing, for comparison (flat, no bass) |

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
