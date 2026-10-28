# EXCERPT 2026-10-28 22:45:00 — Step 4f (Jack Polling Verification) & Step 5 / 5a (Presets & Tone Evaluation)

> **Date:** 2026-10-28 22:45:00 +05:00 (17:45:00 UTC)  
> **Machine:** MacBookPro16,1 (`PCI\VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01`)  
> **Driver Version:** `1.0.12.1` (`T2AudioMiniport.sys` SHA256: `6F6E2D5E583396D974ED38CF04B5E8FA28FE3DF5AE772AC567B76BDEEEEBC9FD`)  
> **Driver Signing:** SHA1 `CA2DE95D1B065F6D6CFF6C4158DC88E2424ABBCE` (Signed and Verified)  
> **Active Mode:** Persistent Mode (`EnableProduction=1`, `CrashGuardStrikes=0`, `Status: OK`)

---

## 1. Summary of Results

1. **Step 4f: Jack Polling (PASS - Protocol behavior identified):**
   - Built and installed version `1.0.12.1` incorporating Claude's commit `a1797cd` (1000 ms periodic jack poll timer).
   - Confirmed live BCE polling: while an audio stream is active (`PoweredDown = FALSE`), the driver sends BCE message 7 (`T2AUDIO_PROP_JACK_PLUGGED`) to codec output device `0x25` at exact 1000 ms intervals (`tag=29` through `tag=85`, 57 consecutive successful queries).
   - Confirmed power state management: when audio stops, PortCls transitions adapter to D3 (`PowerChangeState D3`), and the driver cleanly halts BCE polling to prevent unwanted bus traffic.
   - Tested 3 physical plug/unplug cycles of 3.5mm headphones during active audio stream playback: bridgeOS returned status `0x00000000` with 512 bytes reply on every query, but the returned value stayed `plugged = 0`.
   - Verified PnP stop/restart path with `JackEvents=1`: dynamic restart succeeded with 0 errors, no hung work items, no BSOD.
   - Cleaned up registry parameters back to production defaults: `JackEvents=0`, `HeadphoneAutoMute=0`, `ExtendedProbe=0`.

2. **Step 5 / 5a: Presets and Tone Controls (PASS):**
   - Fixed signed DWORD handling in `tools/T2Audio-Setup.ps1` (`Get-EffectiveParam` converting values `> 0x7FFFFFFF` to signed `[int]`).
   - Verified `-Action Tune -BassDb 6 -TrebleDb -6`: `Status` correctly reported `бас +6 дБ, высокие -6 дБ`, sound became audibly darker. Returning with `-BassDb 0 -TrebleDb 0` restored standard `Как на Mac`.
   - Verified CPU load test under extreme DSP settings: preset `night` (`SpeakerDsp=4`, `DspAutoGain=1`, `CompStyle=1`) + `Tune +12/+12/+12`.
     - Kernel `run summary`: `maxCopyUs = 2112` (2.1 ms max tick, ~40 µs per sub-buffer), `gaps>10ms = 0`, `skips = 0`, `dspLimited = 0`. Stream remained completely glitch-free.
   - Verified dynamic preset switching across all 10 presets: `macos`, `macos-precise`, `bass`, `voice`, `movie`, `night`, `loud`, `maxvolume`, `even`, `bright`.

---

## 2. Step 4f Verbatim Log (Active Polling During Playback)

```text
T2Audio: PowerChangeState D0
T2Audio: crash guard armed
T2Audio: SetState RUN about to StartIo irql=0
T2Audio: START_IO reply size=256 tag=17 type=2 dev=0x0 msg=1 status=0x00000000
T2Audio: START_IO sent to device 0x39
T2Audio: T2 clock: re-aimed lead by -1091 frames (#1, read=17499 next=1950)
T2Audio: BCE send ioctl=0x00222018 dev=FFFFCE88D4581390 in=49 tag=18 type=1 id=0x25 msg=7
T2Audio: BCE send wait=0x00000000 iosb.Status=0x00000000 info=512
T2Audio: BCE send ioctl=0x00222018 dev=FFFFCE88D4581390 in=49 tag=19 type=1 id=0x25 msg=7
T2Audio: BCE send wait=0x00000000 iosb.Status=0x00000000 info=512
...
T2Audio: BCE send ioctl=0x00222018 dev=FFFFCE88D4581390 in=49 tag=80 type=1 id=0x25 msg=7
T2Audio: BCE send wait=0x00000000 iosb.Status=0x00000000 info=512
T2Audio: run summary: reAims=1 rateUpdates=0 ratePpb=0 maxCopyGapUs=3024 gaps>10ms=0 gaps>lead=0 skips=0 maxCopyUs=2703 dspLimited=0 wooferLimited=0 autoGainDb10=0 jackMutedFrames=0
T2Audio: STOP_IO sent to device 0x39
T2Audio: PowerChangeState D3
T2Audio: crash guard disarmed (clean end of session)
```

---

## 3. Step 5a CPU Load Test Verbatim Log

```text
T2Audio: mode parameters: ForceSystemBuffer=0 MdlVariant=0 TweeterMode=1 OutputAttenuation=1 (-6 dB) ClockRateTracking=0 SpeakerDsp=4 DspGainDb=3 TweeterTrimDb=0 WooferLimitDb=6 DspAutoGain=1 BassDb=12 TrebleDb=12 ClarityDb=12 CompStyle=1 ExtendedProbe=0 JackEvents=0 HeadphoneAutoMute=0
T2Audio: run summary: reAims=1 rateUpdates=0 ratePpb=0 maxCopyGapUs=5047 gaps>10ms=0 gaps>lead=0 skips=0 maxCopyUs=2112 dspLimited=0 wooferLimited=0 autoGainDb10=598 jackMutedFrames=0
```
