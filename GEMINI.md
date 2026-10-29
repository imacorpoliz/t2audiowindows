# GEMINI.md — work plan for Gemini (build, signing, hardware tests)

> This file is the instruction set for the Gemini agent that works in this repository on a Windows machine with the WDK and on the test MacBookPro16,1.
> Before starting, read:
> 1. [`docs/DRIVER_GUIDE.md`](docs/DRIVER_GUIDE.md) — how the driver works (hardware, protocol, data path, stages, log);
> 2. the latest dated entry in [`docs/CURRENT_STATE.md`](docs/CURRENT_STATE.md) — the only authoritative status;
> 3. [`docs/ROADMAP.md`](docs/ROADMAP.md) — what is left until the driver is fully functional;
> 4. [`docs/analysis/RE_speaker_layout_and_clock.md`](docs/analysis/RE_speaker_layout_and_clock.md) — speaker layout and the T2 clock;
> 5. [`docs/REVIEW_20261028.md`](docs/REVIEW_20261028.md) — what changed after Stage D, problems found, quick rollbacks.

---

## ▶ NEXT RUN — follow the steps in order (updated 2026-10-28: installer and persistent mode)

**Where we are:** sound with `SpeakerDsp=3` is approved (1.0.11.0). Added for daily use, **not yet tested on hardware**:
* **persistent mode** (`Parameters\EnableProduction=1`): the driver starts on every boot without the agent, protected by the crash guard (`src/T2AudioCrashGuard.h`);
* **installer with presets** `tools/T2Audio-Setup.ps1` (`docs/INSTALL.md`);
* **headphone jack read** at probe time (`docs/HEADPHONES.md`, step 1);
* **lookahead limiter** on every DSP level (instead of the instant one; sound is delayed by 0.5 ms) and **autogain** `DspAutoGain` (preset `even`);
* **groundwork for headphones and microphones** (commit 40c1d6d), all off by default: `ExtendedProbe` (a detailed read-only log of every T2 audio device), `JackEvents` (headphone plug/unplug events), `HeadphoneAutoMute` (speakers go silent while headphones are plugged in);
* **closer lsp post-processing and tone controls** (the commit after 08182f6), all off or flat by default, so `macos` sounds exactly as approved: `CompStyle=1` (compressors with RMS detection and a soft knee, as in lsp `mb_compressor`), K-weighted (LUFS) autogain for `DspAutoGain=1`, `BassDb`/`TrebleDb`/`ClarityDb` (−12…+12 dB), five new installer presets (`macos-precise`, `bass`, `voice`, `movie`, `night`) and `-Action Tune`.

Verifying all of this is the goal of the session. The agent `T2Audio-TestAgent.ps1` is not needed for the install; capture the kernel log with DbgView (`tools/Capture-T2AudioLog.ps1` or DbgViewCLI by hand) from the very start of each step.

The installer's UI is in Russian. The strings quoted below are what it prints; the English meaning is given in brackets.

**Step 0 (FIRST, owner report 2026-10-28): "left sounds normal, right is quiet"** («слева звук нормальный а справа тихий»).
Claude checked on the host: the DSP is exactly symmetric for every preset (identical L/R input → 0.00 dB L/R difference at 60 Hz…8 kHz), and the installer only set the master volume. So the cause is in Windows, the copy path or the hardware. Find which, in this order, with the driver installed (persistent mode), DbgView capturing, and the volume moderate:
1. **Windows balance.** Update to the latest `diagnostics`, run `.\tools\T2Audio-Setup.ps1 -Action Status` and copy the line «Громкость T2: mute=… volume=… L=… R=…» [T2 volume]. If `L` and `R` differ, the installer warns. Then `.\tools\T2Audio-Setup.ps1 -Action Balance -Volume 0.3`: it prints the balance before and after and sets both channels equal. Ask the owner: is it even now? If yes, this was the cause, report and continue with step 1.
2. **Also check by hand:** Sound settings → the T2 speakers → Properties → Levels → Balance; Enhancements / Spatial sound off; is Equalizer APO (`SpeakerDSP-APO` from `mbp`) installed on this device? If yes, that is a bug (double processing); uninstall it from the T2 device and retest.
3. **Which side and which speaker.** Play a tone on one channel of the stereo endpoint (moderate level, 1 kHz, then 200 Hz):
   ```powershell
   .\tools\Test-WasapiTone.ps1 -Channel 0 -Frequency 1000 -Seconds 3 -Amplitude 0.1
   .\tools\Test-WasapiTone.ps1 -Channel 1 -Frequency 1000 -Seconds 3 -Amplitude 0.1
   .\tools\Test-WasapiTone.ps1 -Channel 0 -Frequency 200 -Seconds 3 -Amplitude 0.1
   .\tools\Test-WasapiTone.ps1 -Channel 1 -Frequency 200 -Seconds 3 -Amplitude 0.1
   ```
   The owner says for each: same loudness left vs right? 1 kHz is mostly the tweeter + woofers, 200 Hz only the woofers. Quiet right at 200 Hz only → a right woofer; at 1 kHz only → the right tweeter.
4. **DSP or not.** `-Action Preset -Preset raw` (no DSP, everything else the same), repeat 3. Then `-Action Preset -Preset natural`, repeat 3. Then back to `macos`. Raw also quiet right → not the DSP (copy path, mapping or hardware). Only DSP presets → a DSP bug for Claude.
5. **Hardware reference.** If possible, boot macOS and play the same mono test (or any centred voice): is the right side normal there? If macOS also has a quiet right side, it is the speaker.
6. Report: `docs/logs/EXCERPT_*_balance.md` with the `Status`/`Balance` output, the answers to 3–5 (owner's words verbatim with a translation), `mode parameters` and `run summary` lines (`dspLimited`, `maxCopyUs`). Do not change code.

**Step 1. Build 1.0.12.0** from the current `diagnostics` (the latest commit, with the tone controls; CI step "Parse tools and check installer presets" green).
1. CI is green.
2. Build and sign as in §2. Put `.sys`, `.cat` and `.inf` with `DriverVer` 1.0.12.0 into `packaging\`, record the SHA256.
3. Before installing, the device is on AppleAudio and `Parameters` is empty.

**Step 2. Install with the installer** (sound allowed):
```powershell
.\tools\T2Audio-Setup.ps1 -Action Install -Preset macos
```
PASS if:
* the output contains «T2AudioPort установлен и работает» [T2AudioPort installed and running] and «Динамики T2: Mute снят, громкость 30 %» [T2 speakers: unmuted, volume 30 %];
* `-Action Status` shows the T2AudioPort driver, persistent mode on, preset «Как на Mac» [Like on a Mac], crash guard OK;
* the kernel log contains `persistent mode active`, `crash guard armed` and the line `BCE probe: headphone jack device … : PLUGGED/unplugged` (copy it verbatim).

The owner listens to music for 2–3 minutes: does it sound like listening session #3?

**Step 3. Reboot — the main crash-guard test.**
1. Ordinary Windows restart.
2. After boot, `-Action Status`: the T2AudioPort driver started by itself; **«Защита от падений: в порядке»** [crash guard: OK], i.e. `CrashGuardStrikes` = 0.

If an ordinary restart produced a strike (strikes = 1), a clean shutdown does not clear the flag. Stop and report to Claude: the log lines about `crash guard` and `PowerChangeState` before the shutdown.

**Step 4. Headphones and microphones groundwork** (sound allowed; headphones give no sound yet, that is expected).

Set the parameters with PowerShell (as administrator). `-Action Preset` only rewrites the preset values, so these stay, and it restarts the device:
```powershell
$p = 'HKLM:\SYSTEM\CurrentControlSet\Services\T2AudioMiniport\Parameters'
```

4a. **Read at probe.** With headphones unplugged, `-Action Preset -Preset macos`: the log has `BCE probe: headphone jack device … : unplugged`. Plug headphones in, `-Action Preset -Preset macos` again: `PLUGGED`. Unplug.

4b. **Extended probe** (read-only):
```powershell
Set-ItemProperty $p ExtendedProbe 1 -Type DWord
.\tools\T2Audio-Setup.ps1 -Action Preset -Preset macos
```
PASS: `mode parameters: … ExtendedProbe=1`, then lines `BCE detail: device 0x… output streams: …`, `… input streams: …`, `… stream 0x… pft: rate=… ch=… bits=…`, `… outp ltnc/saft/jack=…`, `… inpt …`, and `BufferStruct device[i] '…' in=… out=…` with `buffer[0]: address=… size=…`. **Copy every `BCE detail` and `BufferStruct` line verbatim into the report**: they are the input for the headphone and microphone endpoints. Speaker sound must be unchanged; play music for 30 s to confirm.

4c. **Jack events** (the driver subscribes to plug/unplug):
```powershell
Set-ItemProperty $p JackEvents 1 -Type DWord
.\tools\T2Audio-Setup.ps1 -Action Preset -Preset macos
```
PASS:
* in the log, `jack listener on device 0x… : registered (0x00000000 …)`;
* then, **without restarting anything**, plug headphones in and out 3 times, ~5 s apart. Each plug and each unplug gives `VHCI PROPERTY_CHANGED #n dev=0x… scope=0x6F757470 sel=0x6A61636B` and `headphone jack PLUGGED (was unplugged)` / `unplugged (was plugged)`;
* speaker sound is unaffected.

If the listener line says `FAILED`, or no `PROPERTY_CHANGED` arrives: record it, set `JackEvents` back to 0, go on with step 5.

4d. **Auto-mute** (only if 4c passed):
```powershell
Set-ItemProperty $p HeadphoneAutoMute 1 -Type DWord
.\tools\T2Audio-Setup.ps1 -Action Preset -Preset macos
```
Play music on the speakers. Plug headphones in: the speakers go silent (and nothing in the headphones, which is expected). Unplug: music returns from the speakers within a moment, without a crash or a stuck stream. Stop playback and record `jackMutedFrames=` from `run summary` (> 0).

4e. **Back to defaults** (unless the owner wants to keep auto-mute):
```powershell
Set-ItemProperty $p HeadphoneAutoMute 0 -Type DWord
Set-ItemProperty $p JackEvents 0 -Type DWord
Set-ItemProperty $p ExtendedProbe 0 -Type DWord
.\tools\T2Audio-Setup.ps1 -Action Preset -Preset macos
```

**Step 4f. Jack polling retest** (after Claude's jack-poll commit; rebuild 1.0.12.0 from the latest `diagnostics`; sound allowed, headphones give no sound yet).
1. With headphones **plugged in before boot**: reboot, then the probe line `BCE probe: headphone jack device 0x25: …` — does it say `PLUGGED`? (This shows whether `'jack'` reflects the jack at all.)
2. `Set-ItemProperty $p JackEvents 1 -Type DWord`, `-Action Preset -Preset macos` (restarts the device). Log: `jack poll started (every 1000 ms)` and `headphone jack … (was …, read #1)`.
3. Unplug / plug 3 times, ~5 s apart, music playing. PASS: a `headphone jack PLUGGED` / `unplugged` line within ~1 s of each change; no audio dropouts; `run summary` normal. Also copy any `VHCI message #… msg=…` and `PROPERTY_CHANGED` lines.
4. If 3 passed: `HeadphoneAutoMute=1`, restart, plug in → speakers silent within ~1 s; unplug → sound back. `jackMutedFrames` in `run summary`.
5. Disable/enable the device once in Device Manager while `JackEvents=1` (tests the poll stop path): no BSOD, log has no `re-read still running`.
6. Back to `JackEvents=0`, `HeadphoneAutoMute=0`.
7. **Mute after reboot:** in step 3 `Status` showed `mute=True` after the reboot. Ask the owner whether they muted it. If not, reboot once more and record `Status` right after login (before touching volume).

**Step 5. Presets.** `-Action Preset -Preset bright`, then `loud`, `maxvolume`, `even`, `macos`. After each one the log line `mode parameters` shows the expected values (`even` has `DspAutoGain=1`). The owner gives a short verdict by ear:
* `maxvolume` and `loud`: a bass-heavy track at 100 %. Is there hum, or harshness on peaks? (The limiter now looks ahead, so harshness on peaks should be lower.)
* `even`: first a quiet recording (old, classical, a podcast), then a loud one (modern pop/rock). Is the level between them more even? Any "pumping" (volume noticeably drifting up and down in pauses and after hits)? Record `autoGainDb10=` from `run summary` (tenths of a dB; e.g. `-45` = −4.5 dB). The autogain now measures LUFS (K-weighted), so bass-heavy tracks should no longer be turned down more than vocal ones.

**Step 5a. New presets and tone** (sound allowed; same log capture). For each preset, after `-Action Preset -Preset <name>` the `mode parameters` line must show exactly the values from the table in `docs/INSTALL.md` (`BassDb=-3` etc. are printed signed). Owner verdict by ear, one line each:
1. **`macos-precise` vs `macos`** (A/B, the same track, 30 s each, same volume): is there a difference? Which one is closer to macOS? Softer peaks, steadier bass? This decides whether `CompStyle=1` becomes the default (ROADMAP §7.2).
2. **`bass`**: a bass-heavy track at 50 % and at 100 %. More bass than `macos`? Any rattle or hum at 100 %?
3. **`voice`**: a podcast or a video call recording. Is speech clearer than with `macos`? Too sharp?
4. **`movie`**: a film scene with quiet dialogue and loud effects. Dialogue understandable without turning the volume up for it?
5. **`night`**: the same film scene at a low volume. Even, no booming?
6. **`-Action Tune`**: from `macos`, run `.\tools\T2Audio-Setup.ps1 -Action Tune -BassDb 6 -TrebleDb -6`. `-Action Status` must show «Тембр: бас +6 дБ, высокие -6 дБ, разборчивость 0 дБ, компрессор быстрый» [tone: bass +6 dB, treble −6 dB, clarity 0 dB, compressor fast] and the log `BassDb=6 TrebleDb=-6`. The sound must get audibly darker. Then `-Action Tune -BassDb 0 -TrebleDb 0`: back to the `macos` sound, `Status` shows the preset «Как на Mac» again. Also check the menu (no parameters, item 5) once: Enter on every question changes nothing.
7. **CPU check, everything on:**
   ```powershell
   .\tools\T2Audio-Setup.ps1 -Action Preset -Preset night        # SpeakerDsp 4, autogain, CompStyle 1
   .\tools\T2Audio-Setup.ps1 -Action Tune -BassDb 12 -TrebleDb 12 -ClarityDb 12
   ```
   Play music 1 minute **at a low volume** (+12 dB on everything is loud and harsh, this is a load test, not a preset). Record `maxCopyUs` from `run summary` (host estimate ~65 µs per tick; must stay well under 100 µs on the Mac, otherwise report it), and that there were no dropouts. Then `-Action Preset -Preset macos`.

If anything sounds broken (crackle, silence, distortion at moderate volume) in 5a: `-Action Preset -Preset macos`, note which preset, continue with step 6. Do not change code.

**Step 6. Sleep** (only if the owner agrees to this step):
1. Play music, put the Mac to sleep for 1–2 minutes, wake it.
2. Did the sound come back? No BSOD?
3. After wake, `-Action Status`: crash guard OK.

On a BSOD, after the reboot `-Action Status` shows a strike; add the minidump to the report.

**Step 7. Back to Apple:**
```powershell
.\tools\T2Audio-Setup.ps1 -Action Uninstall
```
PASS: «Устройство снова на AppleAudio» [device is back on AppleAudio], `Status` shows AppleAudio.

**Step 8. Report:**
1. `docs/logs/EXCERPT_*.md`: installer output at every step, log lines (`persistent mode`, `crash guard`, `headphone jack`, `mode parameters`, `run summary` with `maxCopyUs` (also from 5a.7), `autoGainDb10` and `jackMutedFrames`, the `mode parameters` line of each preset in 5/5a, **all `BCE detail` and `BufferStruct device` lines from 4b**, `jack listener` and `PROPERTY_CHANGED` lines from 4c), the owner's words (verbatim, with a translation).
2. An entry in `docs/CURRENT_STATE.md`.
3. A PR into `diagnostics`.

**If something does not work:** `.\tools\T2Audio-Setup.ps1 -Action Uninstall`, logs, an entry in `CURRENT_STATE.md`, tell the owner. Do not fix code; "proven" means verified only.

> **Installer language (2026-10-29):** `tools/T2Audio-Setup.ps1` is now in **English** (owner's request, release preparation). Older steps below quote its former Russian output with an English translation in brackets; the English messages are those translations (e.g. «T2AudioPort установлен и работает» is now "T2AudioPort is installed and running", «Защита от падений: в порядке» is "Crash guard: OK", the confirmation word is `INSTALL`).

---

## ▶ NEXT RUN, part 3 — everything that needs the Mac (2026-10-29)

**State:** speakers, headphones, microphone, sleep and 16/24-bit work on 1.0.13.4 (owner: «все работает шикарно»; sleep: «проверку после сна я сделал все работает»). What is left needs the Mac, AppleAudio.sys under REA/Ghidra, or the owner's ears. **Order: B → J → R → Q → V → H.** One task at a time; DbgView from the start of every hardware step; each task ends with a report file and an entry in `docs/CURRENT_STATE.md`; a PR into `diagnostics` per task group (never push to `diagnostics` directly; do not change driver code yourself — describe what you found and Claude implements it, as with the microphone).

Safety rules for all of part 3: on a BSOD or power-off stop, run `-Action Status`, collect the minidump / Event 41, switch the feature off, report. Static RE has no hardware risk and can run in parallel.

### B. Build 1.0.13.5 and check the new installer (30 min)
1. Build from the latest `diagnostics` (DriverVer 1.0.13.5), sign, all 7 host tests.
2. The installer is now **English** (messages, menu, presets; confirmation word `INSTALL`). `-Action Install -Preset macos`: check the text, that the T2 **speakers** endpoint (not the headphones) gets the volume (`T2 speakers: unmuted, volume 30 %, L/R balance evened …`), and `-Action Status` (`Status`, `Tone`, `Crash guard: OK`, the feature list with on/off).
3. `-Action Features -Headphones 1 -Microphone 1 -ResumeAfterSleep 1`: all three on; the microphone now also sets `MicStartIo=1`.
4. New `-Action UpdateGuard`: `-Block 1` (answer y), check `HKLM\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\ExcludeWUDriversInQualityUpdate = 1` and `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\DriverSearching\SearchOrderConfig = 0`; `-Block 0` restores the previous values (record them before and after). Leave it as the owner prefers.
Report: `docs/logs/EXCERPT_<date>_partB_installer.md`.

### J. Automatic headphone ↔ speaker switching (the owner's only complaint)
The driver already reports jack changes to Windows (`src/Topology.c`), but the T2 jack read stays 0 and no PROPERTY_CHANGED arrives.
* **J1 — jack experiment (hardware, read-only, 10 min):** `Set-ItemProperty $p JackProbeAll 1 -Type DWord` (JackEvents is already 1), restart the device. Play on the **speakers**, plug/unplug the headphones 3× (~5 s apart, note the times); then play on the **Headphones** endpoint and repeat. Copy every `jack probe #… <name> (dev … obj …): value (was …)` line and every `VHCI message #… msg=…` / `PROPERTY_CHANGED` line. Then `JackProbeAll 0`.
* **J2 — static RE of AppleAudio.sys → `docs/analysis/RE_jack.md`:**
  1. Every `PROPERTY_LISTENER` (msg 11, `0x1400042C4`) AppleAudio sends: exact bytes (object, element, scope, selector), target device ids, and when (start, after START_IO, …).
  2. The `0x222020` handler (`0x1400012C7`–`0x140001332`): which device/object/scope/selector it accepts; what reads the toggled flags `[g+0x38]`/`[g+0x39]`; what the callbacks `[g+0x08]([g+0x10])`, `[g+0x18]([g+0x20])` do (switch the render ring inside one endpoint? raise a KS event? change topology?).
  3. `0x14000330C` (GET_PROPERTY `'jack'` for `'outp'`): request bytes and how the reply is read (offset, size, bit).
  4. How many render endpoints AppleAudio exposes on this Mac and their topology pins / jack descriptions (one "Speakers" that follows the jack, or two).
  5. Any other message AppleAudio sends to "Codec Output" 0x25 or "Codec Input" 0x44 at start (SET_PROPERTY selectors, other listeners).
* **J3 — after Claude's change (later):** plug/unplug while playing: Windows switches the default output within ~1 s both ways, speakers silent while the headphones are in, no clicks; 20 cycles; report.

### R. Release package (1 h)
`R1`: export the **public** test certificate (`Export-Certificate -Cert Cert:\LocalMachine\My\<thumbprint> -FilePath T2AudioPort-test.cer`; never the .pfx), run `.\tools\Make-ReleasePackage.ps1 -CertificatePath T2AudioPort-test.cer`. It must refuse an unsigned file and a certificate with a private key (try once with the .pfx-backed cert to see the refusal). Unzip `dist\T2AudioPort-1.0.13.5.zip` into a new folder, run `tools\T2Audio-Setup.ps1 -Action Install -Preset macos` **from there**; on a machine where the certificate is already trusted the prompt does not appear — to test the prompt, remove the certificate from `LocalMachine\TrustedPublisher` first (re-add it after). Check `SHA256SUMS.txt` with `Get-FileHash`. Do not publish the zip.

### Q. Quality comparisons with the owner (needs the owner's ears and a reboot into macOS / AppleAudio)
* **Q1 — microphone vs Boot Camp:** the same 20 s text, same position, same room, recorded with Voice Recorder (or `tools\Record-WasapiMic.ps1`) on (a) T2AudioPort and (b) AppleAudio (`-Action Uninstall`, record, `-Action Install`); if possible (c) macOS. Report peak/RMS/noise floor in silence (first 2 s) for each and the owner's verdict on clarity and noise (verbatim). Keep the WAVs out of git (attach paths only).
* **Q2 — speakers vs macOS:** the owner plays the same 3 tracks (one with vocals, one with bass, one quiet acoustic) at the same volume in macOS and in Windows (`macos` preset); verdict per track: bass, brightness, loudness, harshness. Any "Windows is duller/thinner/louder" goes to Claude as a preset adjustment.
* **Q3 — low volume:** at 10–20 % volume, does the bass disappear compared with macOS? (input for a loudness feature).

### V. Robustness (needs time, little attention)
* **V1 — Driver Verifier (only with the owner's permission; it can cause a BSOD by design):** `verifier /standard /driver T2AudioMiniport.sys`, reboot, then 15 min of music + 2 min of recording + headphones + sleep/wake once. A Verifier bugcheck is a real driver bug: minidump + `!analyze -v` text to Claude. At the end `verifier /reset`, reboot. With the crash guard: two Verifier crashes stop the driver from loading — that is expected; `-Action ResetGuard` after the report.
* **V2 — long run:** 60 min of music on the speakers, then 10 min on the headphones, with a 10 min recording in parallel. `run summary` / `mic run summary`: `reAims`, `gaps>10ms`, `skips`, `maxRenderUs`, `slowRenders` (expect `slowRenders` ≤ 2 per run, `skips=0`). Owner: any crackle, drift, dropout?
* **V3 — reboot loop:** 5 ordinary reboots and 2 shutdown/power-on cycles with everything on: `CrashGuardStrikes` stays 0, all three endpoints come back every time.

### H. Static RE for the next features (no hardware risk; any time)
* **H1 — hardware volume / mute:** BCE msg 28 (`controls`) in AppleAudio: the object classes `'levl'`, `'slct'`, `'togl'`, their properties (range, steps, dB mapping), whether AppleAudio maps the Windows volume slider to a T2 control (KS volume/mute node in its topology?), and the exact SET_PROPERTY messages. → `docs/analysis/RE_volume.md`.
* **H2 — headset microphone:** how AppleAudio handles "Codec Input" 0x44 (stream 0x45, 1 ch 32-bit, flags 0xC): msg 18 for it, `'nsrt'`, its BufferStruct entry, and how it switches between the built-in array and the headset mic (jack). → `docs/analysis/RE_headset_mic.md`.
* **H3 — sample rates:** whether AppleAudio ever sets `'nsrt'` to 44100/96000 on the speaker or the codec, and what else changes (ring size? timestamps?). → `docs/analysis/RE_sample_rate.md`.

---

## ▶ NEXT RUN, part 2 — new features (only after part 1 passed; all off by default)

Claude added (2026-10-28, commits a2c6227 → the latest `diagnostics`), **source only, CI green, never run on hardware**:
* **headphone endpoint** "T2 Audio Headphones" (`HeadphoneOutput`), with jack state reported to Windows (`JackEvents`) and speaker auto-mute;
* **built-in microphones** "T2 Audio Microphone" (`MicInput`): a host ring registered with the T2 (writes BufferStruct, sends msg 18 — a **new kind of hardware access**), capture with kaiT2en's mic DSP;
* **resume after sleep** (`ResumeAfterSleep`);
* **16/24-bit formats** (`ExtraFormats`).

The installer switches them: `.\tools\T2Audio-Setup.ps1 -Action Features -Headphones 1` (sets HeadphoneOutput, JackEvents, HeadphoneAutoMute), `-Microphone 1`, `-ResumeAfterSleep 1`, `-ExtraFormats 1`; `0` turns each off; `-Action Status` lists them. The installer's output: «Наушники (выход 3,5 мм, автопереключение)» [headphones], «Встроенные микрофоны» [built-in microphones], «Звук после сна без перезапуска» [sound after sleep without restart], «Форматы 16/24 бит» [16/24-bit formats], «вкл/выкл» [on/off].

Rebuild 1.0.13.0 from the latest `diagnostics` (the INF gained interfaces, so the package must be re-signed and reinstalled: `-Action Install -Preset macos`). Check first that part 1 still passes with everything off (speakers play, reboot OK). **One feature at a time; DbgView from the start; on any BSOD: stop, minidump, `-Action Features -Headphones 0 -Microphone 0 -ResumeAfterSleep 0 -ExtraFormats 0`, report.**

**P0. Jack experiment** (read-only, no sound needed; ~5 minutes). Step 4f showed `'outp'/'jack'` of "Codec Output" stays 0. The driver can now read the jack at 8 addresses each second and log every change:
```powershell
$p = 'HKLM:\SYSTEM\CurrentControlSet\Services\T2AudioMiniport\Parameters'
.\tools\T2Audio-Setup.ps1 -Action Features -Headphones 1      # HeadphoneOutput + JackEvents + HeadphoneAutoMute
Set-ItemProperty $p JackProbeAll 1 -Type DWord
.\tools\T2Audio-Setup.ps1 -Action Preset -Preset macos         # restarts the device
```
1. Log: `jack probe #1 <name> (dev … obj …): <value>` for each address (`out dev outp el0`, `out dev inpt el0`, `out dev glob el0`, `out dev outp el1`, `out dev outp el2`, `out stream outp`, `out stream glob`, `in dev inpt el0`). Negative values are failed reads.
2. Play music on the **speakers**, then plug / unplug the headphones 3 times, ~5 s apart. Copy **every** `jack probe` line (only changes are logged) and note the times of plugging.
3. Repeat while playing on the **headphone endpoint** (P1 below, at 10 %): the T2 may only track the jack while the codec runs.
4. If any line changes with the jack: `jack read works (first PLUGGED)` appears when the main read follows; report which address it was. Afterwards `Set-ItemProperty $p JackProbeAll 0 -Type DWord`.

**P1. Headphones** (sound allowed; **headphones on the desk first, not on the ears**, volume 10 %).
1. `-Action Features -Headphones 1`. Log: `headphone endpoint registered`, `headphone stream 0x26: shift=8 …`, `audio path ENABLED for headphones 0x25 ring 0x190000+0x20800`, `jack poll started`. If the stream line says `format unsupported` or the endpoint is NOT registered: record, turn it off, go on with P2.
2. Windows Sound settings: a new output «T2 Audio Headphones». It is **always shown** until a jack read has once returned "plugged" (`jack read works`); only then does Windows see it as unplugged/plugged and switch automatically. With the jack read stuck at 0 (step 4f), select it by hand.
3. Plug in, select it, play a 1 kHz tone at 10 % (`tools\Test-WasapiTone.ps1` on that device, or any music). Is it audible in the headphones, both sides? Speakers silent (auto-mute)? Then music at a comfortable level: owner verdict (quality, noise, crackle).
4. Unplug while playing: speakers take over (Windows switches the default back) within ~1–2 s; plug in again.
5. `run summary` for the headphone stream (reAims, gaps, `maxCopyUs`, `maxRenderUs`, `slowRenders`); `jack change signalled to Windows` lines.

**`run summary` now has `maxRenderUs=` and `slowRenders=N/M`** (render + ring copy alone, without the clock-follow logging; passes over 500 µs out of all passes). In step 4f `maxCopyUs` was 2.1–2.7 ms: Claude's reading is that this is the `T2 clock: re-aimed …` DbgPrint inside the measured section (DbgView attached), not the copy. Record both values in every report; `maxRenderUs` well under 500 µs and `slowRenders` near 0 confirm it.

**P2. Microphones — REVISED after the power-off of 2026-10-29** (`EXCERPT_20261029_part2_mics.md`). Do not set `MicStartIo=1` until P2-RE is done and Claude has confirmed the sequence.

*What changed (commit after b05dfbd):* msg 18 is now sent in AppleAudio's 837-byte form (stream id 0x22, count 1, range 1/1 — your finding); the mic ring is a DMA common buffer of our PCI device (`IoGetDmaAdapter` + `AllocateCommonBuffer`), so the address in BufferStruct is the device-logical one; **and a capture RUN no longer sends START_IO unless `Parameters\MicStartIo=1`** (default 0: the endpoint records silence, no T2 traffic). `-Action Features -Microphone 1` sets only `MicInput`.

**P2-RE (static, AppleAudio.sys with REA/Ghidra, no hardware risk): PASSED (2026-10-29).** Documented in `docs/analysis/RE_mic_capture.md`. Answer each with addresses and disassembly in `docs/analysis/RE_mic_capture.md`:
1. `0x140004344` (msg 18, 837 bytes): are `count=1, 1, 1` really immediates in every path, or is there a loop/branch that writes real ranges (addresses/sizes, page lists) into the 49/50 slots? Who calls it, and when relative to `0x1400046F8` (BufferStruct write), `0x1400044E4` (earlier noted as msg 18 too — what is the difference?) and START_IO (`0x1400048B0`) for the input device?
2. How AppleAudio allocates the input buffers (`0x140001FA4`): `MmAllocateContiguousMemory*`, `AllocateCommonBuffer` (DMA adapter), `MmAllocatePagesForMdl*`? Which addresses go into BufferStruct: `MmGetPhysicalAddress`, the DMA logical address, or MDL PFNs? The table on hardware held 3 buffers (0x3000, 0x20000, …): how are they split?
3. `SET_PROPERTY 'nsrt'` (msg 9, 0x42 bytes, `0x14000445C`): full layout (object = device or stream id? element, scope, selector, data size, the double 48000.0) and whether it is sent for the input device **before** START_IO.
4. The complete ordered list of BCE messages AppleAudio sends when a capture stream goes to RUN and when it stops (any SET_PROPERTY, PROPERTY_LISTENER, msg 18 again, START_IO on which device id: 0x21 or the stream 0x22?).
5. Anything AppleAudio writes into BufferStruct beyond `num_input_streams`, `num_buffers`, `buffers[]` (e.g. the `active`/flag fields at stream `+0x0C`/`+0x10` that the 2026 report mentioned).

**P2-A (safe, MicStartIo=0): PASSED (2026-10-29, 1.0.13.3).** Captured 5 s via Test-WasapiCapture.ps1, BCE Msg 18 & Msg 9 acknowledged 0x0, silent=144488 frames, 0 strikes. See docs/logs/EXCERPT_20261029_part2a_mics.md. rebuild, `-Action Features -Microphone 1`. Log: `mic ring: common buffer va=… logical=0x… physical=0x…` (are they equal? — tells whether DMA remapping is active), `mic SET_INPUT_STREAM_ADDRESS_RANGES dev=0x21 stream=0x22 (837 bytes): 0x00000000`, `mic ring registered`. Start Voice Recorder for 5 s: expected `mic RUN without START_IO (MicStartIo=0)`, a silent recording, **no crash**, `mic run summary: captured=0 silent=…`. Disable/enable the device once.

**P2-B (only after P2-RE matches our sequence and the owner explicitly accepts that the Mac may power off again):** `Set-ItemProperty $p MicStartIo 1 -Type DWord`, restart the device, record 5 s, speak. Expected `mic RUN START_IO: 0x00000000`, `captured` ≈ 48000/s, a voice in the recording. On power-off: after reboot `-Action Features -Microphone 0`, `MicStartIo 0`, report.

**P3. Resume after sleep** (only if the owner allows sleep). `-Action Features -ResumeAfterSleep 1`. Play music, sleep 1–2 min, wake **without touching the player**. Log: `resume after sleep: restarting speaker I/O`, `status=0x00000000`; does the sound come back by itself within ~1 s? With the microphone on as well: `D0: mic ranges re-sent: 0x00000000`. Compare once with `ResumeAfterSleep 0` (expected: silence until the player is restarted).

**P4. 16/24-bit** (`-Action Features -ExtraFormats 1`). Sound settings → T2 speakers → Advanced: the list should now offer 16 and 24 bit. Pick "24 bit, 48000 Hz", play music: same sound as 32-bit? Then back to the default (32 bit). Shared-mode playback must be unaffected.

Report: `docs/logs/EXCERPT_*_part2.md` with the lines above, the owner's verdicts (verbatim, translated), an entry in `CURRENT_STATE.md`, a PR into `diagnostics`. At the end the owner decides which features stay on.

---

## 0. State (as of 2026-10-28 evening; the current state is in the block above and in `docs/CURRENT_STATE.md`)

* The main branch is **`diagnostics`** (the default branch). CI (`.github/workflows/build.yml`): the driver builds with the WDK (Release x64); the host tests pass on MSVC, gcc and clang.
* **Stages B, C and D passed on hardware.** Last Stage D run: build 1.0.7.0, hash `48F336C1…BE4`, `docs/logs/EXCERPT_20261028_043442.md`.
  * No BSOD `0x7E`: `IOCTL 0x222020` is intercepted in the kernel.
  * START_IO/STOP_IO replies are accepted (`msg=1`/`msg=3`, `status=0`).
  * The stream enters RUN, and the T2 clock keeps a lead of 1000 frames.
  * After STOP_IO no more timestamps arrive.
* **Stage E (first sound) passed afterwards** (1.0.8.0), and the DSP levels 2 and 3 were approved by the owner (1.0.10.0, 1.0.11.0). See `docs/CURRENT_STATE.md`.
* **Changes after the Stage D run** (some since verified on hardware; see CURRENT_STATE):
  * `Parameters\OutputAttenuation`: a safety cut on every sample in 6 dB steps. Default is now 1 (−6 dB); 0 disables, maximum 16. Visible in the log: `mode parameters: … OutputAttenuation=1 (-6 dB)`.
  * `tools/Test-WasapiTone.ps1 -EndpointVolume` (up to 0.25): a quiet tone only on an explicitly named endpoint; after the tone, Mute and volume 0 are restored. Fixed 32-bit PCM writing: 16-bit samples used to be written into a 32-bit buffer.
  * `tools/T2Audio-TestAgent.ps1 -AudibleVolume <up to 0.25> -OwnerApprovedAudible`: Stage E. A silent probe always runs first, the tone only afterwards.
  * `Parameters\ClockRateTracking` (default 0): rate tracking against the T2 clock to avoid jumps under drift. Found by the simulation `tests/T2AudioPipelineTest.c`, see §7 E2/E3.
  * Cleanup: old documents moved to `docs/archive/`. `.sys` and `.cat` are no longer committed (§2 A3). DbgViewCLI path: `$env:T2AUDIO_DBGVIEW` or `..\DebugView\`.
  * CI compiles the C# inside `Test-WasapiTone.ps1`, `Test-WasapiRender.ps1` and `T2Audio-Setup.ps1` and runs all host tests. PSScriptAnalyzer, InfVerif and code analysis are report-only.

---

## 1. Hard rules (never break them)

0. **Silence.** People are asleep at home. Until the owner explicitly allows sound:
   * do not run `tools/Test-WasapiTone.ps1` (except the compile check of §7 S1, which plays nothing), music, system sounds, or the Windows "speaker test";
   * do not check AppleAudio "by ear" after a rollback, only the device state (`Get-PnpDevice`);
   * **before any hardware run:** Mute in Windows and volume 0.

   `tools/Test-WasapiRender.ps1` is allowed: it only opens a stream (`Initialize`/`Start`/`Stop`) and writes no samples.
1. **`-BceIo` / `-OwnerApprovedBceIo` only after the owner has written permission for that specific run in the chat.** Never send `START_IO` around the agent: neither with your own script nor through the registry (`EnableBceIo`).
1a. **Sound (`-AudibleVolume`, `-Listen`, `-OwnerApprovedAudible`)** only with the owner's permission for sound. Permission for START_IO does not allow sound. Tone: endpoint volume ≤ 0.25, amplitude ≤ 0.3. In `-Listen` the owner controls the volume; start at 5–10 %. Do not set `OutputAttenuation` below 1 without permission.
2. **Do not change the start gates** in `src/Driver.c`. Autostart only through the installer's persistent mode (`EnableProduction`, with the crash guard) and only with the owner's consent.
3. **Do not put the Mac to sleep** during a stage ≥ 3 session (power handling exists but is not verified) unless the owner allows the sleep test.
4. Raise stages **one at a time**: 1 → 2 → 3 → 5 without START_IO → (with permission) START_IO. Go to the next stage only if the previous one met every criterion.
5. After every run, especially after a failure: `.\tools\T2Audio-TestAgent.ps1 -Mode Recover` (or `T2Audio-Setup.ps1 -Action Uninstall` for an installer setup). In the end the device must be back on **AppleAudio** (Started, no errors), unless the owner decided to keep T2AudioPort installed.
6. Never write "fixed" / "works" in docs and commits without proof from hardware (log, dump, binary hash).
7. Never commit certificates, `.pfx`, private keys, full kernel logs (`docs/logs/capture_*/` is in `.gitignore`), Apple binaries, or REA/Ghidra projects. Only excerpts go into the repository; binaries and large listings go into the private `imacorpoliz/mbp`.
8. Do not push to `diagnostics` directly. Work in your own branch off `diagnostics` (e.g. `gemini/hw-stage3`) and open a PR into `diagnostics`.
9. `-RawChannels` (6 raw channels, **tweeters unprotected**) only to check the layout and speakers, and only with sound allowed by the owner. Nothing below 1 kHz on channels 2/5 (tweeters); both the agent and the tone script enforce this.

---

## 2. Stage A — build and sign a test package

### A1. Build

```powershell
git fetch origin
git checkout -b gemini/hw-stage3 origin/diagnostics

msbuild src\T2AudioMiniport.vcxproj /p:Configuration=Release /p:Platform=x64 /t:Rebuild `
        /p:WindowsTargetPlatformVersion=10.0.26100.0     # or the installed SDK version
# -> src\bin\Release\T2AudioMiniport.sys
```

The project sets `WindowsTargetPlatformVersion 10.0.28000.0`. If that SDK is not installed, pass the installed one with `/p:`, as CI does. Instead of a local build you can take the `T2AudioMiniport-unsigned-<sha>` artifact from GitHub Actions for the same commit.

### A2. Host tests (mandatory before every binary)

```powershell
foreach ($t in 'BceProtocolLogicTest','T2AudioBufferLogicTest','T2AudioSpeakerLogicTest',
               'T2AudioPipelineTest','T2AudioDspTest','T2AudioCrashGuardTest') {
    cl /nologo /W4 /Fe:tests\$t.exe tests\$t.c
    & "tests\$t.exe"; if ($LASTEXITCODE) { throw "$t failed" }
}
```

### A3. Package and signing

1. Copy `src\bin\Release\T2AudioMiniport.sys` into `packaging\`.
2. **Raise `DriverVer`** in `packaging\T2AudioMiniport.inf`: today's date and the next version after the last installed one (e.g. `1.0.12.0`).
3. Sign the `.sys`, regenerate the catalog and sign it with the project's test certificate (SHA1 `CA2DE95D1B065F6D6CFF6C4158DC88E2424ABBCE`, `docs/T2AudioCert_thumbprint.txt`; it must be in `Cert:\CurrentUser\My` and in the trusted stores):

```powershell
$kit = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64"   # your version
cd packaging
& "$kit\signtool.exe" sign /fd SHA256 /sha1 CA2DE95D1B065F6D6CFF6C4158DC88E2424ABBCE `
    /t http://timestamp.digicert.com T2AudioMiniport.sys
& "$kit\makecat.exe" t2audio.cdf                      # -> t2audiominiport.cat
& "$kit\signtool.exe" sign /fd SHA256 /sha1 CA2DE95D1B065F6D6CFF6C4158DC88E2424ABBCE `
    /t http://timestamp.digicert.com t2audiominiport.cat
(Get-FileHash .\T2AudioMiniport.sys -Algorithm SHA256).Hash   # note it: this is -ExpectedSysHash
```

4. Verify the signatures: `& "$kit\signtool.exe" verify /pa /v T2AudioMiniport.sys`, the same for the `.cat`.

**Done when:** the tests passed, `.sys` and `.cat` are signed, the `.sys` hash is recorded.

**Do not commit `.sys` and `.cat`** (they are in `.gitignore` since 2026-10-28). Only `T2AudioMiniport.inf` with the new `DriverVer` and the `.sys` hash in the report go into the repository. Signed packages, if they need to be kept, go into the private `imacorpoliz/mbp`.

---

## 3. Stage B — hardware run, stages 1 → 3 (silent)

### B1. Preparation

```powershell
.\tools\PreInstall-Check.ps1
.\tools\Install-T2AudioDriver.ps1            # only adds the package to the driver store
.\tools\Install-T2AudioDriver.ps1 -Bind      # binds it to the device (may need a reboot, Code 14)
.\tools\T2Audio-TestAgent.ps1 -Mode Status
```

Logs need `DbgViewCLI.exe` (Sysinternals, see `tools/README.md`). Mute and volume 0 before the first run.

**After upgrading from the old 6-channel build** Windows may keep the old endpoint format cached, and the new driver rejects it. The log shows `Validate: 6 channels not offered` and the endpoint does not start. Fix: "Sound → Device properties → Advanced → 2 channels, 32 bit, 48000 Hz". Or reinstall the device (`pnputil /remove-device <InstanceId>`, then `pnputil /add-driver packaging\T2AudioMiniport.inf /install`).

### B2. Runs

```powershell
$h = '<SHA256 from A3>'
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Stage 1 -ExpectedSysHash $h
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Stage 2 -ExpectedSysHash $h
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Stage 3 -ExpectedSysHash $h
.\tools\T2Audio-TestAgent.ps1 -Mode Recover
```

On a BSOD or a hang at any stage, stop. After the reboot: `-Mode Recover`, collect `C:\Windows\Minidump\*.dmp` (`!analyze -v`, stack); only that goes into the report.

### B3. Acceptance criteria (`T2Audio:` log lines)

| Stage | Expected |
|---|---|
| 1 | `diagnostic session active stage=1 … rawChannels=0`, `PcRegisterAdapterPowerManagement status=0x00000000`, `StartDevice SUCCESS`; the WASAPI probe opens the endpoint (format **2 ch / 48000 / 32 bit**); no BSOD |
| 1 | `mode parameters: … TweeterMode=1` |
| 2 | `Resource[0] mapped at … (write-combining\|uncached)`, `GPR read: … signature=0x19870423`, `MapResources SUCCESS: … size=0x61800` |
| 3 | `BCE probe: 5 device(s)`, `uid="Speaker"` for `0x39` |
| 3 | `speaker stream 0x3A pft: rate=48000 id=0x6C70636D flags=0x4 bpp=24 fpp=1 bpf=24 ch=6 bits=24` |
| 3 | `speaker sample shift=8 latency=A+B frames safety=C frames` — **record A, B, C** |
| 3 | `BCE probe done; speaker id 0x39 stream 0x3A format=1` |
| 3 | on shutdown: `PnP minor 0x02/0x04 - releasing hardware`, no BSOD and no PnP hang |
| all | after `-Mode Recover` the device is on AppleAudio, Started |

### B4. Report

Create `docs/logs/EXCERPT_<YYYYMMDD_HHMMSS>.md`:

* `.sys` hash, commit, INF version;
* the commands you ran;
* a log excerpt per stage (`T2Audio:` lines only);
* PASS/FAIL per B3 criterion, latency/safety values;
* device state after Recover.

Add a dated entry to `docs/CURRENT_STATE.md`: what was verified, on which hash, what passed, what did not. Commit and open a PR.

---

## 4. Stage C — stages 4 and 5 without START_IO (only if B fully PASSED)

Goal: keeping the transport open (stage 4), then writing into BAR0 (stereo → 6 channels, WC + `sfence`) and zeroing the ring (stage 5). There will be **no sound**: without START_IO the T2 does not read the ring.

```powershell
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Stage 4 -ExpectedSysHash $h
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Mmio -ExpectedSysHash $h
.\tools\T2Audio-TestAgent.ps1 -Mode Recover
```

Criteria:

* no BSOD;
* stage 4: `audio path ENABLED for speaker 0x39 (bceIo=0, transport held open)`;
* stage 5: `AllocateAudioBuffer: system … size=…` (size a multiple of 8, i.e. stereo), `SetState 3 …`, on stop `SetState 0 … ticks=N` with N > 0;
* there must be **no** `T2 clock: re-aimed` lines: without START_IO there are no timestamps;
* after Recover the device is on AppleAudio, `Started`.

Report as in B4.

---

## 5. Stage D — silent START_IO (only with the owner's permission)

This verifies the BSOD fix and the T2 clock. No sound: Windows is muted, the engine writes zeros, and the ring is zeroed before the start.

Conditions: B and C fully PASSED (done), R1 on the minidumps does not contradict the root cause (done), and **the owner allowed this run in the chat**.

Before the run (mandatory):
1. Rebuild the package from the current `diagnostics` (order: the "▶ Next run" block at the top of this file).
2. **Mute the T2 endpoint itself, not the default device.** Windows keeps volume and Mute per endpoint, and the new T2 endpoint may become the default device at 100 %. Do a `-Stage 1` run: the probe now mutes the T2 endpoint and sets volume 0 by itself. `wasapi.txt` must contain `endpoint mute=True volume=0` and `T2 endpoint muted, volume 0.`. If it says `MUTE NOT CONFIRMED`, stop. Windows remembers this state, so for the D run the endpoint is already muted, and the probe checks it again before `Start`.
3. "Control Panel → Sound → Sounds → Sound scheme: No sounds" for the duration of the run (restore it afterwards), plus Mute and volume 0 on AppleAudio.

```powershell
# Mute + volume 0: check again
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Mmio -BceIo -OwnerApprovedBceIo -ExpectedSysHash $h
.\tools\T2Audio-TestAgent.ps1 -Mode Recover
```

Expected in the log:

```
T2Audio: SetState RUN StartIo status=0x00000000
T2Audio: START_IO sent to device 0x39
T2Audio: VHCI UPDATE_TIMESTAMP #1 dev=0x39 t2=… seed=… sample=…
T2Audio: VHCI UPDATE_TIMESTAMP #2 …                          # about 347 ms later
T2Audio: T2 clock: re-aimed lead by … frames (#1, …)          # 0–2 times at the start, rarely afterwards
T2Audio: STOP_IO sent to device 0x39                          # on stop
```

Silence criterion: `wasapi.txt` contains `endpoint mute=True volume=0`.

Record:
* the `sample` step between timestamps (expected 16 640);
* the interval between timestamps;
* how many times `re-aimed` fired per minute.

If `re-aimed` fires on every timestamp, the clock model is wrong. Stop and attach the log.

**BSOD** → stop, minidump, report. Do not repeat until it is analysed.

---

### 5.1 Stage D repeat (after the START_IO reply fix)

The first D run (2026-10-28) had no BSOD, but the T2's reply to START_IO was rejected by our check, and the stream did not enter RUN. The check is fixed. The repeat follows the same rules and **needs a new permission from the owner**: rebuild, Stage 1 with the mute confirmation, then the same D run. Expected:

```
T2Audio: START_IO reply size=256 tag=… type=… dev=0x… msg=1 status=0x00000000   # record every field
T2Audio: SetState RUN StartIo status=0x00000000
T2Audio: VHCI UPDATE_TIMESTAMP #1 … sample=…
T2Audio: T2 clock: re-aimed lead by … frames (#1, …)    # expected once at the start (~ −1900: the T2 starts reading ~40 ms after START_IO)
T2Audio: STOP_IO reply size=… msg=3 status=0x00000000
T2Audio: STOP_IO sent to device 0x39
```

Record:
* every field of the `START_IO reply` and `STOP_IO reply` lines;
* how many times `re-aimed` fired and by how many frames. More than 2–3 times in 2 seconds is a bug; attach the log;
* `ticks` > 0 on stop;
* after the stop **no more** timestamps arrive;
* uptime at the time of the test (`(Get-CimInstance Win32_OperatingSystem).LastBootUpTime`), to check the hostTime overflow.

If START_IO is rejected again (`START_IO response invalid`), a `STOP_IO` must follow immediately in the log, and the timestamps must stop.

## 6. Stage E — first sound (only once the owner allows noise)

### 6.0 Build for Stage E

Rebuild from the current `diagnostics` (all of Stage A, a new `DriverVer`, hash in the report). Check that CI is green. If `Add-Type` fails, fix the compilation in a separate commit and describe the error.

Before the audible run, do a silent repeat of Stage D on the new build, again only with the owner's permission. The log must contain the line `OutputAttenuation=…`.

### 6.1 Stereo: a quiet tone through the agent

Only after the owner's explicit permission for **sound** (permission for START_IO does not count):

```powershell
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Mmio -BceIo -OwnerApprovedBceIo `
    -AudibleVolume 0.03 -OwnerApprovedAudible -ToneFrequency 440 -ToneSeconds 3 -ToneAmplitude 0.05
```

How the run works:
* the agent first does a silent probe (`wasapi.txt`, Mute confirmed);
* then it plays the tone (`tone.txt`) and at the end sets Mute and volume 0 again;
* in the driver the samples are additionally attenuated by `OutputAttenuation`.

Expected in `tone.txt`:

```
endpoint mute=False volume=0.03
MixFormat: ch=2 rate=48000 bits=32 float=…
Start hr=0x00000000
wrote frames=… 
after run: endpoint mute=True volume=0
```

Record:
* whether the tone is heard, on which side and from which speakers (woofers/tweeters);
* any crackle or clicks every ~0.35 s (that would be a lead error);
* the kernel log: `re-aimed` and `ticks`, as in Stage D.

No tone, but the log looks like Stage D? Repeat with `-ToneFrequency 1000` and `Parameters\OutputAttenuation=1` (−6 dB). Do not raise the endpoint volume. If it is still silent, stop and write the report: the T2 does not read the ring, and reverse engineering is needed (§7).

If the tweeters are unpleasant: `Parameters\TweeterMode=0` (woofers only) and restart the device.

Ordinary music is allowed only after the tone PASSED, at 1–5 % volume and with the same `OutputAttenuation`.

### 6.2 Layout (one channel at a time)

Only with `-RawChannels` (**tweeters unprotected**): a quiet tone of **1 kHz and above**, one channel 0…5 at a time. The agent accepts `-AudibleVolume` together with `-RawChannels` only with `-ToneChannel 0..5`, and for channels 2/5 only with `-ToneFrequency >= 1000`. Easier: `-SpeakerCheck -RawChannels`. The tone script itself refuses too (`TWEETER REFUSED`) if anything below 1 kHz goes to channels 2/5 or to all channels.

```powershell
foreach ($c in 0..5) {
  .\tools\T2Audio-TestAgent.ps1 -Mode Test -Mmio -BceIo -OwnerApprovedBceIo -RawChannels `
      -AudibleVolume 0.03 -OwnerApprovedAudible -ToneChannel $c -ToneFrequency 1000 -ExpectedSysHash $h
  .\tools\T2Audio-TestAgent.ps1 -Mode Recover
  Read-Host "Channel ${c}: which speaker played? (write it down)"
}
```

Record which speaker plays. **Never** send low frequencies to channels 2 and 5. If the order does not match the table in `DRIVER_GUIDE.md` §4.3, fix `T2AUDIO_SPK_*` in `src/T2AudioSpeakerLogic.h` and the tests.

---

## 7. TODO for the builder (without hardware, or after Stage E)

Each task gets its own branch off `diagnostics` and a PR. Mark finished items here: `[x]`, the date and a link to the PR. The overall plan to full functionality is in [`docs/ROADMAP.md`](docs/ROADMAP.md); the items below are the builder's share of it.

**Now (no hardware):**
- [x] **S1. Build the new `diagnostics` on Windows** for Stage E: driver, signing, hash (§2). Built 2026-10-28, DriverVer 1.0.8.0, SHA256 26A701DCA40DEF0EFBF6DC74D0D30849A83AA12D0B1E31F82436F25959ED7D7B. Sound obtained on hardware.
- [ ] **S2. Packaging in CI (D4).** Copy the `.sys` into `packaging/`, stamp `DriverVer`, run `InfVerif`, build an unsigned catalog (like `BuildT2TouchId.yml` by osaul4ik). No secrets in CI.
- [ ] **S3. INF defaults.** Write the parameter defaults into `Parameters` through `AddReg` so they are visible and editable in the registry. Today they are only defaults in code (the installer writes the preset values).
- [ ] **S5. Driver Verifier** (standard flags + IRQL checking) on stages 1–3 silently, then on the persistent-mode setup with sound, only with the owner's permission. Afterwards turn Verifier off (`verifier /reset`, reboot) and record the result.
- [ ] **S6. Go through the report-only CI output** (PSScriptAnalyzer, InfVerif, WDK code analysis) in the Actions log. The first code-analysis triage is the table in `docs/REVIEW_20261028.md` §1.7. Fix the findings in separate PRs, then remove `continue-on-error`.
- [ ] **S4. Remove the obsolete hardware-MDL path** (`T2AudioCreateSpeakerMdl`, `MdlVariant`, `tools/Test-MdlVariants.ps1`): the WaveRT buffer has lived in system memory for a long time. Only if CI stays green and behaviour does not change.

**Tools:**
- [x] **T1. Channel selection in `Test-WasapiTone.ps1`** (`-Channel`): done 2026-10-28 (Claude), compiled in CI.
- [x] **T2. `-RawChannels` in the agent together with the tone** (`-ToneChannel`, ≥ 1 kHz): done 2026-10-28 (Claude).

**Reverse engineering (REA + Ghidra, `docs/REVERSE_PLAN.md`):**
- [ ] **R4.** Whether `SET_PROPERTY 'nsrt'` is needed before START_IO.
- [ ] **R5.** What `'pfta'`, `'ring'`, `'sact'`, `'term'`, `'lcdr'`, `'lcdv'` are.
- [ ] **R6.** Channel numbers in the 6-channel branch of AppleAudio's copy loop (`0x14000A715`–`0x14000B2C0`): static confirmation of the layout.
- [ ] **R7.** How AppleAudio controls volume: msg 28 (`controls`), range and steps. Needed for hardware volume.

**Clock (after Stage E PASSED, with the owner's permission for sound):**
- [ ] **E2. Long run, 10 minutes, `ClockRateTracking=0`** (current behaviour). Quiet music or a tone. Count the `re-aimed` lines in the log and note clicks and dropouts by ear. Measure the real drift: the difference between the `sample` step (16 640) and the interval between timestamps in ms. (Session #3 measured ~10 ppm: one jump every ~18 min.)
- [ ] **E3. The same run with `Parameters\ClockRateTracking=1`** (dword) and a device restart. Expected:
  * log lines `T2 clock: rate=… ppb drift=… ppb lead=…`, where drift is the real T2 drift and stable;
  * `lead` stays within 1000 ± 100;
  * `re-aimed` no more than 1–2 times at the start.
  If `rate` hits ±1 000 000 or `lead` runs away, set it back to 0 and attach the log.

**After Stage E (stages F/G of the README):**
- [x] **F1.** Equalizer from the kaiT2en profile `dsp/profiles/16_1`: done in the driver instead of Equalizer APO (`src/T2AudioDsp.h`, `SpeakerDsp=1..4`; level 3 approved by the owner).
- [x] **F2.** Limiter/DRC in the driver: done (lookahead limiter, compressors; the lookahead version is not yet tested on hardware).
- [ ] **G1.** Hardware volume (after R7).
- [~] **G2.** Headphones: `'jack'` events implemented (`JackEvents`, `HeadphoneAutoMute`, 40c1d6d; next run step 4); the headphone output is next (`docs/HEADPHONES.md` step 3), after the `ExtendedProbe` log.
- [ ] **G3.** Microphone (`0x21 Digital Mic` / `0x44 Codec Input`).
- [~] **G4.** Autostart without `TestSession`: implemented as the installer's persistent mode with the crash guard; awaiting the hardware test (next run, steps 2–3 and 6).

---

## 8. How to do the work

* Small commits with a clear message: what, why, how it was verified.
* Any new arithmetic or parsing goes into `src/BceProtocolLogic.h` / `src/T2AudioBufferLogic.h` / `src/T2AudioSpeakerLogic.h` / `src/T2AudioDsp.h`, plus a test in `tests/`. CI must be green.
* A status change is a dated entry in `docs/CURRENT_STATE.md`. A behaviour change updates `docs/DRIVER_GUIDE.md`.
* All documentation is in English. Quote the owner verbatim (Russian) and add an English translation.
* In reports, distinguish "verified on hardware (hash …)", "verified by tests" and "hypothesis".
