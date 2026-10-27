# Current State

- Target: MacBookPro16,1, Windows 11, T2; goal is six independent speaker channels.
- Restrictions remain: static/offline work only; no driver install/patch, MMIO writes, START/STOP, or hardware playback.
- AppleAudio.sys 6.1.8100.4: 122752 bytes; SHA256 `cd8b81fabc9be6127a082da22c4cc046a4d46e0225eb6baf6d27e0dc9862a150`.
- Windows KS probe accepts stereo PCM16/48 kHz; tested six-channel formats return Win32 1169.
- Native AppleUSBVHCI query confirms Speaker `0x39`, stream `0x3A`, 48 kHz, six channels, signed 24-in-32 interleaved, 24 bytes/frame. This is format metadata, not proof of a PCM write API.
- AppleAudio static analysis proves a six-iteration processing branch and pool-staging to ring-buffer copy. It does not prove physical speaker order or stable public access to the internal path.
- The phase-5 `+8` interface adjustment for the allocation method is proven, but identity with the direct writer caller remains unproven.
- Local `kait2en/` exists, but has no `.git`; source identity is recorded in `SOURCE_MANIFEST.sha256`. A separate upstream checkout is pinned to `ed5a3613a49399a8b426850cdde8c3eaf43f71e9` in `upstream-kait2en/`.
- KaiT2en `dsp/profiles/16_1/graph.json` is host-side PipeWire processing: two inputs and six outputs, targeting `alsa_output.hw_Audio_0`.
- The graph includes built-in copy, biquad, mixer, delay and convolver nodes plus LV2 autogain, virtual bass and multiband compressor; FIR WAV paths are external dependencies.
- The graph's user-facing capture side is stereo, while playback side is six-channel `AUX0..AUX5`; names do not establish physical speaker order.
- KaiT2en `modules/t2bce_audio` owns Linux PCI/BCE transport, BufferStruct, ALSA PCM exposure, one substream per remote stream, workqueue-controlled I/O, and timestamp interpolation.
- Direct Windows reuse is limited by Linux kernel/ALSA/BCE APIs and GPL obligations; DSP data/algorithm inventory is more portable than transport code.
- KaiT2en lifecycle facts are recorded in `TRANSPORT_DECISION.md`: T2 owns the playback BAR/ring, host allocates capture DMA, BCE carries command/reply and async timestamp paths, and suspend/resume quiesces remote access around PCI/BCE power changes.
- The confirmed AppleUSBVHCI path is control/query-only in the reviewed Windows evidence; no supported Windows PCM mapping/event contract is proven. A successful query is not playback proof.
- Recommended architecture is option B: a Windows audio component with an explicit T2/BCE PCI/resource owner, pending the concrete mapping/event fact named in `TRANSPORT_DECISION.md`.
- The offline inventory tool is `tools/inventory_kait2en_profile.py`; its generated result is in `evidence_kait2en_inventory.txt`.
- `tools/parse_native_probe_report.py` parses the saved textual native probe report into a JSON Speaker contract; it does not parse raw wire bytes. Its checked output is `native_probe_contract.json`.

## Speaker DSP via Equalizer APO (pivot)

- Decision: the custom T2 audio driver was abandoned (repeated BSODs) and the original Apple driver was restored. The Windows endpoint is stereo-only, so the host-side DSP is ported in the stereo domain.
- `SpeakerDSP-APO/` ports the KAIT2EN 16,1 graph to Equalizer APO; `generate_config.py` emits `config.txt`: Preamp -> bankstown virtual bass -> Tone1 (12th-order high-pass) -> XTC (mid/side) -> Tone2 (midrange EQ).
- The nonlinear bankstown virtual bass is a from-scratch VST2 plugin in `SpeakerDSP-APO/vst/` (C++ port of chadmed/bankstown, MIT). Verified against an independent Python reference: max impulse error ~1e-6 and it generates 2nd/3rd harmonics of a 60 Hz tone (`vst/verify.py` -> PASS).
- The 6-channel crossover / per-driver EQ / limiters are performed by Apple firmware on Windows and are not reproduced.
- Equalizer APO 1.3 is installed and attached to the active Speakers endpoint `{4A2CB326-76DA-4445-8E2F-4D1A784D36FB}` as both the Pre-Mix (SFX, `{EACD2258-FCAC-4FF4-B36D-419E924A6D79}`) and Post-Mix (`{EC1CC9CE-FAED-4822-828A-82A81A6F018F}`) classes.
- Deployed: `config.txt` plus `XTC-side-{44k,48k,96k}.wav` in `C:\Program Files\EqualizerAPO\config\`; `bankstown.dll` in `C:\Program Files\EqualizerAPO\VSTPlugins\` (the default plugin path resolved by the loader).
- Verified live: during playback `audiodg.exe` loads `EqualizerAPO.dll`, `libfftw3f-3.dll`, `libsndfile-1.dll` and `bankstown.dll`; the trace log records `Adding VST plugin ...\bankstown.dll` and `Loaded library ...` with no crash message.
- The chain is applied exactly once: with both Pre-Mix and Post-Mix installed, `StageFilterFactory` (`stageMatches = capture || !preMix || !postMixInstalled`) makes the Pre-Mix instance a no-op and the Post-Mix instance applies the full chain, so there is no double processing.
- Debugging aid: the APO writes `EqualizerAPO.log` to `GetTempPathW()` of the audiodg service account, i.e. `C:\Windows\ServiceProfiles\LocalService\AppData\Local\Temp\EqualizerAPO.log`; verbose tracing is enabled via `HKLM\SOFTWARE\EqualizerAPO\EnableTrace = true`.

## Findings that changed the approach

- **XTC caused the "crackle".** Isolating the chain (bankstown off + XTC off, only Tone1+Tone2+Preamp) made the crackle disappear (user-confirmed); re-enabling bankstown alone did not bring it back. The XTC side FIR itself is near-flat (peak +3.75 dB @2 kHz) and adds no bass, so XTC was dropped.
- **bankstown gives only weak bass.** With the graph's parameters it synthesises harmonics ~-20 dB relative to a full-scale 100 Hz tone; for music the harmonics sit ~-46..-64 dB. Combined with Tone1's steep ~75 Hz high-pass it removes the real bass and does not replace it convincingly.
- **The graph's bass actually comes from the 6-channel per-driver EQ** (woofer channels c0/c1/c3/c4: +3.6 dB @30 Hz rolling to -22.6 dB @1 kHz; tweeter channels c2/c5: high-passed), which Apple firmware applies on Windows and we do not reproduce. `kait2en/dsp/README.md` states the graphs "reproduce the model-specific processing that macOS applies".
- **User confirmed the stock Windows sound (no APO) is thin, i.e. the Apple Boot Camp driver provides no useful bass.** So a bass boost is required rather than assumed to be duplicated.

## Speaker APO configuration

### 2026-10-09: bananakid Boot Camp APO preset installed

- Installed the official [apple-t2-audio-controller-boot-camp 1.0.0](https://github.com/bananakid/apple-t2-audio-controller-boot-camp/releases/tag/1.0.0) **FIR/Equalizer APO preset** for `MacBookPro16,1` on the existing AppleAudio driver. Release archive SHA256: `2D9A410EDC8EAD334855B327AAB07A02C57F361BF95CCFC5A77A58425B37059B`. The WAVs and `increase-bass.txt` already existed in the unused `C:\Program Files\Equalizer APO\config` tree and matched the release; the same official files were copied to the actual installed `C:\Program Files\EqualizerAPO\config` tree.
- Active `C:\Program Files\EqualizerAPO\config\config.txt` now has `Device: all`, `Preamp: -16 dB`, `Include: increase-bass.txt`; the latter is the unmodified release preset (woofer/tweeter convolution, stereo downmix). Preamp headroom accounts for measured combined FIR peak ~+15.51 dB. The previous simple-EQ config is backed up at `C:\Program Files\EqualizerAPO\config\config.before-bananakid-20261009_054323.bak` (copy it back to `config.txt` to undo).
- The current active AppleAudio Speakers endpoint is `{b72e5770-3c1d-41de-98e2-43a45eeac87c}` (older ID below is obsolete). Its `FxProperties` includes Equalizer APO SFX/EFX GUIDs. `Benchmark.exe` loaded both release FIR WAVs without errors at 48 kHz/stereo; test sweep output peak 0.85333 (−1.38 dBFS). WASAPI probe initialized and started/stopped that endpoint successfully; PCI `106B:1803` remained `AppleAudio`, `Started/OK`. Auditory evaluation by user is pending.

- Previous simple-EQ configuration: `SpeakerDSP-APO/config_simple.txt`, previously deployed to `C:\Program Files\EqualizerAPO\config\config.txt` (other backups at `config_kait2en.bak` and `config_chain.bak`).
- Filters: `Preamp -10 dB` -> `HP 45 Hz Q 0.707` -> `LS 150 Hz +8 dB Q 0.707` -> `PK 90 Hz +2 dB Q 1.0` -> `PK 300 Hz -2 dB Q 1.2` -> `PK 2500 Hz -2 dB Q 1.0` -> `HS 10000 Hz +2 dB Q 0.707`. `Device: all`.
- Preamp = -(max boost) so the full-scale bass test tone no longer clips: measured loopback peaks white -6.0 dBFS, bass -2.1 dBFS, zero samples >1.0 (previously bass +3.4 / s100 +4.7 dBFS).
- Measured loopback transfer (white noise): bass boosted ~+10..16 dB around 90..150 Hz, treble ~+3..5 dB, midrange dipped. Matches the design.
- Equalizer APO filter syntax confirmed from source (`filters/BiQuadFilterFactory.cpp`): `Filter: ON <PK|LS|HS|HP|LP|BP|NO|AP> Fc <f> [Gain <g>] [Q <q> | BW <bw> | S <slope>]`; for LS/HS gain is required, Q optional (default 0.9); for HP/LP Q optional (default 0.707).
- Alternative not used: the built-in `LoudnessCorrection: State 1 ReferenceLevel 0 ReferenceOffset 0` filter applies a volume-dependent bass boost with matching pre-attenuation (clip-safe), but it is more complex and volume-dependent.

## Superseded notes

- **SUPERSEDED:** historical channel table assignments for channels 4 and 5. The current graph exposes `AUX0..AUX5`, but neither it nor the probe establishes physical speaker order; do not use the old table for new conclusions.
- **SUPERSEDED:** treating a function pointer read at RVA `0xF100` as proof that a vtable begins there. The observation only establishes a pointer/value relationship at that location; vtable base and object identity remain unproven.
- **SUPERSEDED:** old `I=O+8` offset conclusions. The corrected relation is `O+0x90=I+0x88`, `O+0x98=I+0x90`, `O+0xA0=I+0x98`, `O+0xA8=I+0xA0`.
