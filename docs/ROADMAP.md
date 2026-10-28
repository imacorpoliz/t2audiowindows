# Roadmap to a fully functional T2AudioPort

**Date:** 2026-10-28. **Scope:** what is left between today's driver and a driver that fully replaces AppleAudio on the MacBookPro16,1 (and, optionally, on other T2 Macs).
Status of each item: ✅ proven on hardware · 🟡 implemented, not yet tested on hardware · ⬜ not implemented.
Who: **G** = Gemini on the Windows/Mac machine (build, hardware tests, reverse engineering with REA/Ghidra) · **C** = Claude (code, host tests, docs) · **O** = the owner (listening, permissions, decisions).

---

## 0. Where we are

| Area | State |
|---|---|
| Driver loads, BAR/BufferStruct, BCE transport, START_IO/STOP_IO, T2 clock | ✅ (Stage D, 1.0.7.0) |
| Sound on all 6 speakers through a stereo endpoint | ✅ (Stage E, 1.0.8.0) |
| Speaker DSP (KAIT2EN EQ, XTC, virtual bass, multiband compressor) | ✅ level 3 approved by the owner (1.0.11.0) |
| Copy timing (no underruns, max gap ~3 ms), start-of-stream click fix | ✅ |
| Lookahead limiter, autogain (`DspAutoGain`), buzz compressor (`SpeakerDsp=4`), trims | 🟡 |
| Persistent mode (start on every boot) + crash guard, installer with presets | 🟡 |
| Headphone jack state (read at probe only) | 🟡 |
| Sleep / hibernate / wake | 🟡 (STOP_IO on power-down exists; resume path unverified) |
| Headphone output, jack auto-switch | ⬜ |
| Built-in microphones (3-mic array), headset microphone | ⬜ |
| Hardware volume / mute | ⬜ (software volume works) |
| Production signing (no test mode) | ⬜ (needs an owner decision) |
| Other T2 Mac models | ⬜ |

"Fully functional" here means: **speakers + headphones + microphones work, survive sleep and reboots, follow the jack, and the package installs without test mode.** Other models are a separate, optional goal (§8).

---

## 1. Verify what is already built — next hardware session (G, O)

Nothing below can be trusted for daily use until this passes. The steps are in `GEMINI.md`, "▶ NEXT RUN".

1. Build 1.0.12.0 (CI is green on `421e08c`).
2. Install with `tools/T2Audio-Setup.ps1 -Action Install -Preset macos`; listen.
3. **Reboot test of the crash guard:** an ordinary restart must leave `CrashGuardStrikes=0` and the driver must start by itself.
4. Jack line in the log, plugged and unplugged (confirms the headphone device id and the `'jack'` property for §4).
5. Presets: `maxvolume`/`loud` at 100 % with heavy bass (the hum the owner reported; the lookahead limiter should help), `even` (autogain: more even level, no pumping), `bright` (the "sample rate seems a bit low" remark).
6. Sleep and wake (with permission): sound returns, no BSOD, crash guard OK.
7. Uninstall returns AppleAudio.

**Exit criterion:** all PASS → the driver is fit for daily use on speakers. Any FAIL → a log for Claude, fix, repeat.

---

## 2. Robustness for daily use

| # | Item | Who | Notes |
|---|---|---|---|
| 2.1 | **Resume after sleep while a stream is RUN.** If the audio engine keeps the stream in RUN through sleep, the driver stops I/O on power-down but only restarts it on the next `SetState(RUN)` (`RE_appleaudio_protocol.md` §7, open item): silence until the app restarts playback. Fix: on D0, if a stream is in RUN, re-zero the ring, re-anchor and send START_IO from a PASSIVE work item. | C, then G | Only after §1.6 shows the actual behaviour. |
| 2.2 | **Hibernate and Fast Startup** (a kernel hibernation on shutdown): check that the crash guard sees a clean end and that the device comes back. | G | Same test as 2.1 with `shutdown /h` and with Fast Startup on. |
| 2.3 | **Long-run clock** (`GEMINI.md` §7 E2/E3): 10+ minutes with `ClockRateTracking=0` and `=1`. Decide whether to make rate tracking the default (it removes the rare re-aim every ~18 min). | G, then C | |
| 2.4 | **Driver Verifier** (standard + IRQL checks) on the persistent setup with sound. | G | `GEMINI.md` §7 S5. |
| 2.5 | **DPC cost on the real machine**: `maxCopyUs` from `run summary` at levels 3 and 4. If well above 100 µs, optimise the DSP (fewer stages at level 4, process in blocks, SSE2 for the biquads). | G, then C | Host measurement: ~60–73 µs per 2 ms tick. |
| 2.6 | **Code hygiene:** remove the obsolete MDL-on-BAR path (S4), switch to `ExAllocatePool2`, fix the WDK code-analysis findings and make the CI analysis blocking (S6), INF defaults in `AddReg` (S3), packaging in CI (S2). | C, G | No behaviour change; CI must stay green. |
| 2.7 | **Windows Update may reinstall AppleAudio** (Microsoft-signed, ranks higher). Options: the installer detects it and offers to re-run; a device-install restriction policy for the hardware id; or simply document it (current state). | C, O | Owner decides how intrusive this may be. |
| 2.8 | **Formats:** only 48 kHz / 32-bit / stereo is offered. Shared mode is fine (the engine converts), but exclusive-mode apps asking for 44.1 kHz or 16/24-bit fail. Add 16- and 24-bit PCM (cheap) and, optionally, 44.1/96 kHz through `SET_PROPERTY 'nsrt'` (AppleAudio does this) with DSP tables per rate. | C, then G | 44.1 kHz needs a second set of DSP coefficients (the generator already takes the rate as input; the XTC FIR exists for 44.1/48/96). |
| 2.9 | **Latency reporting:** add the limiter's 24 frames (0.5 ms) to `GetHWLatency`. | C | ✅ done in code (40c1d6d). |

---

## 3. Volume and mute

Today Windows applies the volume in software before the driver. That works, and the DSP's compressors and limiter see the attenuated signal, which is the right order. What is missing:

| # | Item | Who | Notes |
|---|---|---|---|
| 3.1 | **Reverse engineer msg 28 (`controls`)**: object classes `'levl'` (level), `'slct'` (selector), `'togl'` (toggle) and their properties: range, steps, dB mapping (`REVERSE_PLAN.md` R7). | G | Static, with REA/Ghidra on AppleAudio. |
| 3.2 | **Hardware volume/mute node** in the topology filter (`KSNODETYPE_VOLUME`, `KSNODETYPE_MUTE`) driving the T2 level control. Benefit: full 24-bit resolution at low volume, and an amplifier mute that does not depend on writing zeros. | C, then G | Only if 3.1 shows a usable control. Optional: keep software volume and add only a hardware mute. |
| 3.3 | **Low-volume loudness** (optional): the KAIT2EN chain is tuned for normal levels; at very low volume the bass fades first. A gentle loudness contour driven by the endpoint volume would need 3.2 (or a volume notification). | C, O | Owner preference. |

---

## 4. Headphones (3.5 mm)

Plan in detail: [`HEADPHONES.md`](HEADPHONES.md). The kaiT2en driver shows the way: the headphones are a separate BCE device `"Codec Output"` (`0x25` on the test machine) with its own output stream and its own ring in BufferStruct, a jack property `'outp'/'jack'`, and a property listener.

| # | Item | Who | Notes |
|---|---|---|---|
| 4.1 | Jack read at probe | 🟡 | Verified in §1.4. |
| 4.2 | 🟡 **Implemented behind `JackEvents` / `HeadphoneAutoMute` (40c1d6d), awaiting the hardware test.** **Jack events:** send `PROPERTY_LISTENER` (msg 11) for `'outp'/'jack'`; handle `PROPERTY_CHANGED` (msg 13) in the `0x222020` handler; re-read the property (AppleAudio only toggles a flag, then reads `GET_PROPERTY 'jack'`, `RE_appleaudio_protocol.md` §4). While plugged in, write silence to the speaker ring (speakers mute automatically). | C, then G | Parser + test first (`BceProtocolLogic.h`). |
| 4.3 | 🟡 **Implemented as `ExtendedProbe` (40c1d6d), awaiting the hardware log.** **Probe the headphone stream:** log `GET_OUTPUT_STREAM_LIST`, `'pft '`, `'ltnc'`, `'saft'` for `"Codec Output"` and its BufferStruct ring (name, size). Read-only. | C, then G | Tells us the format (probably stereo S24_LE) before any write. |
| 4.4 | **Second endpoint "Headphones":** a second WaveRT filter + topology with a headphone pin; generalise `T2AudioFindSpeakerBuffer`, the stream query, START_IO/STOP_IO and the clock anchor (today speaker-only) to per-device state; no speaker DSP (optionally a flat limiter). | C | The biggest code item. Keep the speaker path unchanged; behind a parameter until tested. |
| 4.5 | **Jack reporting to Windows:** `KSPROPERTY_JACK_DESCRIPTION` with the connection state and a jack-change notification, so Windows shows the endpoint as plugged/unplugged and switches the default device. | C | |
| 4.6 | Hardware test: plug/unplug while playing, both endpoints, sleep with headphones in. | G, O | Quiet levels first: headphones are far more sensitive than speakers. |

---

## 5. Microphones

Two input devices: `"Digital Mic"` (`0x21`, the 3-microphone array) and `"Codec Input"` (`0x43`, the headset microphone on the jack). Unlike the speakers, **input buffers are allocated by the host** and registered with the T2: kaiT2en (`t2audio_init_bs_stream_host` + `SET_INPUT_STREAM_ADDRESS_RANGES`) and AppleAudio (`0x1FA4` allocate → `0x46F8` write the physical ranges into BufferStruct → `0x44E4` msg 18; re-registered on every D0, `AppleAudio_BufferRegistration_Report.md`).

| # | Item | Who | Notes |
|---|---|---|---|
| 5.1 | 🟡 **Covered by `ExtendedProbe` (40c1d6d).** **Probe the input devices:** `GET_INPUT_STREAM_LIST` (msg 24), `'pft '`, `'ltnc'`, `'saft'`, `'inpt'/'jack'` for both devices; log only. | C, then G | Read-only. |
| 5.2 | **Buffer registration:** allocate a non-paged physically described buffer, write its page ranges into `BufferStruct.devices[i].input_streams[0]` (`num_input_streams`, `num_buffers`, `buffers[]`), send msg 18; repeat after D0. Pure layout code + host tests first. | C | Writes into device memory: new stage gate, own test plan. |
| 5.3 | **Capture endpoint(s):** a WaveRT capture filter. Because the input buffer is host RAM, the WaveRT buffer can be the very buffer the T2 writes into (zero-copy), or a copy as on output. Clock from `UPDATE_TIMESTAMP` of the input device. | C | |
| 5.4 | **Microphone DSP:** kaiT2en `dsp/profiles/16_1/mic.json` takes the 3 channels, sums them (a fixed beam), then a high-pass and an EQ biquad → mono. Small; port like the speaker tables (GPL-3 applies). Expose mono "Microphone Array" to Windows; optionally the raw 3 channels for apps that do their own processing. | C | |
| 5.5 | **Headset microphone** on `"Codec Input"`, switched by the `'inpt'/'jack'` state. | C | After 4.2 and 5.3. |
| 5.6 | Hardware tests: recording levels, noise, sleep/wake re-registration. | G, O | |

---

## 6. Distribution and signing

| # | Item | Who | Notes |
|---|---|---|---|
| 6.1 | **Signing.** Today the driver needs `testsigning` on and Secure Boot off. For an install without test mode the package needs a Microsoft signature: an EV code-signing certificate (an organisation, a yearly cost) + attestation signing through the Windows Hardware Dev Center. Alternative: stay with test signing for personal use. | O | Decision; nothing to build until then. |
| 6.2 | **Licence.** The DSP tables are generated from kaiT2en's GPL-3.0-or-later graph, so a distributed driver binary carries GPL-3.0 obligations (`THIRD_PARTY_LICENSES.md`). Decide the repository licence and, if needed, an offer of source with every binary. | O | |
| 6.3 | **Installer polish:** English UI option (today Russian only), an offline package layout (driver + installer in one folder), clear messages when Windows Update brings back AppleAudio (2.7). | C | |
| 6.4 | **Release process:** versioned packages built in CI (S2), the signed `.sys`/`.cat` stored outside git (in `imacorpoliz/mbp` or releases), a changelog from `CURRENT_STATE.md`. | C, G | |

---

## 7. Sound quality follow-ups (optional, by ear)

| # | Item | Notes |
|---|---|---|
| 7.1 | Owner verdicts on `maxvolume`, `even`, `bright` (§1.5) decide the defaults. | If the hum remains at full volume with `SpeakerDsp=4` + `WooferLimitDb`, measure which band it is (a sweep with `-SpeakerCheck`). |
| 7.2 | Closer lsp emulation: soft knee and RMS detection in the compressors, the true ControlFreak limiter curve, K-weighting (LUFS) in the autogain. | Only if the owner hears a difference worth it. |
| 7.3 | "Not quite like macOS yet": compare against macOS on the same tracks (the owner can boot macOS); adjust `DspGainDb`/trims per preset. | Ears, not code. |

---

## 8. Other T2 Macs (optional, separate goal)

The PCI function is the same on every T2 Mac, but the speaker count, the channel layout and the DSP differ. kaiT2en has profiles for `15_1`–`15_4`, `16_1`–`16_4`, `8_1`, `8_2`, `9_1`, `imac20_1`, `imacpro1_1`.

1. Detect the model (SMBIOS product name, or the PCI subsystem id) and refuse unknown models by default.
2. Generate the DSP tables for each profile with `tools/dsp/gen_kait2en_dsp.py`, select at runtime, and read the ring channel count from `'pft '` (not every model has 6 channels).
3. Each model needs a tester with that Mac. Without one, a model stays unsupported.

---

## Suggested order

1. §1 (verify) → fix whatever fails.
2. §2.1–2.5 (sleep/resume, long runs, Verifier, DPC cost).
3. §4.2 (jack → mute speakers): small and very visible for daily use.
4. §4.3–4.6 (headphone endpoint).
5. §5 (microphones).
6. §3 (hardware volume), §2.6–2.9 (hygiene, formats) in parallel when convenient.
7. §6 when the owner decides on signing and licence; §8 only if wanted.

Each step follows the project rules: pure logic in headers with host tests, a new hardware access behind a gate, hardware runs only through `GEMINI.md` with the owner's permission, and every result recorded in `CURRENT_STATE.md`.
