# T2AudioPort Driver - Current State (Authoritative)

**Last Updated**: 2026-10-28 (after Stage E; Claude analysis and fixes, see the first entry)  
**Current status**: **STAGE E FIRST SOUND ACHIEVED (PROVEN ON HARDWARE).** Physical sound successfully produced on MacBookPro16,1 on bare-metal Windows via custom T2Audio driver! Streams verified both via WASAPI automated tone and live Windows system audio. Complete stability: zero BSODs, clean START_IO and STOP_IO. Owner acoustic observations recorded: sound is present, flat, occasionally crackles, and not very loud. Deep analysis and action plan documented in [docs/CLAUDE_REPORT_STAGE_E.md](CLAUDE_REPORT_STAGE_E.md).  
**Master Handover Guide**: See [docs/AGENT_HANDOVER_GUIDE.md](AGENT_HANDOVER_GUIDE.md).  
**Physical Machine Status**: MacBookPro16,1 running official `AppleAudio.sys` (Status OK).

### 2026-10-28 (after Stage E): log analysis; start-of-stream gap fixed; listening mode; copy-timing counters (source only, not hardware-tested)

Analysis of `docs/logs/EXCERPT_20261028_143928.md` and the owner's report ("звука в тесте не было"; live playback: flat, sometimes crackles, not loud):

- **The automated tone was inaudible by design error (Claude):** endpoint volume 0.03 × tone amplitude 0.05 × `OutputAttenuation=3` (−18 dB) ≈ −80 dB. The kernel log of that run is nevertheless normal (START_IO, RUN, timestamps, copy, STOP_IO). The sound the owner heard came from the later live-playback session with `OutputAttenuation=1`.
- **Clock drift is NOT the likely crackle cause (correction to the entry below):** the T2 timestamps of the run give a drift of about **+10 ppm** (lap intervals 346.656–346.671 ms vs 346.667 ms nominal). At 10 ppm the lead leaves the ±500-frame window only after ~18 minutes, so drift re-aims are rare. `ClockRateTracking` still removes them, but it does not explain "sometimes".
- **Found: an audible gap at the start of every stream.** Each RUN logs two re-aims: `-1748` then `+615` (Stage D: `-1906`, `+768`). The second one zero-fills ~13 ms in the middle of the audio. Cause: uptime was ~49.8 h, so VHCI's `hostTime` had wrapped (`qpc*1e7` overflow, value `2^64 − 5.1·10^12`); the driver then anchors on the reception time, and the first timestamp after START_IO arrives ~13 ms late (#1→#2 received 333 ms apart, hostTime 346.67 ms apart). Windows starts and stops the render stream often, hence "sometimes".
- **Fix:** `T2AudioClockUnwrapHostTime` (`src/T2AudioSpeakerLogic.h`) moves a wrapped hostTime back by whole wrap periods (`2^64·100/freq` ns, 51.2 h at 10 MHz) next to the reception time; used in `BceTransport.c` before the anchor is computed. A plausible hostTime is returned unchanged, so machines with < 25.6 h uptime are unaffected. `tests/T2AudioPipelineTest.c` reproduces the hardware log (old driver: 2 re-aims and a gap; with unwrap: 1 re-aim, no gap) and checks the logged value unwraps to ~49.8 h.
- **Second suspect, now measured:** DPC latency. If no copy runs for longer than the lead (~20.8 ms), the T2 plays stale ring data. New per-run log line when leaving RUN: `run summary: reAims=… rateUpdates=… ratePpb=… maxCopyGapUs=… gaps>10ms=… gaps>lead=… skips=…`. `gaps>lead` > 0 or `skips` > 0 point to DPC latency (fix would be a larger lead).
- **Listening mode:** `T2Audio-TestAgent.ps1 … -Listen -OwnerApprovedAudible` starts the driver, skips probe and tone, and keeps it running until Enter while the kernel log is captured.
- **Levels:** `OutputAttenuation` default 3 → **1** (−6 dB, the value the owner listened at); tone limits raised to endpoint volume ≤ 0.25, amplitude ≤ 0.3 (default 0.2).
- "Not loud": besides the −6 dB cut, Windows' endpoint volume and the missing DSP/loudness processing (F1/F2). Whether AppleAudio also sets amplifier gain over BCE is open (R7).

### 2026-10-28: Stage E Hardware Verification — First Sound Achieved! (PROVEN ON HARDWARE)

- **Test Report:** [docs/logs/EXCERPT_20261028_143928.md](logs/EXCERPT_20261028_143928.md) & [docs/CLAUDE_REPORT_STAGE_E.md](CLAUDE_REPORT_STAGE_E.md).
- **Tested Build:** DriverVer `10/28/2026,1.0.8.0`, SHA256 `26A701DCA40DEF0EFBF6DC74D0D30849A83AA12D0B1E31F82436F25959ED7D7B`, signed with test cert CA2DE95D1B065F6D6CFF6C4158DC88E2424ABBCE.
- **Pre-check (PASS):** Stage 1 verified mute and endpoint parameters (wasapi.txt: ndpoint mute=True volume=0, T2 endpoint muted, volume 0.).
- **Stage D Regression (PASS):** Clean `START_IO reply msg=1 status=0x0`, `SetState RUN StartIo status=0x0`, sample delta 16640, `STOP_IO reply msg=3 status=0x0`.
- **Stage E Automated Tone (PASS):** 440 Hz tone played for 3 seconds at 0.03 volume; 144,000 frames rendered in 201 iterations; endpoint automatically re-muted.
- **Stage E Live Playback (PASS):** Driver enabled with `OutputAttenuation=1` (-6 dB) and `DiagStage=5` (MMIO+BCE); default Windows audio endpoint played audio directly from user applications.
- **Physical Acoustic Observations (Owner):**
  - **Звук есть (Sound present):** Core audio engine, DMA buffer mirroring, and co-processor stream confirmed operational.
  - **Плоский (Flat):** Expected due to absence of Apple's CoreAudio DSP/EQ and speaker compensation curves (Task F1).
  - **Иногда хрепит (Occasional crackles):** *(Corrected in the entry above: measured drift is ~10 ppm, so this is more likely the start-of-stream re-aim gap or DPC latency.)* Exactly matches the predicted clock-drift behavior from `tests/T2AudioPipelineTest.c` when `ClockRateTracking=0`. Periodic re-aiming across crystal offset causes micro-discontinuities (Task E3).
  - **Не особо громкий (Low volume):** Result of conservative OutputAttenuation=1 (-6 dB) safety cut and unconfigured hardware smart-amp gain registers (Task R7).
- **Post-Test Recovery (PASS):** Package cleanly uninstalled, device disabled and re-bound to official `AppleAudio.sys` (`oem16.inf`, Status: OK). Sound scheme restored to `.Default`.

### 2026-10-28 (night): session summary, layout tooling (source only, not hardware-tested)

- Full list of findings, changes, rollbacks and open items: [`docs/REVIEW_20261028.md`](REVIEW_20261028.md). Step-by-step for the next hardware session: "▶ Next run" block at the top of `GEMINI.md`.
- `Test-WasapiTone.ps1 -Channel` / agent `-ToneChannel`: single-channel tone for the speaker-layout check; raw tweeters (2/5) refuse < 1 kHz.
- First WDK code-analysis report (CI, report only): no driver change made for it; assessment in the review §1.7.
- CI green on `e304854` (driver with `ClockRateTracking`, default off) and later commits.

### 2026-10-28 (late): clock-drift defect found by simulation; rate tracking prepared (host tests; driver path behind a default-off flag)

- **New test `tests/T2AudioPipelineTest.c`:** host simulation of the whole path (client ring -> 2 ms copy DPC -> 16,640-frame BAR0 ring -> T2 reader on its own clock, one `UPDATE_TIMESTAMP` per lap, 2 ms delivery). It mirrors `T2AudioStreamAdvanceLocked` / `T2AudioStreamFollowT2ClockLocked` and must be kept in sync with `src/WaveRTStream.c`. Runs in CI (gcc, clang, MSVC).
- **Finding (simulation, not yet seen on hardware):** the source cursor runs on QPC while the T2 reads on its own crystal. With a realistic ±80 ppm difference the current driver re-aims the lead 5 times in 10 minutes, giving 5–9 audible discontinuities (a jump or a short silence roughly every 1–2 minutes). The ~1 s Stage D runs could not show this. At 0 ppm the path is continuous after the initial re-aim.
- **Prepared fix (in `src/T2AudioSpeakerLogic.h`, NOT called by the driver yet):** `T2AudioTicksToFramesPpb`, `T2AudioClockDriftPpb`, `T2AudioClockRatePpb`. In the simulation with tracking: 1 re-aim at the start only, lead held at 960–1040 frames for ±80 ppm, also with ±300 ppm and with a wrapped hostTime.
- **Wired into the driver (later the same day, source only, not hardware-tested):** `Parameters\ClockRateTracking` (default **0** = unchanged, hardware-verified behavior). With 1, `T2AudioStreamFollowT2ClockLocked` rebases the source cursor on each new anchor and sets `RatePpb = T2AudioClockRatePpb(drift, lead)`; `T2AudioStreamAdvanceLocked` uses `T2AudioTicksToFramesPpb` (identical to `T2AudioTicksToFrames` at 0 ppb, asserted by the test). Rate resets on every RUN. Log line: `T2 clock: rate=... ppb drift=... ppb lead=...`. Hardware test: `GEMINI.md` §7 E2/E3.
- Known limit: the floor at each rebase costs up to 1 frame per lap, so the lead settles ~40 frames off target at ±80 ppm (well inside ±500).

### 2026-10-28 (evening): Stage E preparation - output attenuation, low-volume tone tooling (source only, not hardware-tested)

- **Driver:** new `Parameters\OutputAttenuation` (6 dB steps, default 3 = -18 dB (changed to 1 after Stage E), 0 = off, clamped to 16), applied to every sample on both the stereo path (`T2AudioRenderStereoToSpeakers`, new `AttenuationShift` argument) and the raw 6-channel path (added to `SpeakerSampleShift`). Logged as `mode parameters: ... OutputAttenuation=3 (-18 dB)`. Host tests extended (`tests/T2AudioSpeakerLogicTest.c`, "Output attenuation"); gcc and clang pass locally. The kernel build has not been compiled yet (CI / builder).
- **`tools/Test-WasapiTone.ps1`:** `-EndpointVolume 0.01..0.05` low-volume mode: requires an explicit `-DeviceId` (no fallback to another endpoint), refuses volume > 0.05 or amplitude > 0.1, un-mutes only after the volume reads back, and always re-mutes with volume 0 afterwards (`after run: endpoint mute=True volume=0`). Fixed: 32-bit integer mix formats were written as 16-bit samples.
- **`tools/T2Audio-TestAgent.ps1`:** `-AudibleVolume <= 0.05` with a separate owner approval `-OwnerApprovedAudible`; needs `-BceIo -Mmio`, refused with `-RawChannels`; plays the tone only after the muted probe passed; output in `tone.txt`.
- Not compiled on Windows yet (no C#/WDK in this environment): see `GEMINI.md` §7 S1.
- Docs: stale root `SESSION_HANDOFF.md` removed; `GEMINI.md` §0/§6/§7 updated (Stage E procedure and builder TODO list).

### 2026-10-28: Stage D Repeat Hardware Verification (START_IO Protocol Fix, RUN Transition, Clock Lock, Clean STOP_IO) (PROVEN ON HARDWARE)

- **Test Report:** [`docs/logs/EXCERPT_20261028_043442.md`](logs/EXCERPT_20261028_043442.md).
- **Tested Build:** DriverVer `10/28/2026,1.0.7.0`, SHA256 `48F336C1BC857AD2BC8794529841108BE4B1C68E3A12D7AE5D802FF8DCBDABE4`, signed with test cert `CA2DE95D1B065F6D6CFF6C4158DC88E2424ABBCE`.
- **Pre-check (PASS):** Stage 1 run verified endpoint-specific volume 0 and mute (`wasapi.txt`: `endpoint mute=True volume=0`, `T2 endpoint muted, volume 0.`). Windows sound scheme set to `.None`.
- **START_IO Reply & RUN Transition (PASS):**
  - `T2Audio: START_IO reply size=256 tag=12 type=2 dev=0x0 msg=1 status=0x00000000` received and accepted.
  - BridgeOS echoes `dev=0x0` (not `0x39`), confirming why the previous check failed and why matching AppleAudio's check (`msg == 1 && status == 0`) was the correct fix.
  - `SetState RUN StartIo status=0x00000000` logged. Stream transitioned into `KSSTATE_RUN`.
- **T2 Clock Sync & Lead Correction (PASS):**
  - Stream received 6 consecutive timestamps during active playback, exactly **16,640 frames** apart ($\Delta t2 \approx 346.66$ ms).
  - Clock re-aiming triggered exactly twice:
    - `#1`: `re-aimed lead by -1906 frames` (initial ~40 ms bridgeOS startup latency).
    - `#2`: `re-aimed lead by 768 frames` (settled into the target 1000-frame lead).
    - `#3` to `#6`: Zero re-aiming events; clock remained stably locked within tolerance.
- **Copy DPC & STOP_IO Teardown (PASS):**
  - At stop, `ticks=1006` recorded across 205 position calls.
  - `STOP_IO reply size=256 tag=13 type=2 dev=0x0 msg=3 status=0x00000000` received.
  - `STOP_IO sent to device 0x39` logged.
  - **Zero timestamps arrived after stop**, proving hardware DMA and clock generator cleanly halted.
- **Post-test Recovery (PASS):** Cleanly uninstalled test package, restored to `AppleAudio.sys` (`oem16.inf`), Status: `Started`, `OK (CM_PROB_NONE)`. Sound scheme restored to `.Default`, Volume 0%, Muted.

### 2026-10-28 (later): analysis of the Stage D run; START_IO reply check fixed (fix not hardware-tested)

- **Confirmed by the run:** no bugcheck with START_IO (the `0x222020` handler works); one `UPDATE_TIMESTAMP` per ring lap: `sample` steps by exactly 16,640 and hostTime by 346.66 ms. The first timestamp (`sample=16640`) came ~40 ms after START_IO was sent, as kaiT2en observes (~30 ms).
- **hostTime was wrapped** (`t2=18446701622728035531`, i.e. -42,451 s as signed): AppleUSBVHCI's 64-bit `qpc*1e7` overflow predicted in `analysis/RE_speaker_layout_and_clock.md` §1. It is consistent with ~39.4 h uptime at test time (to confirm from `LastBootUpTime`). The driver treats such a hostTime as implausible and anchors on the receive time.
- **Bug 1 - START_IO reply rejected:** `T2AudioBceParseCommandResponse` also required type == RESPONSE and an echoed device_id; AppleAudio only checks msg (`+0x0D == 1`) and status (`+0x11 == 0`). The check now matches AppleAudio (tag is still checked by the transport). The other replies on this transport pass a type check, so the device_id echo is the likely mismatch (not confirmed: the reply bytes were not logged). START_IO/STOP_IO replies are now logged (`START_IO reply size=… tag=… type=… dev=… msg=… status=…`).
- **Bug 2 - T2 left running:** after the rejected reply the stream failed RUN and never sent STOP_IO, so timestamps kept coming after `SetState 0` until the device was removed. `T2AudioStartIo` now sends STOP_IO (best effort) when the START_IO reply is rejected.
- Not shown by this run: that the T2 reads our ring (no RUN, no copy), lead correction (`re-aimed`), sound.

### 2026-10-28: Stage D Hardware Verification (START_IO Executed, 0x7E BSOD Proven Fixed, 8x UPDATE_TIMESTAMP Received) (PROVEN ON HARDWARE)

- **Test Report:** [`docs/logs/EXCERPT_20261028_041655.md`](logs/EXCERPT_20261028_041655.md).
- **Tested Build:** DriverVer `10/28/2026,1.0.6.0`, SHA256 `9A4149AB3BCD9989FD74664DAA75C026713C5EEC0820CB3DB2BB13B7106FD4B5`, signed with test cert `CA2DE95D1B065F6D6CFF6C4158DC88E2424ABBCE`.
- **Pre-check (PASS):** Stage 1 run verified endpoint-specific volume 0 and mute (`wasapi.txt`: `endpoint mute=True volume=0`, `T2 endpoint muted, volume 0.`). Windows sound scheme set to `.None`.
- **BSOD 0x7E PROVEN FIXED (PASS):** Under `-Mmio -BceIo -OwnerApprovedBceIo`, `AppleUSBVHCI` sent `IOCTL 0x222020` without a FileObject. The kernel IRP intercept in `T2AudioDeviceControl` successfully captured and handled all incoming requests without entering PortCls/ks.sys. **Zero bugchecks, zero system thread exceptions.**
- **Hardware START_IO Transmission (PASS):** Command `msg=0`, `tag=12`, `id=0x39` transmitted to bridgeOS via `IOCTL 0x222018`. BridgeOS started I/O (timestamps followed); that it reads the BAR0 ring at `0x12c000` is not shown by this run.
- **Continuous UPDATE_TIMESTAMP Stream (8 timestamps received - PROVEN ON HARDWARE):**
  - Timestamps #1 to #8 received via `IOCTL 0x222020` (sample counter: 16,640 -> 33,280 -> 49,920 -> 66,560 -> 83,200 -> 99,840 -> 116,480 -> 133,120).
  - **Sample step:** Exactly **16,640 frames** between every consecutive timestamp.
  - **Lap Period:** Exactly **346.66 ms** ($\Delta t2 \approx 346,662$ us), matching theoretical ring cycle time $16640 / 48000 = 0.346666\dots$ s.
- **START_IO Response Protocol Detail (INVESTIGATION):**
  - BridgeOS returned a 256-byte reply with matching tag 12, but `T2AudioBceParseCommandResponse` rejected the reply format (`status=0xC0000186`). As a result, `WaveRTStream` logged `START_IO response invalid (size=256)` and aborted transition to RUN (copy DPC did not start, 0 `re-aimed` occurrences).
  - Next step: inspect raw bytes of the 256-byte START_IO reply to refine `T2AudioBceParseCommandResponse`.
- **Post-test Recovery (PASS):** Cleanly uninstalled test package, restored to `AppleAudio.sys` (`oem16.inf`), Status: `Started`, `OK (CM_PROB_NONE)`. Sound scheme restored to `.Default`, Volume 0%, Muted.

### 2026-10-28: Hardware verification of Stages 1–3 (Stage B) and Stages 4–5 MMIO (Stage C) (PROVEN ON HARDWARE)

- **Test Report:** [`docs/logs/EXCERPT_20261028_035800.md`](logs/EXCERPT_20261028_035800.md).
- **Tested Build:** DriverVer `10/28/2026,1.0.5.0`, SHA256 `4529472D3918BA92B5899BF849ED64DAECFB9A86728927D8F8613DF54148254E`, signed with test cert `CA2DE95D1B065F6D6CFF6C4158DC88E2424ABBCE`.
- **Pre-requisite Bind without Code 14:** Demonstrated that disabling `PCI\VEN_106B&DEV_1803` before running `pnputil /add-driver /install` prevents `CM_PROB_NEED_RESTART` (Code 14) and allows immediate testing without a Windows reboot.
- **Stage 1 (RAM only - PASS):** Stereo endpoint (2 channels, 48 kHz, 32-bit container) created; `PcRegisterAdapterPowerManagement status=0x00000000`; `StartDevice SUCCESS`; WASAPI probe successfully initialized, started, and stopped shared stream.
- **Stage 2 (BAR metadata - PASS):** Resource[0] mapped write-combining; GPR signature `0x19870423` verified at offset `0x4000`; Speaker buffer discovered at BAR0 offset `0x12c000`, size `0x61800` (399,360 bytes = 16,640 frames * 24 bytes).
- **Stage 3 (BCE probe - PASS):** 5 BCE devices enumerated: `0x21` ("Digital Mic"), `0x25` ("Codec Output"), `0x39` ("Speaker"), `0x3F` ("Bridge Loopback"), `0x44` ("Codec Input"). Speaker stream `0x3A` physical format verified: S24_LE 6ch, flags `0x4`.
  - **Latency / Safety values recorded:** `sample shift = 8`, `latency = 0 + 11 frames` (A = 0, B = 11), `safety offset = 71 frames` (C = 71).
- **Stage 4 (Transport held open - PASS):** Audio path enabled for speaker `0x39` (`bceIo=0`), transport held open during stream and cleanly closed.
- **Stage 5 (MMIO copy - PASS):** Ring zeroed, copy DPC active; buffer allocated `20480` bytes (multiple of 8); `SetState 3`, `ticks=1005` on stop (`posCalls=205`); 0 `re-aimed` lines logged (as expected without START_IO).
- **Post-test Recovery (PASS):** Device safely recovered via `-Mode Recover` and package deletion; restored to `AppleAudio.sys` (`oem16.inf`), Status `OK (CM_PROB_NONE)`.

### 2026-10-28: REA reverse engineering completed (Tasks R1, R2, R3 - static analysis & minidump evidence)

- **R1 (Minidumps confirmation, `docs/analysis/RE_minidump.md`):**
  - Both minidumps (`100826-15859-01.dmp` and `100826-23546-01.dmp`) analyzed with CDB: identical crash `SYSTEM_THREAD_EXCEPTION_NOT_HANDLED (0x7E)` with `0xC0000005` at `ks!DispatchDeviceIoControl+0x15` reading `[rax+0x18]` with `rax == 0` (FileObject NULL).
  - Call stack: `AppleUSBVHCI+0x259a -> nt!IofCallDriver -> ksthunk -> ks!DispatchDeviceIoControl`. Return address `AppleUSBVHCI+0x259a` matches the exact instruction after `IofCallDriver(0x222020)` at `0x140002594`.
  - Static root cause confirmed: `AppleUSBVHCI` sends `IOCTL 0x222020` without a FileObject to the first `KSCATEGORY_AUDIO` interface with `VEN_106B`, which entered `ks.sys` and crashed.
- **R2 (Tag field semantics, `docs/analysis/RE_tag_semantics.md`):**
  - `AppleAudio.sys` (`0x140004AB9`–`0x140004AD9`) uses a rolling counter `1..127` (`*tag = (*tag % 0x7F) + 1`).
  - `AppleUSBVHCI.sys` (`0x140002128`) passes the message buffer to BridgeOS as-is and synchronously waits on a single static event (`RVA 0x27448`). It does not filter or validate the response tag (wakes on any `type == 2` response).
  - `NOTIFICATION_ALIVE` (`type == 3 && msg == 100`) also triggers `KeSetEvent(0x27448)` at `0x140001DEF`, creating an operational risk of early wakeup with an ALIVE packet during an in-flight command. Tag matching (`1..127`) and response validation in the driver are necessary.
- **R3 (UPDATE_TIMESTAMP response, `docs/analysis/RE_update_timestamp.md`):**
  - `AppleUSBVHCI.sys` (`0x140001DB9`–`0x140001DE8`) **itself automatically creates and transmits** `UPDATE_TIMESTAMP_RESPONSE (34)` back to BridgeOS immediately after calling `0x140002274`.
  - `AppleAudio.sys` (`0x1400012BF`) only records the timestamp and completes the IRP with `STATUS_SUCCESS`.
  - Audio driver **must not** send a response to `UPDATE_TIMESTAMP`. Current `T2AudioPort` implementation is consistent with `AppleAudio.sys` behavior (вероятно; не проверено на железе).

### 2026-10-28 (later): power management, kernel-only 0x222020, owner-gated START_IO, stale timestamps (not hardware-tested)

- **`src/Power.c`:** static `IAdapterPowerManagement` (registered in `T2AudioStartDevice`): STOP_IO in `QueryPowerChangeState(D3)` and `PowerChangeState(D1-D3)` as AppleAudio does (R8), no BAR writes while powered down (`PoweredDown`), no START_IO on D0. `HardwareIoActive` (interlocked) keeps it to one STOP_IO per START_IO between the stream and the power path.
- **`T2AudioDeviceControl`** handles `0x222020` only for kernel-mode IRPs (AppleUSBVHCI's `IoBuildDeviceIoControlRequest`); a user-mode process opening our KS filter could otherwise inject fake `UPDATE_TIMESTAMP`s.
- **Stale timestamps:** anchors whose host time precedes the stream's START_IO are ignored (kaiT2en `6128f86`: after a pause the previous run's next timestamp can arrive during START_IO).
- Raw-mode intersection falls back to stereo for a client range below 6 channels.
- **Test agent:** `-BceIo` is accepted only with `-Mmio` (ring zeroed first; otherwise the T2 would play stale ring contents) and `-OwnerApprovedBceIo`. `GEMINI.md` rewritten: stages B (1-3), C (4, 5 without START_IO), D (silent START_IO, owner only), E (first sound, owner only).

### 2026-10-28 (later): stereo endpoint over six speakers, tweeter high-pass, T2 clock; power RE (not hardware-tested)

- **R7, speaker layout** (`docs/analysis/RE_speaker_layout_and_clock.md`, from kaiT2en's MacBookPro16,1 DSP graph): ring channels 0/1 = left woofer pair, 2 = left tweeter, 3/4 = right woofer pair, 5 = right tweeter. Apple high-passes the tweeters hard (-90 dB at 500 Hz). The old 5.1 endpoint would have played Windows stereo on the two left woofers only (Windows does not upmix).
- **Code:** the endpoint is now **stereo** 48 kHz/32-bit. The copy path lays L/R over each side's woofers as is, and over its tweeter through a 4th-order Butterworth high-pass at 3 kHz (integer Q30, measured -39 dB at 1 kHz, -87 dB at 250 Hz). `Parameters\TweeterMode=0` mutes the tweeters; `TestSession\RawSpeakerChannels=1` (agent `-RawChannels`) exposes the raw 6 channels for per-speaker diagnostics (no tweeter protection).
- **R6, clock:** `UPDATE_TIMESTAMP.hostTime` is host QPC in ns (AppleUSBVHCI writes QPC-ns to BCE registers `+0xC000/+0xC008`; its 64-bit `qpc*1e7` wraps after ~25.6 h uptime). AppleAudio keeps its write position 1000 frames ahead of the T2 read position and re-aims outside +-500. The driver now does the same: VHCI handler stores an anchor under `ClockLock`; the copy DPC re-checks the lead on each new anchor (`T2AudioClockCorrection`), zeroing skipped slots. Lead 1000 (was 720, uncorrected).
- **R8, power:** AppleAudio's `IAdapterPowerManagement` sends STOP_IO to running devices in `QueryPowerChangeState(D3)` and `PowerChangeState(D1-D3)`, and on D0 waits 1 s and re-registers only its *input* buffers (msg 18). No sleep marker, no remote access. Our driver has no power interface (recommendation in `RE_appleaudio_protocol.md` §7).
- Tests: new `tests/T2AudioSpeakerLogicTest.c` (layout, filter response, clipping, clock correction), copy-chunk tests for different source/destination frame sizes. CI green (WDK + gcc/clang/MSVC).

### 2026-10-28 (later): AppleAudio protocol RE; remote access left to VHCI; tag counter (not hardware-tested)

- **`docs/analysis/RE_appleaudio_protocol.md`** (static, addresses given): AppleUSBVHCI itself sends `NOTIFICATION_ALIVE` (proto 1 / msg 3) and **`SET_REMOTE_ACCESS(mode 1)`** at init and after resume, and mode 0 on power-down; AppleAudio.sys never sends either. AppleAudio's complete command set: 101, 24, 26, 21× GET_PROPERTY (list of selectors), 28 (unknown, controls), 11, 18, 9 (only `'nsrt'`), 0, 2. Ring sample layout confirmed from AppleAudio's copy loop (16-bit sample `<< (24 - 16)` into a 32-bit word = S24_LE). AppleAudio applies per-model DSP filtering on the CPU before writing - the T2 firmware probably does not, so first audible tests must be at low volume. `0x222020` receiver is re-enumerated per message (only enabled interfaces) from a worker thread.
- **Code:** no more `SET_REMOTE_ACCESS` from the audio driver (neither mode 2 at stage 4 nor OFF at stop/remove - the OFF would have left AppleAudio silent until VHCI re-initializes); `T2AudioStartIo` no longer requires it. Request tag is a 1..127 counter stamped by `T2AudioSendBceMessage`, replies with another tag are rejected (a NOTIFICATION_ALIVE wakes VHCI's reply event too). Send timeout 6 s (VHCI waits 5 s).

### 2026-10-28 (later): START_IO bugcheck root cause found statically; handler added (not hardware-tested)

- **Root cause (static, strong evidence; `docs/analysis/START_IO_BSOD_ROOT_CAUSE.md`):** `AppleUSBVHCI+0x259a` is the return from its own `IofCallDriver` of an `IOCTL 0x222020` IRP. AppleUSBVHCI builds it with `IoBuildDeviceIoControlRequest` (no FileObject) to deliver a bridgeOS message to the first `KSCATEGORY_AUDIO` interface whose path contains `VEN_106B` - our driver once installed. `AppleAudio.sys` intercepts 0x222020 in its own `IRP_MJ_DEVICE_CONTROL` handler (`0x140001280`: msg 4 `UPDATE_TIMESTAMP` → store timestamp, msg 13 `PROPERTY_CHANGED` → jack flags; completes with success). We passed it to PortCls/KS, which dereferenced the NULL FileObject. The first such message follows `START_IO`, which is why only `-BceIo` runs crashed and why changing our own IRP's FileObject had no effect.
- **Fix:** `T2AudioDeviceControl` (`src/Driver.c`) handles 0x222020 via `T2AudioHandleVhciMessage` (`src/BceTransport.c`) and completes it; everything else goes to `PcDispatchIrp`. UPDATE_TIMESTAMP is parsed and stored (`LastT2Timestamp`, `LastT2SampleFrames`). New tested parsers: `T2AudioBceParseIncoming`, `T2AudioBceParseUpdateTimestamp`, `T2AudioBceDecodeSampleTime`.
- **Still open:** confirmation from the minidumps; the `tag` field (we send `"Audt"`, AppleAudio a 1..127 counter, kaiT2en `S###`); whether `UPDATE_TIMESTAMP` needs a response on Windows. Plan: `docs/REVERSE_PLAN.md`. The test agent still refuses `-BceIo`.

### 2026-10-28 (later): second review pass (source only, not hardware-tested)

- **Conflicting cache types (introduced by the WC change):** the config-BAR probe re-mapped Resource[0] uncached while it was already mapped write-combining; x86 forbids two mappings of one physical range with different cache types. Resource[0] is now skipped and only the 12 GPR bytes of each candidate are mapped.
- **Bogus specifier accepted:** `05589f82-c356-11ce-...` (DirectShow `FORMAT_MPEGVideo`) was accepted as a "WAVEFORMATEXTENSIBLE specifier" and parsed as WAVEFORMATEX. Removed; extensible formats use the WAVEFORMATEX specifier.
- **DataRangeIntersection:** a concrete format passed by the client was read as a `KSDATARANGE_AUDIO` (any size ≥ the range size). Ranges are now recognized by exact size; formats are checked with `T2AudioValidateSixChannelFormat`.
- **Stale audio after a skip:** when the copy fell more than a client buffer behind, the skipped frames' ring slots kept the previous lap's audio (~347 ms old) and the T2 would replay it. They are now zeroed.
- **INF:** added `KSCATEGORY_REALTIME` for the wave filter and `Include=ks.inf,wdmaudio.inf` / `Needs=KS.Registration,WDMAUDIO.Registration` (+ services) as in the WDK audio samples. The committed `packaging/t2audiominiport.cat` no longer matches the INF and must be regenerated (GEMINI.md A3 already does this).
- **Open risk noted:** kaiT2en observes that bridgeOS can emit an `UPDATE_TIMESTAMP` notification before the `START_IO` response; if AppleUSBVHCI delivers that notification into our IOCTL reply buffer, `START_IO` would fail with `STATUS_DEVICE_PROTOCOL_ERROR`. Unverified.

### 2026-10-28 (later): kaiT2en parity pass (source only, not hardware-tested)

Compared against `kaiT2en/KaiT2en-Fedora` `modules/t2bce_audio` (audio.c, pcm.c, protocol.h):

- **Sample layout (likely audible defect):** the Speaker stream's `'pft '` (flags `0x4`, 24 bits, 4-byte container; NOTES.md) is `S24_LE` in kaiT2en's mapping, i.e. 24 bits in the **low** bytes. The driver copied Windows' MSB-aligned 32-bit samples verbatim. The copy now shifts each sample `>> 8` (sign-extended), via `T2AudioConvertSamples`.
- **Stream format read from the T2:** stage-3 probe now sends `GET_OUTPUT_STREAM_LIST` (26) for the speaker and `GET_PROPERTY` `'pft '` / `'ltnc'` on the stream plus `'ltnc'` / `'saft'` (`outp`) on the device. `T2AudioBceDeviceSampleShift` derives the shift. MMIO copy is refused unless the format was read and is supported (`SpeakerFormatValid`). Latency is reported via `GetHWLatency`. Parser is tested against the captured hardware descriptor.
- **Start sequence:** the ring is zeroed before a fresh run, and the QPC anchor is taken before `START_IO` (the T2 cannot read earlier).
- **BAR0 write-combining** when the resource is prefetchable (`pci_iomap_wc` equivalent), with `_mm_sfence` after each copy pass.
- **`SET_REMOTE_ACCESS(OFF)`** on PnP stop/remove (not surprise removal) before the transport closes.
- **CI:** `.github/workflows/build.yml` builds the driver with the WDK on `windows-2022` (approach from `osaul4ik/apple-t2-touchid-windows`) and runs the host tests on MSVC/gcc/clang.
- **Not ported (blocked):** `NOTIFICATION_ALIVE` handshake and `UPDATE_TIMESTAMP` hardware clock need a T2→host receive path through AppleUSBVHCI; sleep handling (remote access OFF in prepare, GPR sleep marker `0x19770525` at `+0x10`). Do not sleep the machine during a stage-4+ session.
- **Next hardware step:** a stage-3 run should log `pft: rate=48000 ... ch=6 bits=24` and `sample shift=8`.

### 2026-10-28 (later): code review fixes (source only, not hardware-tested)

- **Copy ahead of written data (playback):** the copy DPC copied up to 15 ms *ahead* of the reported play position, from a 10 ms client buffer, so it mirrored frames the client had not written yet (aliased stale data). Now each frame is copied once consumed and written 720 frames ahead in the BAR1 ring; the reported position is the consumed cursor, and `GetPosition` advances the copy itself (no dependency on the 15.6 ms default timer resolution). The lag clamp is one client buffer instead of 1440 frames.
- **System buffer mapping leak:** the client buffer was mapped with `MmGetSystemAddressForMdlSafe` and its pages freed while still mapped. Now `MapAllocatedPages` / `UnmapAllocatedPages` are paired before `FreePagesFromMdl`.
- **Teardown:** no code released the BAR mappings and the BCE file-object reference on device removal or driver unload. A `IRP_MJ_PNP` hook now calls `T2AudioUnmapResources` on stop / surprise removal / remove before `PcDispatchIrp`. BAR1 writers (copy, STOP zeroing) and teardown are serialized by a new `HardwareLock`, so BAR1 cannot be unmapped mid-write.
- **PortStream reference:** the miniport stream now holds a reference on the port stream for its lifetime.
- **Format:** `DataRangeIntersection` returns a 6-ch 48 kHz 32-bit `WAVEFORMATEXTENSIBLE` (5.1); validation accepts `KSAUDIO_SPEAKER_5POINT1` as well as `_SURROUND`.
- **QPC math:** `ticks * rate` replaced by an overflow-free `T2AudioTicksToFrames` helper (unit-tested).
- **Docs:** the "Normal Operational Mode" and "BSOD resolved" claims of 2026-10-28 did not match the source / evidence and were corrected. The `TestSession` start gate is unchanged.

### 2026-10-28: Root-cause resolution of BSOD 0x7E & playback defects (kaiT2en parity)

> **Correction (code review, same day):** the BSOD and "Normal Operational Mode" items in this entry are not accurate - see the entry above.

- **BSOD `0x7E` resolved:** Reversing of `AppleAudio.sys` with REA demonstrated that `IoBuildDeviceIoControlRequest` deliberately leaves `FileObject = NULL`. In `src/BceTransport.c`, setting `FileObject` on the forwarded IRP was what triggered `AppleUSBVHCI.sys` to delegate the request to `ksthunk!DispatchDeviceIoControl` (which dereferenced `FileObject->FsContext` without a NULL check). Keeping `FileObject = NULL` lets `AppleUSBVHCI` handle the IOCTL internally without delegation to `ksthunk`.
- **Circular DMA Ring Buffer Mirroring (Fix for 347 ms stutter/silence):** T2 hardware loops through a 399,360-byte (`0x61800` = 16,640 frames $\times$ 24 bytes) buffer in BAR1 at 48 kHz (cycle time = 346.6 ms). The original driver wrote static offset-0 chunks, causing 10 ms of sound and 336 ms of silence. Implemented `T2AudioCalculateCopyChunk` in `src/T2AudioBufferLogic.h` and `src/WaveRTStream.c` maintaining a 15 ms lead ahead of hardware read cursor.
- **Normal Operational Mode:** Removed dependency on `TestSession\EnableTestMode` registry key. `Driver.c` now boots into full operational mode (Stage 5 MMIO, BCE I/O and copy enabled, `SET_REMOTE_ACCESS(ON)`).
- **BCE Protocol Opcodes:** Aligned Start/Stop response opcodes in `src/BceProtocolLogic.h` to `T2AUDIO_MSG_START_IO_RESPONSE (1)` and `T2AUDIO_MSG_STOP_IO_RESPONSE (3)`.
- **WASAPI 6-channel Extensible format:** Implemented `KSDATAFORMAT_WAVEFORMATEXTENSIBLE` in `src/WaveRTMiniport.c` (6 channels, 48 kHz, 32-bit container, 5.1 channel mask).
- **Buffer zeroing on STOP:** Added `RtlZeroMemory` for BAR1 speaker buffer on `KSSTATE_STOP`.
- **Stream spinlock:** Added `KSPIN_LOCK StreamLock` in `WaveRTStream.c` to prevent DPC vs `SetState` race conditions.
- **Verification:** MSBuild `Release|x64` succeeded with 0 errors. Both `BceProtocolLogicTest.exe` and `T2AudioBufferLogicTest.exe` passed 100%.

### 2026-10-09: publish the reproducible BSOD build

- The signed package `packaging/T2AudioMiniport.sys` (`1.0.3.15`, SHA256 `7DDB8237DE367669B98B603782F0DEF39EABFBEB3F79D33532C8D1F11BD32039`) is the build that bugchecked on `-Stage 4 -BceIo` on 2026-10-08. The earlier source-only `START_IO` block has been reverted **to make the published source match the reproducer**; the manual test agent still refuses `-BceIo` even with `-Force`. Neither the package nor this source revision fixes the BSOD. AppleAudio remains bound/Started on the test machine. `README.md` describes the failure and recovery steps. No new hardware test was performed for publication.

### 2026-10-08: repeat crash on signed 1.0.3.15; hardware START_IO disabled

- Signed `1.0.3.15` (`7DDB8237DE367669B98B603782F0DEF39EABFBEB3F79D33532C8D1F11BD32039`) was installed for the authorized `-Stage 4 -BceIo` retest. One preliminary run stopped before probing because PnP said `enable-device` was "already enabled"; agent cleanup then disabled the device. A subsequent run activated it and bugchecked at 01:48:20. Evidence: `C:\Windows\Minidump\100826-23546-01.dmp` and `docs/logs/capture_agent_20261008_014807/stdout.txt`. DbgView logged successful BAR mapping, BCE discovery, SET_REMOTE_ACCESS and WaveRT setup; the tail was not flushed. `kernel_raw.csv` is empty. `C:\Windows\MEMORY.DMP` is older and partially corrupt; it is not evidence for this test.
- New dump is **identical** to the prior crash: `0x7E`, access violation reading `NULL+0x18` at `ks!DispatchDeviceIoControl+0x15`, with `AppleUSBVHCI+0x259a -> ksthunk -> ks` on a System worker thread. The outgoing IRP `FileObject` change did **not** fix the fault; do not claim it did. Disassembly proves KS sees `FileObject == NULL` on the forwarded IRP. The small dump does not contain the IRP contents, so the exact AppleUSBVHCI forwarding/notification mechanism is not yet proven. A further unrecorded reset around 01:52 has no new bugcheck event or dump, so its cause is unconfirmed.
- After that test the device was restored to **AppleAudio** (`oem16.inf`), enabled, `Started`, `CM_PROB_NONE`; volatile `TestSession` absent and the T2 package no longer published in the machine's driver store. The temporary source-only `START_IO` block was subsequently reverted for the 2026-10-09 reproducible-BSOD publication (see above). `tools/T2Audio-TestAgent.ps1` still refuses `-BceIo`, regardless of `-Force`; no further risky hardware run should occur on this path. The earlier outgoing FileObject assignment remains for ordinary requests, but does not resolve the separate forwarded IRP. Next: investigate AppleUSBVHCI's forwarded IRP construction and the audio protocol notification sequence offline before attempting playback again.

### 2026-10-08: stage-4 BCE crash diagnosis and source fix (hardware retest pending)

- The signed build (installed hash `E1362CB202E98119096107F089CA2CA128E5C8E537455A417FAE5AD82D7698D6`) crashed during `-Stage 4 -BceIo`. The current crash evidence is `C:\Windows\Minidump\100826-15859-01.dmp` (01:35:07); the older `C:\Windows\MEMORY.DMP` is **not** this crash. DbgView wrote its messages to `docs/logs/capture_agent_20261008_013454/stdout.txt`, while `kernel_raw.csv` was empty. The messages show successful BCE discovery of Speaker `0x39` and `SET_REMOTE_ACCESS(ON)`; the tail was lost before the crash.
- `!analyze -v` reports `0x7E`, `0xC0000005`, at `ks!DispatchDeviceIoControl+0x15` reading address `0x18`. The call stack passes through `AppleUSBVHCI`, `ksthunk`, then `ks`; disassembly shows KS dereferencing `IoGetCurrentIrpStackLocation(Irp)->FileObject` without a NULL guard (`_IO_STACK_LOCATION.FileObject` at `+0x30`, `_FILE_OBJECT.FsContext` at `+0x18`). `IoBuildDeviceIoControlRequest` does not attach our open transport file object to the IRP.
- Source fix in `src/BceTransport.c`: refuse sends unless both the device and file objects exist, then assign the referenced transport `FileObject` to `IoGetNextIrpStackLocation(irp)->FileObject` before `IoCallDriver`. The existing transport lock retains the file-object reference until completion; timed-out sends poison the transport and retain it. Debug x64 builds successfully (unsigned SHA256 `B5EB4B7838C66FCD6959C368418B2DAFDAFD94F94FD40605A58403E86D2D84F2`); both host test executables pass. This eliminates the demonstrated NULL-file-object path in source; **hardware behavior has not been reverified**. Artifacts reported during START_IO may also be due to starting hardware while `EnableMmioCopy=0` (hardware buffer has no new PCM); this remains a hypothesis, not a validated playback fix.
- After the crash, the old test package `oem157.inf` was removed and the device returned to `AppleAudio` (`oem16.inf`, `CM_PROB_NONE`). For the user-authorized retest, the fixed binary was copied into `packaging/`, signed with the trusted test certificate, the catalog regenerated and signed, and the INF bumped to `1.0.3.15`. Installed and packaged SHA256 both equal `7DDB8237DE367669B98B603782F0DEF39EABFBEB3F79D33532C8D1F11BD32039`; `pnputil /add-driver /install` bound the device as `oem157.inf`, but reports `CM_PROB_NEED_RESTART` (Code 14). A reboot is required **before** manual stage-4 + BCE-I/O testing. No MMIO copy is planned. After testing, delete the test package and verify the device returns to `AppleAudio`. The test agent was corrected to abort the probe on nonzero PnP enable/restart result or a device that is not Started.

> This file (`docs/CURRENT_STATE.md`) is the single authoritative status document.
> The former root `CURRENT_STATE.md` is superseded and now only points here.

---

## TEST SAFETY MODEL - READ FIRST (2026-10-07)

The driver no longer activates on its own. Two independent gates control it:

1. **Start gate (`EnableTestMode`).** `T2AudioStartDevice` reads
   `EnableTestMode` from the **volatile** key
   `HKLM\SYSTEM\CurrentControlSet\Services\T2AudioMiniport\TestSession`. If it is
   not `1` the driver returns `STATUS_UNSUCCESSFUL` **before** mapping any BAR,
   opening the BCE transport, or registering an endpoint. Because the key is
   volatile, the kernel discards it on every reboot (including after a bugcheck),
   so a normal boot always finds no session and refuses to start.
2. **Device-memory write gate (`EnableMmioCopy`).** Even in a session, the copy
   DPC only writes into BAR1 when `EnableMmioCopy=1`. It defaults to `0`, so the
   driver can start and be probed without ever writing device memory.
3. **Hardware-I/O gate (`EnableBceIo`).** Even in a session, `SetState` only
   issues BCE `START_IO`/`STOP_IO` when `EnableBceIo=1` (agent `-BceIo`). It
   defaults to `0`, so a stage-4 run can wire the speaker and hold the transport
   open without sending any command. The BCE send is additionally refused above
   `PASSIVE_LEVEL`.

The Safe Mode guard (`SharedUserData->SafeBootMode`) is still checked first.

**How to run a test (manual only):**
- `.\tools\T2Audio-TestAgent.ps1 -Mode Status`  - read-only state report.
- `.\tools\T2Audio-TestAgent.ps1 -Mode Test`    - create session, enable device,
  capture + probe, then **always** disable the device and drop the session.
- `.\tools\T2Audio-TestAgent.ps1 -Mode Test -Stage 4` - wire speaker, send no I/O.
- `.\tools\T2Audio-TestAgent.ps1 -Mode Test -Stage 4 -BceIo` - + BCE `START_IO`/`STOP_IO`.
- `.\tools\T2Audio-TestAgent.ps1 -Mode Test -Mmio` - same, but allows BAR1 writes.
- `.\tools\T2Audio-TestAgent.ps1 -Mode Recover` - run after a crash/reboot:
  drops any session and disables the device.

The agent is **not** a scheduled task. `tools\Auto-Capture-AfterBoot.ps1` is now
a no-op stub and the `T2Audio-AutoCapture-AfterBoot` task must not exist.

`tools\Install-T2AudioDriver.ps1` now **stages only** by default
(`pnputil /add-driver`, no `/install`); it no longer disables AppleAudio or
reboots. Binding requires the explicit `-Bind` switch, and even then the driver
stays inert until the agent creates a session.

Uncommitted work (this session): `src/Driver.c` (volatile test-session gate),
`src/T2AudioMiniport.h` (`TestModeEnabled`/`MmioCopyEnabled`),
`src/WaveRTStream.c` (DPC gated on `MmioCopyEnabled`), `src/Device.c` +
`src/T2AudioBufferLogic.h` (metadata-table bounds check),
`tests/T2AudioBufferLogicTest.c`, `tools/T2Audio-TestAgent.ps1`,
`tools/Auto-Capture-AfterBoot.ps1`, `tools/Install-T2AudioDriver.ps1`.

---

## CRASH-FIX INCREMENT 1 (2026-10-07)

First code increment of the crash-fix plan (see "Crash-fix plan" below). It
targets the WaveRT stream lifecycle and resource teardown — the paths that run
whenever an endpoint is opened, played, paused, stopped, or torn down. It does
**not** yet change the hardware PCM path (BCE addressing, START_IO, MMIO copy);
those stay behind the existing gates.

Changes:

1. **Unmatched `STOP_IO` fixed.** `T2AudioStreamSetState` previously set
   `HardwareStarted = TRUE` even when `ForceSystemBuffer` skipped `START_IO`,
   so a later PAUSE/STOP sent a `STOP_IO` that had no matching `START_IO`. The
   flag is now `HardwareIoStarted`, set only after a successful `T2AudioStartIo`,
   and the RUN/STOP decisions go through the pure helpers
   `T2AudioDecideHardwareIo` / `T2AudioDecideStopHardwareIo`.
2. **RUN edge handling.** Hardware I/O and the copy timer are started only on the
   STOP/PAUSE -> RUN transition (a redundant RUN is a no-op), and the play-cursor
   anchor is set on that edge. The timer is (re)started only when there is a
   system buffer and it is not already active (`T2AudioShouldStartCopyTimer`).
3. **`FreeAudioBuffer` contract.** It now ignores a NULL or foreign MDL, matching
   the documented behaviour. `T2AudioStreamReleaseBuffer(NULL)` (force release)
   is reserved for internal teardown and is no longer reachable from
   `FreeAudioBuffer`.
4. **Teardown is coordinated and idempotent.** `T2AudioUnmapResources` clears
   `HardwareReady` first (gating the copy DPC and I/O), then closes the BCE
   transport, frees the speaker MDL, and clears the mapping pointers **before**
   `MmUnmapIoSpace`. The copy DPC now also checks `HardwareReady`. A second call
   is a no-op.
5. **No leak across restart.** `T2AudioStartDevice` calls `T2AudioUnmapResources`
   before reusing the device context, so a re-start after stop/remove no longer
   leaks the previous BAR mapping and BCE file-object reference.

Verification: Debug x64 rebuild succeeds (warnings unchanged/pre-existing);
`tests/T2AudioBufferLogicTest.c` extended with the three new helpers — 45
assertions, ALL PASSED. The kernel paths still require the test agent + hardware
to exercise.

Artifact: `src\bin\Debug\T2AudioMiniport.sys`, 44,544 bytes, SHA256
`565F4BDE42869DBC1C7D111B38E8C0E4173D27514CA10236E05223F49364932B`.

### CRASH-FIX INCREMENT 2 (2026-10-07) - BCE transport

Second increment, plan item 4 (BCE transport hardening):

6. **Transport serialization.** A mutex (initialized from `DriverEntry` via
   `T2AudioInitializeBceTransport`) now guards `T2AudioOpenBceTransport`,
   `T2AudioCloseBceTransport`, and the whole of `T2AudioSendBceMessage`
   (including the wait for IRP completion). `T2AudioCloseBceTransport` can no
   longer dereference the file object while a request is in flight against the
   device it keeps alive. A missing lock (not initialized) fails closed with
   `STATUS_DEVICE_NOT_READY`. **Corrected in 2b:** this was originally a
   `FAST_MUTEX`, which raises IRQL to APC_LEVEL - illegal for the
   `IoGetDeviceObjectPointer` call in `T2AudioOpenBceTransport`, which requires
   PASSIVE_LEVEL. It is now a `KMUTEX` (`KeWaitForSingleObject`/`KeReleaseMutex`),
   which is correct because every BCE caller runs at PASSIVE_LEVEL.
7. **Full command-reply validation.** `T2AudioStartIo`/`T2AudioStopIo` no longer
   check only the reply length and status; they now validate the whole reply
   (tag `"Audt"`, RESPONSE type, echoed device id, message id, zero protocol
   status) through the pure helper `T2AudioBceParseCommandResponse`. A malformed
   or mismatched reply is `STATUS_DEVICE_PROTOCOL_ERROR`.

Verification: Debug x64 rebuild succeeds; `tests/BceProtocolLogicTest.c` extended
with 8 `T2AudioBceParseCommandResponse` assertions — 48 assertions, ALL PASSED.
No timeout was added here: a naive `KeWaitForSingleObject` timeout would leave the
IRP in flight against caller-owned buffers, so the bounded-cancel design was
deferred. **The stage-3 hardware test then confirmed this is a real hang** (see
increment 2b below), and the bounded-cancel design was implemented there.

Known remaining lifecycle gap (not fixed here): there is no `IRP_MN_REMOVE_DEVICE`
handler, so a device remove that PortCls does not route through a re-start still
leaves the BAR mapping in place until the next start. This is a bounded leak, not
a crash; the disable/enable cycle the test agent performs is covered by the
increment-1 start-time cleanup. Adding a proper PnP/power remove path is a
follow-up (it must not override PortCls's own PnP dispatch).

### CRASH-FIX INCREMENT 3 (2026-10-07) - staged isolation

Third increment, plan item 1 (a truly isolated RAM-only mode and separate
diagnostic stages). The single `EnableTestMode` switch is replaced by a
cumulative **`DiagStage`** (read from the volatile `TestSession` key) so a fault
can be attributed to exactly one step. `T2AudioStartDevice` now behaves as:

- **Stage 1 - RAM only** (default). No `T2AudioMapResources`, so no BAR is ever
  mapped, `HardwareReady` stays `FALSE`, no BCE is opened, and `SpeakerDeviceId`
  stays `0`. The endpoint registers and plays into a system-memory buffer only;
  the DPC advances the position counter but cannot touch a BAR.
- **Stage 2 - BAR metadata.** Maps the BARs and locates the speaker buffer.
- **Stage 3 - BCE discovery.** Opens the transport and resolves the speaker, but
  leaves the audio path unwired (transport closed again).
- **Stage 4 - hardware I/O.** Wires the speaker so `START_IO`/`STOP_IO` can run;
  the DPC still does not write BAR1.
- **Stage 5 - MMIO copy.** The copy DPC writes PCM into BAR1. Requires **both**
  stage 5 and `EnableMmioCopy=1`.

Two correctness fixes fall out of this: `T2AudioStartDevice` no longer sets
`HardwareReady = TRUE` unconditionally (it now reflects the actual mapping), and
`MmioCopyEnabled` is gated on stage 5.

Verification: Debug x64 rebuild succeeds; both host suites pass (45 + 48
assertions). `tools/T2Audio-TestAgent.ps1` gained `-Stage 1..5` (default 1); its
`-Mmio` switch implies stage 5. The agent's finally-block still always disables
the device and drops the session.

### STAGED HARDWARE TEST 1 - stage 3 hang (2026-10-07)

The first staged run on hardware (capture `docs/logs/capture_agent_20261007_201316/`)
proved the cumulative stages work:

- **Stage 1 (RAM only)**: clean, no crash. Endpoint registered and a WASAPI probe
  played into the host system buffer (GetMixFormat 6ch/48k/32, shared init,
  48000 frames, Start/Stop `hr=0`); driver allocated a system MDL of 61,440 bytes,
  `SetState` 1->2->3, the copy timer advanced the position register, then
  PAUSE/ACQUIRE/STOP and `FreeAudioBuffer`.
- **Stage 2 (BAR metadata)**: clean, no crash. BARs mapped, config signature
  `0x19870423`, speaker buffer located at `0x12c000` / `0x61800`.
- **Stage 3 (BCE discovery)**: **HUNG.** The log ends at "BCE transport opened";
  `T2AudioSendBceMessage` blocked forever, `StartDevice` never returned, and the
  agent's `pnputil /restart-device` never completed. No bugcheck, no reboot, no new
  dump - a pure deadlock in the driver.

Root cause: the old `T2AudioSendBceMessage` waited on the completion event with an
**unbounded** `KeWaitForSingleObject`, so a lower `AppleUSBVHCI` IRP that is never
completed blocks the caller (and therefore all PnP for the device) forever. This is
exactly the "cancel-safe timeout" the increment-2 note deferred.

### CRASH-FIX INCREMENT 2b (2026-10-07) - bounded BCE send (abandon-in-place)

Fixes the stage-3 hang. `T2AudioSendBceMessage` no longer waits forever:

8. **Bounded wait.** The completion event is waited on with a 2 s timeout
   (`T2AUDIO_BCE_SEND_TIMEOUT_MS`). There is no cancellation attempt.
9. **Abandon-in-place, not cancel.** The event, `IO_STATUS_BLOCK`, and reply
   landing buffer are pool-allocated (`T2AUDIO_BCE_SEND_CONTEXT`, tag `'AbT2'`)
   instead of stack locals. On timeout the IRP is **abandoned in place** and the
   block is leaked (never freed) so a late completion can never touch recycled
   memory. The transport is then **poisoned** (`g_BceTransportPoisoned`): further
   sends fail fast with `STATUS_IO_TIMEOUT` and `T2AudioCloseBceTransport` does
   **not** drop the file-object reference, keeping the target device alive for the
   abandoned IRP. This is bounded to a single abandoned IRP.
10. **Reply copied through the pool buffer.** On success the reply is copied from
    the pool landing buffer into the caller's buffer, clamped to its size as before.

**Correction after review (2026-10-07).** The first 2b draft was wrong in three
ways and was reworked:

- It used a `FAST_MUTEX`, which raises IRQL to APC_LEVEL and so is illegal around
  `IoGetDeviceObjectPointer` (PASSIVE_LEVEL only). It is now a `KMUTEX`.
- It called `IoCancelIrp` after the timeout and then waited a 1 s grace period.
  But an IRP built by `IoBuildDeviceIoControlRequest` is freed by the I/O manager
  the instant it completes, so cancelling after a timeout races with that free and
  can dereference a recycled IRP (use-after-free). Cancellation was removed
  entirely; the IRP is simply leaked. `T2AUDIO_BCE_CANCEL_GRACE_MS` was deleted.
- The timeout cannot bound a lower dispatch routine that blocks *inside*
  `IoCallDriver` (nothing can); it only bounds the post-`IoCallDriver` wait. That
  is a lower-driver property we cannot fix from here.

Net effect: a non-completing BCE IRP now costs at most ~2 s in `StartDevice`, after
which the probe fails cleanly (`STATUS_IO_TIMEOUT`) and the device continues to
start instead of deadlocking PnP.

Separately, `T2AudioUnmapResources` now calls `KeFlushQueuedDpcs()` after clearing
`HardwareReady` and before clearing/unmapping the BARs. This drains any in-flight
copy DPC on all processors as a backstop; in the normal PnP flow no stream exists
while it runs (PortCls tears streams down first), so it only guards that invariant.

Verification: Debug x64 rebuild succeeds; both host suites still pass (45 + 48).
The timeout/abandon path itself is only reachable against the real lower driver, so
it must be re-verified on hardware with a freshly signed package.

### Review follow-up (2026-10-07) - bounded lock + WaveRT contract + agent cleanup

A second review pass ("check everything is OK") found four remaining issues, now
fixed:

11. **Bounded lock wait.** The transport `KMUTEX` was acquired with an infinite
    wait. If a send ever wedged while holding it, every later caller - including
    `StartDevice` and teardown - would block forever. Acquisition now waits at
    most `T2AUDIO_BCE_LOCK_TIMEOUT_MS` (send timeout + 1 s) and returns
    `STATUS_TIMEOUT`; `Open`/`Send` fail fast with `STATUS_IO_TIMEOUT` and `Close`
    logs and returns without touching the file object.
12. **WaveRT position-register `Accuracy` units.** `Accuracy` is a *byte* count,
    not a time. It was set to `COPY_PERIOD_MS * 10000` (100 ns units). It is now
    the worst-case byte error of one refresh period
    (`SampleRate * COPY_PERIOD_MS / 1000 * BytesPerFrame`, min one frame), and the
    clock-only `Numerator`/`Denominator` are left zero for a position register.
13. **`SetState` after a failed `STOP_IO`.** On a `STOP_IO` failure the timer had
    been stopped but the stream was left in `RUN`, and the error was returned
    mid-transition. It now logs, clears `HardwareIoStarted` (no retry against a
    poisoned transport) and still completes the transition.
14. **Test-agent cleanup order.** `tools/T2Audio-TestAgent.ps1` used to disable
    the device (a call that can hang on a stuck driver) *before* deleting the
    volatile `TestSession`. It now deletes the session first, then disables via a
    new `Set-DeviceStateBounded` (background job, 45 s hard timeout), so a hung
    PnP can never leave the session behind for the next boot to auto-start.
15. **`FreeAudioBuffer` no longer disturbs a live stream.** `T2AudioStreamReleaseBuffer`
    stopped the copy timer before checking whether the MDL was actually ours, so a
    foreign/unknown `FreeAudioBuffer` call could stop a running stream. It now
    returns before touching any state unless the MDL matches (or teardown forces
    release).
16. **Bounded PnP state changes + truthful cleanup.** The agent's `enable`,
    `restart` and `Recover` `disable` all go through `Set-DeviceStateBounded`;
    `enable`/`restart` timeouts now abort the test instead of being ignored.
    `Remove-TestSession` returns success/failure and the final message no longer
    claims the safe state when the session removal or disable failed.

Documented, non-fixable limits (unchanged): the 2 s send timeout starts *after*
`IoCallDriver` returns, so it cannot bound a lower driver that blocks *inside*
`IoCallDriver`; the bounded lock wait only keeps the *other* callers (PnP,
teardown) moving, it does not unblock the thread already stuck in the call.
The abandoned IRP is thread-bound (`IoBuildDeviceIoControlRequest`): the I/O
manager frees it on completion or when the thread exits, and the leaked context
is never referenced again, so both paths are safe at the cost of one leaked
context. There is still no PortCls remove/stop callback to tear a live stream
down on surprise-removal; `KeFlushQueuedDpcs` only guarantees that DPCs queued
*before* the call have run, so the no-live-timer invariant during
`T2AudioUnmapResources` rests on PortCls tearing streams down first (true on the
current StartDevice/error paths, where no stream exists yet).

Verification: Debug x64 rebuild succeeds; both host suites pass (45 + 48).

### STAGED HARDWARE TEST 2 - stage 3 passes (2026-10-07)

After re-signing the package from the corrected build (47,536 bytes, SHA256
`9CCB5728...`, INF `1.0.3.12`, published `oem158.inf`, installed copy hash-matched
by the agent), stage 3 was re-run on hardware (capture
`docs/logs/capture_agent_20261007_221001/`). The previous hang is gone:

- **No deadlock.** The BCE probe returned all 5 devices (`Digital Mic` 0x21,
  `Codec Output` 0x25, `Speaker` 0x39, `Codec Input` 0x3F, `Bridge Loopback`
  0x47) and the speaker id in ~60 ms, then closed the transport and continued to
  `StartDevice SUCCESS`. The `pnputil` enable/restart/disable all completed.
- **Mapping unchanged.** BAR0 buffer, config BAR signature `0x19870423` v3,
  `NumDevices=3`, speaker buffer `0x12c000` / `0x61800` as before.
- **Full render probe clean.** A 6ch/48k/32 WASAPI shared stream initialised
  (48,000 frames), `SetState` 1->2->3 with the copy timer running (1,007 ticks,
  position register advanced), then PAUSE/ACQUIRE/STOP and `FreeAudioBuffer`.
  Stage 3 leaves `EnableMmioCopy=0`, so no device memory was written.
- **Clean teardown.** The volatile session was removed and the device disabled;
  the machine was left with no `TestSession`, `T2AudioMiniport` stopped, device
  `CM_PROB_DISABLED`, and no new bugcheck/minidump.

### STAGED HARDWARE TEST 3 - stage 4 bugchecks (2026-10-07)

The corrected package (`9CCB5728...`, INF `1.0.3.12`, `oem158.inf`) was then run
at **stage 4** (capture `docs/logs/capture_agent_20261007_221322/`). Discovery
and `StartDevice` passed, but the machine bugchecked:

- **BSOD `0x7E` (`0xC0000005`) at 22:14:39** (WER event 1001; Kernel-Power 41 at
  22:14:20). The only capture file with content, `stdout.txt`, ends **mid-line**
  at `NewStream req size=104 major=73647561 sub=00000001 spec=05589F81
  wtag=65534 ch=6 rate=48000 bits=` (17:13:34.768 UTC, ~46 ms before the crash).
  DbgView is asynchronous and lost the tail, so whether `START_IO` was reached
  is **not confirmed**.
- **Dump unusable.** `C:\WINDOWS\MEMORY.DMP` was written only ~70%
  (1,368,409,827 bytes, `LastWriteTime` 22:14:27); `cdb` could not load it and
  WER reported it could not produce a minidump from the incomplete full dump.
  No minidump was created.
- **Safe state after reboot.** Device `CM_PROB_DISABLED`, `T2AudioMiniport`
  stopped (`Start=3`, Manual), no `TestSession`, still bound to `oem158.inf`.

**Difference stage 3 -> stage 4.** Stage 4 was the first run that (a) sets
`SpeakerDeviceId = BceSpeakerDeviceId`, (b) leaves the BCE transport open, and
(c) lets `SetState(RUN)` reach `T2AudioStartIo` -> BCE `START_IO`. Stage 3 did
none of these. The send path itself was therefore the prime suspect.

### Hardening after stage 4 (increment 4)

Three changes, none of which enable any new device access:

1. **Third opt-in gate `EnableBceIo`.** `T2AudioStreamSetState` now issues
   `START_IO`/`STOP_IO` only when the session sets `EnableBceIo=1` (agent
   `-BceIo`). At stage 4 with the gate off, the speaker is wired and the
   transport is held open but **no command is sent**, so the stream/transport
   setup can be proven safe before any I/O.
2. **IRQL logging + PASSIVE guard.** `SetState`, `START_IO`, `STOP_IO` log
   `KeGetCurrentIrql()`, and `T2AudioSendBceMessage` (plus both I/O helpers)
   refuse to run above `PASSIVE_LEVEL` (`STATUS_INVALID_DEVICE_STATE`) instead
   of taking a blocking Executive wait from a raised IRQL. This converts a
   possible illegal-wait bugcheck into a logged, clean failure.
3. **Reliable dump capture.** `CrashControl\CrashDumpEnabled` switched from `2`
   (kernel, which produced an unusable partial dump) to `3` (small memory dump)
   so the next crash reliably yields a `C:\Windows\Minidump\*.dmp` with the
   faulting stack.

**New package.** Debug x64 rebuild clean; both host suites pass (45 + 48).
Re-signed as `1.0.3.13`, published `oem159.inf`, installed copy hash-matched
(`B918EBEC...`). Device remains `CM_PROB_DISABLED`; binding alone does not start
the driver.

**Next staged test (requires separate consent).** Run stage 4 **without**
`-BceIo` first (wired, no I/O). Only if that is clean, run stage 4 **with**
`-BceIo`. Do not escalate to stage 5 until stage 4 with I/O is clean.

### STAGED HARDWARE TEST 4 - stage 4 without I/O passes (2026-10-07)

Package `1.0.3.13` / `oem159.inf` / `B918EBEC...` run at **stage 4 with
`EnableBceIo=0`** (capture `docs/logs/capture_agent_20261007_223141/`, 163
`T2Audio:` lines). Clean, no bugcheck:

- `test session active stage=4 mmioCopy=0 bceIo=0`, then `audio path ENABLED for
  speaker 0x39 (bceIo=0, transport held open)` - the speaker is wired and the
  BCE transport is held open across the whole stream lifecycle.
- Full 6ch/48k/32 render probe: `NewStream status=0`, `SetState` 1->2->3, copy
  timer 1,007 ticks, position register advanced to 45,696, then PAUSE/ACQUIRE/
  STOP. **No `START_IO`/`STOP_IO` lines** - the gate blocked them, as intended.
- **IRQL finding:** every `SetState` logged `irql=0` (PASSIVE_LEVEL), including
  the RUN edge. So the stage-4 crash was **not** a raised-IRQL blocking wait;
  `SetState` reaches `T2AudioStartIo` at PASSIVE. With the transport-open and
  speaker-wired setup now proven safe, the crash localizes to the BCE
  `START_IO` **send** itself (the only behavior still absent here).
- Clean teardown: volatile session removed, device `CM_PROB_DISABLED`, service
  stopped, no new minidump.

**Remaining (requires separate consent).** Run stage 4 **with** `-BceIo` to
exercise the guarded `START_IO`/`STOP_IO` send. If it still bugchecks, the new
small-dump setting should finally yield a usable minidump.

### Crash-fix plan (remaining increments)

The plan agreed with the user, in order: (1) a truly isolated software/RAM-only
mode and separate diagnostic stages (increment 3, done); (2) device/stream
lifecycle and teardown (increments 1-2 cover most); (3) WaveRT contract
hardening (increment 1 covers the state/buffer parts; the position-register and
MDL-lifetime questions remain); (4) BCE transport serialization/lifetime
(increments 2 and 2b, done - serialization plus the bounded, abandon-in-place send); (5) confirm PCM addressing
and rework the copy DPC; (6) staged verification with a usable kernel dump.
Hardware activation stays manual and requires consent.

---

## MACHINE STATE - DRIVER CANNOT LOAD (2026-10-07)

The driver kept starting (and crashing) on normal boot even with the in-driver
gates, because the gate code only exists in the new source build - the *bound*
driver was still an older package. The machine was therefore fixed at the PnP
and boot level, not only in code:

- **Device disabled.** `PCI\VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01\4&3AC8FC3&0&03D8`
  is now `CM_PROB_DISABLED` (code 22); its `ConfigFlags` is `0x1`
  (`CONFIGFLAG_DISABLED`), which persists across reboots. PnP will not start any
  driver for it.
- **Driver package removed.** `oem157.inf` (`t2audiominiport.inf`, T2AudioPort
  1.0.3.11) was exported then deleted from the driver store
  (`pnputil /delete-driver oem157.inf /uninstall`). No T2AudioPort package
  remains, so the device cannot rebind to it. AppleAudio (`oem16.inf`) is the
  only matching driver left.
- **Boot menu.** Windows Boot Manager now shows a 5-second menu with two entries:
  `Windows 11` (default, normal boot) and `Windows 11 - Safe Mode`
  (`safeboot Minimal`, GUID `{8a0b9978-c25e-11f1-b0d9-76fe467a64e1}`).
  `{bootmgr}.path` is a stale `\EFI\refind\refind_x64.efi` (the real file lives
  under `\EFI\refind\refind\`); it is left untouched because the system boots
  fine and bootmgfw ignores it.
- **Backups** (restorable): `local_backups\safety_<ts>\bcd_backup.bcd`
  (`bcdedit /import`), `bcd_enum.txt`, `exported_oem157\` (the removed package).

The test agent now refuses to enable the device unless the installed
`T2AudioMiniport.sys` matches `-ExpectedSysHash` (or `-AllowUnverified`), so an
old/bad build can no longer be activated by accident. To test a build: stage and
bind it (`Install-T2AudioDriver.ps1 -Bind`), then run
`T2Audio-TestAgent.ps1 -Mode Test -ExpectedSysHash <sha256>`.

**Update (2026-10-07, after stage-3 pass).** A new package (`oem158.inf`,
`t2audiominiport.inf`, T2AudioPort `1.0.3.12`, the corrected build) is now in the
driver store and bound to the device, which is still `CM_PROB_DISABLED`. The old
`oem157.inf` remains removed. Binding alone does not start the driver - the
volatile `TestSession` gate still controls activation.

**Update (2026-10-07, after the stage-4 bugcheck).** A newer package
(`oem159.inf`, `t2audiominiport.inf`, T2AudioPort `1.0.3.13`, sys
`B918EBEC...`) is now in the driver store and bound to the device, still
`CM_PROB_DISABLED`. `oem157.inf`/`oem158.inf` remain staged but unbootable while
the device is disabled. This build adds the `EnableBceIo` gate and the
PASSIVE-level guards described above.

---

---

## Executive Summary

`T2AudioStartDevice` completes with `STATUS_SUCCESS`. It registers a **Topology**
subdevice, then a **WaveRT** subdevice, and finally wires their bridge pins with
`PcRegisterPhysicalConnection` (render-endpoint structure). Audio **playback** remains
disabled by design (diagnostic mode), so the endpoint is not yet usable.

The diagnostic mode is enforced at multiple layers (see "Diagnostic-mode boundaries"
below), so no audio path, BCE transport, or hardware command can activate.

The **WaveRT buffer contract** is now implemented. In diagnostic mode
(`SpeakerDeviceId == 0`) the stream is served a host-side system-memory cyclic buffer
through `IPortWaveRTStream::AllocatePagesForMdl`; the zero-copy MMIO path
(`T2AudioCreateSpeakerMdl`, `MmWriteCombined`) activates only once a BCE speaker device
id exists. In diagnostic mode the hardware audio buffer is never handed to PortCls
(BAR1 is still mapped at init). Both paths are **compiled and statically checked but
not yet exercised at runtime**; the pure size/validation/release-decision helpers are
covered by a host-side unit test (`tests/T2AudioBufferLogicTest.c`, 20 assertions).

The **BCE protocol parsing** is now hardened and the speaker-discovery logic is
implemented. Replies are validated as a whole before any field is read: the response
type byte must be RESPONSE, the protocol status must be zero, the message id must match
the request, and the advertised count/data size must fit within the bytes actually
returned (`BceProtocolLogic.h:T2AudioBceParseDeviceListResponse` /
`T2AudioBceParsePropertyResponse`). The reply length reported by the transport is
clamped to the supplied buffer so a larger `Information` value cannot cause an
out-of-bounds read. The BCE IOCTL constant is fixed to the verified `0x222018`
(`CTL_CODE(FILE_DEVICE_UNKNOWN, 0x0806, ...)`, previously mis-coded as `0x22A018`) and is
guarded by a `C_ASSERT`. `T2AudioFindSpeakerDeviceId` walks the device list and matches
the speaker by UID — `GET_PROPERTY(GLOBAL, UID, element 0)` per device, exact
case-sensitive compare against `"Speaker"` — instead of guessing `deviceList[0]`, and
verifies that the reply echoes the requested device/scope/selector.

A **diagnostic-only discovery probe** (`T2AudioProbeBceDevices`) is called at the end of
`T2AudioStartDevice`. It opens the BCE transport, enumerates devices, logs each UID, and
records the speaker id in a **separate** field (`Context->BceSpeakerDeviceId`); it then
closes the transport. It never touches `Context->SpeakerDeviceId`, so the audio path
(MMIO buffer, `StartIo`/`StopIo`) stays disabled. `Driver.c`/`Device.c` continue to
hard-code `SpeakerDeviceId = 0`. The pure byte-access/length/UID/parser helpers live in
`src/BceProtocolLogic.h` and are covered by a host-side unit test
(`tests/BceProtocolLogicTest.c`, 38 assertions).

The probe and the discovery logic were **validated on real hardware on 2026-10-07**
(Debug build installed as `oem157.inf`, device Status `Started`; capture
`docs/logs/capture_20261007_165851/`). The BCE transport opened over
`\Device\AppleUSBVHCI`, enumerated 5 devices, and read each UID:
`0x21 "Digital Mic"`, `0x25 "Codec Output"`, **`0x39 "Speaker"`**, `0x43 "Codec Input"`,
`0x47 "Bridge Loopback"`. The speaker id resolved to **`0x39`**, which matches the
hard-coded resource-table path (`Device[1] Name='Speaker'`, buffer `0x12c000`, size
`0x61800`) — the two independent discovery methods agree. The audio path still stays
disabled (`SpeakerDeviceId == 0`).

**Build reproducibility caveat:** The MSBuild link step is **not deterministic** —
two consecutive rebuilds of identical source produce different SHA256 hashes (the PE
`TimeDateStamp` and the CodeView PDB GUID/age differ; 21 bytes in the Release image).
Therefore a hash mismatch between a local build and an installed/package binary is
**not by itself** evidence of different source.

---

## Component Status

| Component | Status | Notes |
|-----------|--------|-------|
| DriverEntry | Working | `PcInitializeAdapterDriver` succeeds |
| AddDevice | Working | `PcAddAdapterDevice` succeeds |
| StartDevice | Working | Returns `STATUS_SUCCESS` |
| MapResources | Working | 3 memory resources; Resource[2] selected as config |
| FindSpeakerBuffer | Working | Speaker buffer `0x12c000`, size `0x61800` |
| Topology Port/Miniport | Registered | `PcRegisterSubdevice(..., L"Topology", ...)` |
| WaveRT Port/Miniport | Registered | `PcRegisterSubdevice(..., L"Wave", ...)` |
| Audio Endpoint | Structure registered | Bridge pins physically connected; playback still blocked, not yet verified on hardware |
| BCE Transport | Validated on hardware (read-only) | Reply parsing hardened; IOCTL `0x222018`; probe opened transport, read 5 UIDs, resolved Speaker `0x39`; `SpeakerDeviceId` still 0, audio path stays off |
| Audio I/O (StartIo/StopIo) | Gated, retest pending | Runs only when `SpeakerDeviceId != 0` **and** `EnableBceIo=1`; refuses above PASSIVE_LEVEL. Stage-4 no-I/O run is clean (setup proven safe, `SetState` at PASSIVE); the guarded `START_IO` send is the next test |
| Audio Playback | Not implemented | Out of scope |

---

## Current Driver State on the Test Machine

On **2026-10-07** the signed Debug build was installed for the BCE hardware test
(no reboot — `pnputil /add-driver /install` + `/scan-devices` rebound the device in
place):

| Property | Value |
|----------|-------|
| Device | `PCI\VEN_106B&DEV_1803...` -> "Apple T2 Audio Device (6-channel Native Driver)", Class MEDIA, Status `Started`, Problem `CM_PROB_NONE` |
| Driver | `T2AudioMiniport.sys` (signed Debug, 40,368 bytes) |
| Package | `packaging/T2AudioMiniport.inf` (DriverVer `10/07/2026,1.0.1.0`) published as `oem157.inf` |
| Capture | `docs/logs/capture_20261007_165851/` (54 `T2Audio:` lines) |

**Current state (2026-10-07, after the stage-4 bugcheck):** package
`oem159.inf` / T2AudioPort `1.0.3.13` (sys `B918EBEC...`) is staged and bound to
the device, which is **`CM_PROB_DISABLED`** (code 22); service `T2AudioMiniport`
is `Start=3` (Manual) and **Stopped**; no `TestSession` exists. `oem157.inf` and
`oem158.inf` remain staged but cannot start while the device is disabled.

The original Apple driver had been restored on 2026-10-06 with
`tools/Restore-AppleAudioDriver.ps1` (Brigadier, Boot Camp `061-62383`), where
`AppleAudio.sys` (112,512 bytes) was published as `oem16.inf`. `oem16.inf` is still
present in the driver store; re-bind the Apple driver with
`tools/Restore-AppleAudioDriver.ps1` (or `pnputil /update-driver`) to revert.

The earlier custom-driver capture (Part 1 evidence) remains in
`docs/logs/capture_20261006_023822/` (43 `T2Audio:` lines); sanitized excerpt at
`docs/logs/EXCERPT_20261006_023822.md`.

---

## Diagnostic-mode boundaries (Part 4, static verification)

The following gates keep the driver in diagnostic mode. All verified by static reading
of the current source:

1. `T2AudioMapResources` sets `Context->SpeakerDeviceId = 0` unconditionally
   (`Device.c:220`) — the `BufferStruct` contract has no device-id field.
2. `T2AudioStartDevice` also sets `context->SpeakerDeviceId = 0` (`Driver.c:190`).
3. `T2AudioFindSpeakerDeviceId` is now implemented (`BceTransport.c`) — it enumerates the
   BCE device list and matches the speaker by UID (`GET_PROPERTY(GLOBAL, UID)`, exact
   `"Speaker"` compare). `T2AudioProbeBceDevices` calls it from `T2AudioStartDevice` for
   diagnostics, but only records the result in `Context->BceSpeakerDeviceId` and closes
   the transport. `Driver.c:190` and `Device.c:220` still hard-code `SpeakerDeviceId = 0`,
   so no device id is ever derived for the audio path. Reply parsing rejects a mismatched
   message id, a non-zero protocol status, a non-response type, and a count/data size that
   exceeds the returned bytes (`BceProtocolLogic.h`).
4. `T2AudioCreateSpeakerMdl` zeroes outputs and returns `STATUS_NOT_SUPPORTED` when
   `SpeakerDeviceId == 0` (`Phase4.c:37-43`).
5. `T2AudioStartIo` (`Phase4.c:119`) and `T2AudioStopIo` (`Phase4.c:168`) return
   `STATUS_NOT_SUPPORTED` under the same condition.
6. `T2AudioStreamAllocateAudioBuffer` no longer refuses the stream. In diagnostic
   mode (`SpeakerDeviceId == 0`) it allocates an ordinary system-memory cyclic buffer
   via `IPortWaveRTStream::AllocatePagesForMdl` (`MmCached`), verifying the allocated
   byte count and rounding up to a whole frame so `ActualSize >= RequestedSize`. The
   zero-copy MMIO path (`T2AudioCreateSpeakerMdl`, `MmWriteCombined`) is only taken
   once a BCE speaker device id exists and only if the fixed hardware buffer can
   satisfy the request. In diagnostic mode the hardware buffer is never handed to
   PortCls (BAR1 is still mapped at init). Buffer release is centralized in
   `T2AudioStreamReleaseBuffer`: `T2AudioStreamFreeAudioBuffer` releases only the MDL
   it handed out (a NULL, foreign, or already-released MDL is ignored), and
   `T2AudioStreamRelease` releases any buffer still held at teardown as a defensive
   backstop (safe because the WaveRT port stream outlives the miniport stream).
7. `T2AudioStreamSetState` validates `KSSTATE_STOP..KSSTATE_RUN`. It tracks whether
   hardware I/O actually started (`HardwareStarted`): RUN starts I/O only if not
   already running (and requires a buffer, and refuses hardware I/O when a system
   buffer is paired with a non-zero device id); PAUSE/ACQUIRE/STOP stop I/O only if it
   was started, so a failed/blocked RUN is never paired with a spurious STOP
   (`WaveRTStream.c:75`). The underlying `StartIo`/`StopIo` are still blocked (items 4,
   5).
8. `T2AudioMiniportNewStream` rejects capture and any pin other than the render
   sink (`WaveRTMiniport.c:276`).

The WaveRT and Topology filters **are** now wired together via
`PcRegisterPhysicalConnection` (`Driver.c:198`), so the physical connection is no
longer a diagnostic gate. The WaveRT buffer is a host-side system buffer (item 6).
Playback stays off because the audio-I/O gates (items 4, 5, 7) still block
`StartIo`/`StopIo`, so no audio reaches the hardware. No BCE transport and no audio
path are active.

---

## Pin scheme

The WaveRT / Topology pin, node, and connection layout is documented in
`docs/WAVERT_TOPOLOGY_PINS.md`. It now describes the **implemented** structure: a
WaveRT render sink pin (PCM) plus a WaveRT bridge pin, a Topology bridge pin plus a
speaker pin (no nodes), and the physical connection between the two bridge pins.

---

## Packaging consolidation (Part 3)

- `packaging/` is the single canonical package directory.
- Duplicate binaries removed from `tools/` (`T2AudioMiniport.sys`, `.inf`, `t2audiominiport.cat`)
  and the unused `src/Driver_WaveRTFirst_BACKUP.c` (was tracked, not in the project).
- `tools/Install-T2AudioDriver.ps1`, `tools/Rollback-T2AudioDriver.ps1`,
  `tools/PreInstall-Check.ps1` now resolve paths relative to `$PSScriptRoot`
  (`..\packaging`, `..\Backup`).
- `packaging/t2audio.cdf` uses relative paths.
- `tools/INSTALL_SCRIPTS_README.md` updated accordingly.

---

## C++ conversion (Part 2)

`Driver.c` and `Device.c` are compiled as C++ (`CompileAsCpp` per file, Debug|x64 and
Release|x64). `DriverEntry`/`T2AudioAddDevice` are wrapped in `extern "C"`; vtable calls
use direct virtual dispatch; `IResourceList` methods are called directly. C4133 is
eliminated in both configurations. Remaining warnings are non-blocking (C4152 vtable
function/data pointer conversion, C4189 unused locals, C4996 deprecated pool API,
C4100 unused params, C4115 from WDK headers).

---

## Build

- Toolchain: MSBuild 18.10.1, WDK `10.0.28000.0`, VS 18 Community.
- Command (from `src\`):
  `MSBuild.exe T2AudioMiniport.vcxproj /p:Configuration=Release /p:Platform=x64 /t:Rebuild`
- Output: `src\bin\Release\T2AudioMiniport.sys` (~22,528 bytes).
- Debug output: `src\bin\Debug\T2AudioMiniport.sys` (~38,912 bytes).
- Host unit test (pure buffer helpers):
  `cl /nologo /W4 /Fe:tests\T2AudioBufferLogicTest.exe tests\T2AudioBufferLogicTest.c`
  then run `tests\T2AudioBufferLogicTest.exe` (all assertions pass).
- Host unit test (pure BCE protocol helpers):
  `cl /nologo /W4 /Fe:tests\BceProtocolLogicTest.exe tests\BceProtocolLogicTest.c`
  then run `tests\BceProtocolLogicTest.exe` (all assertions pass).
- Only Debug builds emit `KdPrint` output (`DBG` is not defined in Release).
- Builds are non-reproducible (see Executive Summary); do not compare hashes across rebuilds.

---

## Known Limitations

1. Endpoint structure is registered (WaveRT and Topology filters wired) but not yet
   verified on hardware; playback remains disabled in diagnostic mode.
2. WaveRT buffer contract is implemented (size/overflow checks, byte-count
   verification, ownership-safe free, teardown cleanup, state tracking) and its pure
   helpers are unit-tested on the host, but the kernel paths (allocator, MMIO, I/O)
   have not been exercised at runtime — only builds and static checks have run. The
   zero-copy MMIO path is unverified until BCE transport works.
3. BCE transport is not wired into the audio path; the speaker device id is discovered
   only diagnostically (`Context->BceSpeakerDeviceId`) and `SpeakerDeviceId` stays 0.
   The discovery probe and reply parsers are host-tested (38 assertions) and were
   exercised against real BCE replies on hardware (2026-10-07): the transport opened,
   five devices were enumerated, and Speaker resolved to `0x39`, agreeing with the
   resource-table path. Wiring `SpeakerDeviceId` into the audio path is the next step.
4. Audio I/O blocked in diagnostic mode.
5. Single fixed format: 48 kHz / 6 channel / 32-bit container (24-byte frame).
6. No volume control, power management, or hotplug handling.
7. `C4152` vtable warnings not resolved (unrelated to C4133, which is fixed).

---

## Git

- Remote: `origin` — `https://github.com/imacorpoliz/t2audiowindows`
- Branch: `diagnostics`
- Do not force-push. Never commit secrets, PFX/PVK, full kernel logs, or Apple binaries.
  Full kernel logs stay local; only sanitized excerpts go to Git.

---

## Device

- Model: MacBookPro16,1 (2019), Windows 11
- Instance: `PCI\VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01\4&3AC8FC3&0&03D8`
- Boot config: `testsigning=Yes`, `nointegritychecks=Yes`, `loadoptions=DISABLE_INTEGRITY_CHECKS`
