# Installing T2AudioPort

## Requirements

* **MacBookPro16,1** (MacBook Pro 16", 2019) with Windows 10/11 x64 and standard Boot Camp drivers (`AppleAudio`).
* **Test Mode** (the driver is test-signed): `.\tools\T2Audio-Setup.ps1 -Action Prepare` (menu 8) checks Secure Boot, offers to suspend BitLocker for one reboot, runs `bcdedit /set testsigning on`; then reboot. Secure Boot must be off (macOS Recovery → Startup Security Utility → "No Security"). Windows shows "Test Mode" on the desktop while it is on.
* **Signed Package:** either a release zip (`packaging\`, `tools\`, `certificate\`; made by `tools\Make-ReleasePackage.ps1`) — the installer offers to trust the included test certificate — or your own build in `packaging\`.

## Install

Open PowerShell **as Administrator** in the release folder:

```powershell
.\tools\T2Audio-Setup.ps1
```

Or run directly via commands:

| Action | Command | Notes |
|---|---|---|
| Prepare Windows (test mode, once) | `.\tools\T2Audio-Setup.ps1 -Action Prepare` | Needs one reboot |
| Install driver | `.\tools\T2Audio-Setup.ps1 -Action Install -Preset macos` | Default macOS sound |
| Restore original Apple driver | `.\tools\T2Audio-Setup.ps1 -Action Uninstall` | Clean rollback |
| Sound preset | `.\tools\T2Audio-Setup.ps1 -Action Preset -Preset bright` | 13 tuned presets |
| Tone (bass / treble / clarity) | `.\tools\T2Audio-Setup.ps1 -Action Tune -BassDb 3 -TrebleDb -2` | Live +/- 12 dB |
| Optional features | `.\tools\T2Audio-Setup.ps1 -Action Features -Headphones 1 -Microphone 1` | Feature toggles |
| Sound test | `.\tools\T2Audio-Setup.ps1 -Action SoundTest` | L / R / Both stereo |
| Microphone test | `.\tools\T2Audio-Setup.ps1 -Action MicTest` | Record 5s & playback |
| Doctor | `.\tools\T2Audio-Setup.ps1 -Action Doctor` | System diagnostics |
| Reset crash guard | `.\tools\T2Audio-Setup.ps1 -Action ResetGuard` | Clears strike counter |

`-Force` skips confirmation prompts.

To apply the current registry settings without changing them, use
`-Action Restart`. Feature switches restart the device once after all selected
changes; audio is briefly interrupted.

## Presets

| Preset | Description |
|---|---|
| `macos` ★ | Apple-style EQ for the 6 speakers, virtual bass, multiband compressor. **Recommended default.** |
| `macos-precise` | Same as macos, but compressor measures like macOS (RMS, soft knee). |
| `bright` | Tweeters +3 dB: more air and clarity. |
| `loud` | More make-up gain (+9 dB) and woofer rattle protection. |
| `maxvolume` | Woofers limited 6 dB lower, for loud music with bass. |
| `even` | Slow loudness autogain: quiet tracks up, loud ones down. |
| `bass` | Bass +4 dB shelf with rattle protection. |
| `voice` | Clarity +4 dB, bass -3 dB for podcasts, YouTube, and meetings. |
| `movie` | Even loudness, dialogue +2 dB, bass +2 dB, precise compressor. |
| `night` | Less bass, fewer peaks, even loudness; quiet late-night listening. |
| `warm` | Rolled-off highs (-2 dB) and warm lower-mids. |
