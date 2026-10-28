# RE: the T2 clock (R6) and the speaker layout (R7)

Static analysis, 2026-10-28. The binaries are the same as in [`RE_appleaudio_protocol.md`](RE_appleaudio_protocol.md) (VAs with ImageBase `0x140000000`), plus the kaiT2en DSP profile `dsp/profiles/16_1/graph.json` from the private `imacorpoliz/mbp` (`upstream-kait2en`). Labels: **proven**, **probable**, **hypothesis**.

## 1. Where the time in `UPDATE_TIMESTAMP` comes from — **proven**

AppleUSBVHCI.sys, `0x140002FAC`–`0x140002FCC` and `0x140003138`:

```
KeQueryPerformanceCounter(&freq)
rax = qpc * 10000000 / freq        ; 100 ns units
rdx = rax * 100                    ; nanoseconds
0x140003138: write32(BAR+0xC008, low32(ns)); write32(BAR+0xC000, high32(ns))
```

The VHCI publishes the host time (QPC in nanoseconds) for the T2 in the BCE registers `+0xC000`/`+0xC008`. This is the counterpart of `timestamp.c` in apple-bce on Linux. bridgeOS converts its hardware time into that scale (in kaiT2en: `mach_bridge_remote_time()`), so the `hostTime` in `UPDATE_TIMESTAMP` is **host QPC nanoseconds**.

An important detail: `imul rax, rax, 0x989680` is a 64-bit product `qpc * 10^7`. At a 10 MHz QPC it overflows after about 2^63 / 10^14 ≈ 92 000 s, i.e. **~25.6 hours of uptime**. After that `hostTime` is no longer in our scale (**hypothesis** at the time; AppleAudio on Windows presumably behaves the same; confirmed on hardware in §8). That is why the driver does not trust `hostTime` blindly (see §3).

The VHCI delivers the message unchanged: `0x140002274` copies it into a work item and does not recompute the time.

## 2. AppleAudio's clock model (R6) — **proven**

Storing (`0x14000435C`, called from the `0x222020` handler for `UPDATE_TIMESTAMP`): the device is looked up by `device_id` (array `[ctx+0x48]`, stride `0xE970`), then:

```
inc   dword [dev+0xE954]          ; timestamp counter
mov   rax, [msg+0x15]             ; first u64 of the payload = hostTime
mov   [dev+0xE958], rax
```

AppleAudio **does not read** `updateSeed` and `sampleTime`. The counter is reset on START_IO/STOP_IO (`0x1400023C8`, `0x140004A3E`).

The write position (`0x140009A24`; called from the copy loop `0x14000B3A7` under `KfRaiseIrql(DISPATCH)`; the mirror variant for recording is `0x140009BD0`):

```
src     = [s+0xE8] / bytesPerClientFrame + [s+0xD0]     ; client frames + offset d0
if [dev+0xE954] != [s+0x2A8]:                            ; a new timestamp arrived
    elapsed = (QPC_ms(now) - hostTime/1e6) * (rate/1000) ; frames since the timestamp
    hw      = elapsed + count * ringBytes / ringFrameBytes ; count * ringFrames
    lead    = src - hw
    if lead outside [500, 1500]:  d0 += 1000 - lead        ; re-aim
    [s+0x2A8] = count
return (src * ringFrameBytes) % ringBytes                 ; write offset in the ring
```

* `[s+0xA0]` is the ring size in bytes, `[s+0xA8]` its address (BAR), `[dev+0x1A4]` the rate.
* A timestamp arrives **once per ring revolution** (`count * ringFrames`); kaiT2en describes the same with `sampleTime`, which grows by a fixed `timestampPeriod`.
* **Writes are kept 1000 frames (~21 ms) ahead of the T2 read position; when the lead leaves ±500, the write position jumps back to 1000.** The client cursor runs on QPC, as in our driver.

## 3. What the driver does for R6

* `T2AudioHandleVhciMessage` (`src/BceTransport.c`): for the speaker, the anchor `ClockAnchorFrames` / `ClockAnchorQpc` is stored under `ClockLock`. It is `sampleTime` (as in kaiT2en) shifted by the delivery delay `now_ns − hostTime` if that is within 0…50 ms; otherwise the timestamp is taken as current. Each new timestamp increments `ClockGeneration`. A wrapped `hostTime` is unwrapped (`T2AudioClockUnwrapHostTime`).
* Copying (`T2AudioStreamFollowT2ClockLocked`, `src/WaveRTStream.c`): source frame N is written to ring frame `N + DestOffset`. On every new timestamp the T2 read position is extrapolated by QPC, and if the lead left the 1000 ± 500 window (`T2AudioClockCorrection`, `src/T2AudioSpeakerLogic.h`), `DestOffset` is re-aimed. Slots skipped by a forward jump are zeroed.
* Until the first timestamp the driver assumes that after START_IO the T2 reads from slot 0, with a lead of 1000 (it used to be 720 without correction).

Without this QPC and the T2 crystal drift apart. At a 50 ppm difference a 720-frame lead would vanish after about 5 minutes, and the T2 would read the previous revolution (an echo 347 ms late).

## 4. Speaker layout of the MacBookPro16,1 (R7) — **probable** (from the kaiT2en DSP)

`graph.json`: the output has 6 channels `AUX0..AUX5` in ring order (`playback.props.audio.position`). Tracing the graph from the outputs to the inputs, and the response of the `bq_raw` chains at 48 kHz:

| Ring channel | Source | Role | 100 Hz | 500 Hz | 1 kHz | 5 kHz | 10 kHz |
|---|---|---|---|---|---|---|---|
| 0 | left (`XTC_l`) | woofer L, pair A | +1 | −22 | −24 | −84 | −121 |
| 1 | left | woofer L, pair B | +1 | −22 | −24 | −84 | −121 |
| 2 | left | **tweeter L** | −200 | −92 | −46 | −14 | −14 |
| 3 | right (`XTC_r`) | woofer R, pair A | +1 | −22 | −24 | −84 | −121 |
| 4 | right | woofer R, pair B | +1 | −22 | −24 | −84 | −121 |
| 5 | right | **tweeter R** | −200 | −92 | −46 | −14 | −14 |

(dB, biquads only; on top of that, compressors, limiters, XTC and the virtual bass.) The woofer pairs have no negative polarity (every `b0 > 0`).

Consistent with AppleAudio: its copy loop (`0x14000A715`–`0x14000B2C0`) runs each channel through its own filter set, selected by model (`0x140014688`). The channel numbers in the Windows driver are not statically confirmed, hence **probable**. (Listening on hardware is consistent with this layout: bass from the woofer channels, top from 2/5.)

Conclusions:

1. **The tweeters must never get the full range**: Apple has −90 dB at 500 Hz there.
2. Windows does not upmix stereo to 5.1. With the former 5.1 endpoint (`FL FR FC LFE SL SR` → channels 0…5) stereo would play only on the **two left woofers**, and `FC`/`LFE` would go to the left tweeter and a right woofer.

## 5. What the driver does for R7

* The endpoint is **stereo** (48 kHz, 32 bit, `KSAUDIO_SPEAKER_STEREO`). The plain copy path (`T2AudioRenderStereoToSpeakers`):
  * L → channels 0, 1 as is, and into 2 through a filter;
  * R → channels 3, 4 as is, and into 5 through a filter.
* Tweeter filter: 4th-order Butterworth HPF at 3 kHz (two RBJ biquads, integer Q30, no FPU at DISPATCH). Measured by a test: 250 Hz −87 dB, 1 kHz −39 dB, 3 kHz −3 dB, 10 kHz 0 dB. Gentler than Apple's, so **the first probes were at low volume**.
* `Parameters\TweeterMode = 0` silences the tweeters (woofers only).
* `TestSession\RawSpeakerChannels = 1` (agent: `-RawChannels`) opens the 6 raw channels to feed each speaker separately. **The tweeters are then unprotected.**
* Apple's equalisation, compressors and XTC: since then reproduced by the in-driver DSP (`SpeakerDsp ≥ 1`, `DRIVER_GUIDE.md` §8.3).

## 6. Open

* Confirm the channel order on hardware (a quiet tone per channel through `-RawChannels`, only with the owner's permission).
* Whether `sampleTime` grows from zero after START_IO (check: the first `UPDATE_TIMESTAMP` lines in the log, `VHCI UPDATE_TIMESTAMP #n … sample=`). (Answered in §8: the first one is 16 640.)
* The `UPDATE_TIMESTAMP` rate (expected once per 16 640 frames ≈ 347 ms). (Confirmed in §8.)

## 7. Comparison with the latest kaiT2en (2026-10-21/22)

Upstream `kaiT2en/KaiT2en-Fedora`, commits `bbf3878`, `beed204`, `6128f86` in `modules/t2bce_audio`. The author reconstructed the behaviour of `BASPDevice` (BridgeAudioSP on macOS):

| kaiT2en observation | For us |
|---|---|
| `UPDATE_TIMESTAMP` is a "zero timestamp" once per ring revolution; position = `sampleTime` + the time since `hostTime` at the nominal rate, but not past the next timestamp | matches AppleAudio's model (§2); we do the same |
| Ring start = the revolution of the first timestamp: `sample_base = sampleTime − sampleTime mod ring` | confirms that `sampleTime mod ring` is the read slot |
| `hostTime` usually lags the reception time by 1–3 ms; after wake it can be far off, and then the reception time is used | our window is 0…50 ms (theirs is one ring revolution, ≈347 ms); outside it, the reception time |
| The first timestamp arrives ~30 ms after the device starts reading; before it the position is counted from a moment before START_IO | same for us: until the first timestamp the T2 is assumed to have started from slot 0 at START_IO |
| After a pause longer than a revolution, a timestamp of the **previous** run may arrive during START_IO; timestamps with `hostTime` before START_IO are discarded | **ported**: `ClockAnchorNs` < the stream's `RunStartNs` → the timestamp is ignored |
| `seed = 1` and `sampleTime = 0` reset the scale; `sampleTime` going backwards is a scale glitch | no special handling needed: the correction works modulo the ring and re-aims by itself |
| If the writer did not advance between two timestamps, the ring is zeroed (otherwise the T2 loops old samples) | not ported: our copy runs on QPC, and the engine writes silence itself. Revisit if "stuck" sound is heard on hardware |
| `'saft'` is read for both directions | we read the output `'saft'` |

The output layout in `dsp/profiles/16_1/graph.json` has not changed since.

## 8. Hardware check (Stage D, 2026-10-28, `docs/logs/EXCERPT_20261028_041655.md`)

| Prediction | Result |
|---|---|
| One timestamp per ring revolution | **confirmed**: `sample` grows by exactly 16 640, interval 346.66 ms (±7 µs) |
| `sampleTime mod ring` is the read slot, the first timestamp shortly after START_IO | first timestamp `sample=16640` ~40 ms after START_IO was sent (≡ slot 0) |
| `hostTime` is corrupted after ~25.6 h of uptime by the `qpc*1e7` overflow in the VHCI | **confirmed**: `t2 = 2^64 − 4.245·10^13` (negative). This matches an uptime of ≈ 1.8447·10^14 − 4.245·10^13 ns ≈ 39.4 h. The driver uses the reception time (later: unwraps it) |
| Lead correction | not verified in this run: the stream did not enter RUN because the START_IO reply was rejected (fixed; verified in the next runs, see `CURRENT_STATE.md`) |
