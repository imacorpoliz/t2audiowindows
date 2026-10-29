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
| Lookahead limiter, autogain (`DspAutoGain`), buzz compressor (`SpeakerDsp=4`), trims, tone controls, presets | ✅ (1.0.12, presets and CPU load verified; owner keeps `macos`) |
| Persistent mode (start on every boot) + crash guard, installer | ✅ (1.0.12; installer in English since 2026-10-29) |
| Sleep / wake, sound back after sleep (`ResumeAfterSleep`) | ✅ (1.0.13, tested by the owner 2026-10-29) |
| Headphone output | ✅ (1.0.13) |
| Jack detection → automatic headphone/speaker switching | ⬜ (no working jack signal yet; GEMINI.md part 3) |
| Built-in microphones (3-mic array) | ✅ (1.0.13) |
| Headset microphone | ⬜ |
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
| 2.1 | ✅ **Verified by the owner on hardware (2026-10-29): sound comes back after sleep (`ResumeAfterSleep=1`).** Resume after sleep while a stream is RUN. | C, then G | |
| 2.2 | **Hibernate and Fast Startup** (a kernel hibernation on shutdown): check that the crash guard sees a clean end and that the device comes back. | G | Same test as 2.1 with `shutdown /h` and with Fast Startup on. |
| 2.3 | **Long-run clock** (`GEMINI.md` §7 E2/E3): 10+ minutes with `ClockRateTracking=0` and `=1`. Decide whether to make rate tracking the default (it removes the rare re-aim every ~18 min). | G, then C | |
| 2.4 | **Driver Verifier** (standard + IRQL checks) on the persistent setup with sound. | G | `GEMINI.md` §7 S5. |
| 2.5 | **DPC cost on the real machine**: `maxCopyUs` from `run summary` at levels 3 and 4. If well above 100 µs, optimise the DSP (fewer stages at level 4, process in blocks, SSE2 for the biquads). | G, then C | Host measurement: ~60–73 µs per 2 ms tick. |
| 2.6 | **Code hygiene:** remove the obsolete MDL-on-BAR path (S4), switch to `ExAllocatePool2`, fix the WDK code-analysis findings and make the CI analysis blocking (S6), INF defaults in `AddReg` (S3), packaging in CI (S2). | C, G | No behaviour change; CI must stay green. |
| 2.7 | 🟡 **Done in the installer (2026-10-29): `Status` warns; opt-in `-Action UpdateGuard -Block 1`. Hardware check: GEMINI.md part 3, B.** **Windows Update may reinstall AppleAudio** (Microsoft-signed, ranks higher). Options: the installer detects it and offers to re-run; a device-install restriction policy for the hardware id; or simply document it (current state). | C, O | Owner decides how intrusive this may be. |
| 2.8 | ✅ **16/24-bit verified on hardware (2026-10-29); 44.1/96 kHz not done.** **Formats:** only 48 kHz / 32-bit / stereo is offered. Shared mode is fine (the engine converts), but exclusive-mode apps asking for 44.1 kHz or 16/24-bit fail. Add 16- and 24-bit PCM (cheap) and, optionally, 44.1/96 kHz through `SET_PROPERTY 'nsrt'` (AppleAudio does this) with DSP tables per rate. | C, then G | 44.1 kHz needs a second set of DSP coefficients (the generator already takes the rate as input; the XTC FIR exists for 44.1/48/96). |
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
| 4.2 | 🟡 **Hardware 2026-10-28: the listener registers (reply OK) but bridgeOS sent no `PROPERTY_CHANGED` on 3 plug/unplug cycles. Added: with `JackEvents=1` the driver also polls `'jack'` every 1 s (source only); awaiting the retest (GEMINI.md step 4f).** Implemented behind `JackEvents` / `HeadphoneAutoMute` (40c1d6d). **Jack events:** send `PROPERTY_LISTENER` (msg 11) for `'outp'/'jack'`; handle `PROPERTY_CHANGED` (msg 13) in the `0x222020` handler; re-read the property (AppleAudio only toggles a flag, then reads `GET_PROPERTY 'jack'`, `RE_appleaudio_protocol.md` §4). While plugged in, write silence to the speaker ring (speakers mute automatically). | C, then G | Parser + test first (`BceProtocolLogic.h`). |
| 4.3 | ✅ **Done on hardware 2026-10-28** (`EXCERPT_20261028_step4.md`): `"Codec Output"` 0x25 has one output stream 0x26, 48 kHz, **2 ch, 24-bit in 32-bit slots (bpf 8)**, flags 0x4 (signed int, same as the speaker); BufferStruct ring at BAR1 0x190000, size 0x20800 = **16 640 frames**, the same length as the speaker ring. So the headphone endpoint can reuse the speaker copy path with 2 channels instead of 6. **Probe the headphone stream:** log `GET_OUTPUT_STREAM_LIST`, `'pft '`, `'ltnc'`, `'saft'` for `"Codec Output"` and its BufferStruct ring (name, size). Read-only. | C, then G | Tells us the format (probably stereo S24_LE) before any write. |
| 4.4 | ✅ **Proven on hardware 1.0.13.4 (2026-10-29): the headphone endpoint plays.** Implemented behind `HeadphoneOutput`. **Second endpoint "Headphones":** a second WaveRT filter + topology with a headphone pin; generalise `T2AudioFindSpeakerBuffer`, the stream query, START_IO/STOP_IO and the clock anchor (today speaker-only) to per-device state; no speaker DSP (optionally a flat limiter). | C | The biggest code item. Keep the speaker path unchanged; behind a parameter until tested. |
| 4.5 | 🟡 **Reporting implemented, but no working jack signal yet (read stays 0, no notifications) → no automatic switching; GEMINI.md part 3 (J1/J2).** Implemented (2026-10-28): `KSPROPERTY_JACK_DESCRIPTION(2)` + `KSEVENT_PINCAPS_JACKINFOCHANGE` on the headphone jack pin, driven by the jack poll (`JackEvents=1`); without it the jack reads connected.** **Jack reporting to Windows:** `KSPROPERTY_JACK_DESCRIPTION` with the connection state and a jack-change notification, so Windows shows the endpoint as plugged/unplugged and switches the default device. | C | |
| 4.6 | Hardware test: plug/unplug while playing, both endpoints, sleep with headphones in. | G, O | Quiet levels first: headphones are far more sensitive than speakers. |

---

## 5. Microphones

Two input devices: `"Digital Mic"` (`0x21`, the 3-microphone array) and `"Codec Input"` (`0x44` on hardware 2026-10-28, the headset microphone on the jack). Unlike the speakers, **input buffers are allocated by the host** and registered with the T2: kaiT2en (`t2audio_init_bs_stream_host` + `SET_INPUT_STREAM_ADDRESS_RANGES`) and AppleAudio (`0x1FA4` allocate → `0x46F8` write the physical ranges into BufferStruct → `0x44E4` msg 18; re-registered on every D0, `AppleAudio_BufferRegistration_Report.md`).

| # | Item | Who | Notes |
|---|---|---|---|
| 5.1 | ✅ **Done on hardware 2026-10-28:** `"Digital Mic"` 0x21 input stream 0x22: 48 kHz, **3 ch, 24-bit in 32-bit slots (bpf 12)**, flags 0x4; BufferStruct already lists 3 host buffers for it (e.g. 0x47E2DA000 size 0x3000, 0x47E2BA000 size 0x20000), i.e. physical RAM addresses registered earlier (bridgeOS keeps them across a warm reboot; whoever registered them, AppleAudio or macOS, they are not ours and must be replaced, never written to). `"Codec Input"` 0x44 stream 0x45: **1 ch, 32-bit int, flags 0xC** (signed, packed). | **Probe the input devices:** `GET_INPUT_STREAM_LIST` (msg 24), `'pft '`, `'ltnc'`, `'saft'`, `'inpt'/'jack'` for both devices; log only. | C, then G | Read-only. |
| 5.2 | ✅ **Proven on hardware 1.0.13.4: msg 18 in AppleAudio's 837-byte form + `nsrt`, "Digital Mic" appended to BufferStruct.** **Buffer registration:** allocate a non-paged physically described buffer, write its page ranges into `BufferStruct.devices[i].input_streams[0]` (`num_input_streams`, `num_buffers`, `buffers[]`), send msg 18; repeat after D0. Pure layout code + host tests first. | C | Writes into device memory: new stage gate, own test plan. |
| 5.3 | ✅ **Proven: speech recorded (peak 0.177, RMS −37 dBFS).** **Capture endpoint(s):** a WaveRT capture filter. Because the input buffer is host RAM, the WaveRT buffer can be the very buffer the T2 writes into (zero-copy), or a copy as on output. Clock from `UPDATE_TIMESTAMP` of the input device. | C | |
| 5.4 | 🟡 **Implemented (`MicDsp=1`, host-tested against a double reference).** **Microphone DSP:** kaiT2en `dsp/profiles/16_1/mic.json` takes the 3 channels, sums them (a fixed beam), then a high-pass and an EQ biquad → mono. Small; port like the speaker tables (GPL-3 applies). Expose mono "Microphone Array" to Windows; optionally the raw 3 channels for apps that do their own processing. | C | |
| 5.5 | **Headset microphone** on `"Codec Input"`, switched by the `'inpt'/'jack'` state. | C | After 4.2 and 5.3. |
| 5.6 | Hardware tests: recording levels, noise, sleep/wake re-registration. | G, O | |

---

## 6. Distribution and signing

| # | Item | Who | Notes |
|---|---|---|---|
| 6.1 | ✅ **Decided by the owner (2026-10-29): no Microsoft signature; the driver stays test-signed (test mode, Secure Boot off).** **Signing.** Today the driver needs `testsigning` on and Secure Boot off. For an install without test mode the package needs a Microsoft signature: an EV code-signing certificate (an organisation, a yearly cost) + attestation signing through the Windows Hardware Dev Center. Alternative: stay with test signing for personal use. | O | Decision; nothing to build until then. |
| 6.2 | **Licence.** The DSP tables are generated from kaiT2en's GPL-3.0-or-later graph, so a distributed driver binary carries GPL-3.0 obligations (`THIRD_PARTY_LICENSES.md`). Decide the repository licence and, if needed, an offer of source with every binary. | O | |
| 6.3 | ✅ **English UI done (2026-10-29).** **Installer polish:** English UI, an offline package layout (driver + installer in one folder), clear messages when Windows Update brings back AppleAudio (2.7). | C | |
| 6.4 | **Release process:** versioned packages built in CI (S2), the signed `.sys`/`.cat` stored outside git (in `imacorpoliz/mbp` or releases), a changelog from `CURRENT_STATE.md`. | C, G | |

---

## 7. Sound quality follow-ups (optional, by ear)

| # | Item | Notes |
|---|---|---|
| 7.1 | Owner verdicts on `maxvolume`, `even`, `bright` (§1.5) decide the defaults. | If the hum remains at full volume with `SpeakerDsp=4` + `WooferLimitDb`, measure which band it is (a sweep with `-SpeakerCheck`). |
| 7.2 | Closer lsp emulation. **Done in source (2026-10-28, not hardware-tested):** RMS detection and a soft knee in the compressors (`CompStyle=1`, preset `macos-precise`), K-weighting (LUFS) and the −72 LUFS gate in the autogain. Left: the ControlFreak limiter's short release (ours 21 ms vs lsp 0.5 ms; ours is deliberately gentler), lsp autogain's fast surge path. | GEMINI.md step 5a: if the owner prefers `macos-precise`, make `CompStyle=1` the default. |
| 7.4 | User tone controls `BassDb`/`TrebleDb`/`ClarityDb` and presets `bass`, `voice`, `movie`, `night`; `-Action Tune`. **Done in source, not hardware-tested.** | Verdicts in GEMINI.md step 5a; adjust the preset values by ear. |
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
