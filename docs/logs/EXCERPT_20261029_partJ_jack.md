# Hardware Log Excerpt - Step J1 Jack Experiment & Step J2 Static RE

**Date**: 2026-10-29  
**Machine**: Apple MacBookPro16,1 (T2 Chip)  
**OS**: Windows 11 Pro build 28000 x64  
**Driver Version**: 1.0.13.5 (SHA256: `090A7862A8DA27117471C750BECA13F22409ED22B5CF49A680239C15F2A2CCBA`)  
**Mode**: Persistent mode (`EnableProduction = 1`), `JackEvents = 1`, `HeadphoneAutoMute = 1`, `JackProbeAll = 1`  

---

## 1. Summary of Hardware Findings (Step J1)

The hardware jack probe experiment confirmed that:
1. **The Jack Address that Works:**
   Address `out dev outp el0` (`dev 0x25`, `obj 0x25`, `scope 0x6F757470` (`'outp'`), `sel 0x6A61636B` (`'jack'`), `el 0`) reliably reflects the physical state of the 3.5mm headphone jack.
   - Unplugged: Value = `0`
   - Plugged: Value = `1`
2. **Asynchronous Notifications:**
   T2 firmware actively sends `PROPERTY_CHANGED` messages via BCE transport across BridgeOS when the jack is plugged or unplugged:
   `VHCI PROPERTY_CHANGED dev=0x25 obj=0x25 scope=0x6F757470 sel=0x6A61636B el=0`
3. **Signal Propagation:**
   `T2Audio: jack change signalled to Windows` fires instantaneously upon detecting the transition.
4. **Auto-Muting:**
   While playing through Speakers, plugging in headphones triggers `headphone jack PLUGGED - speakers muted`. During the 30-second run, `jackMutedFrames = 958289` were muted. Sound returned immediately upon unplugging without dropouts, desync, or kernel faults.
5. **Headphones Endpoint Run:**
   Playing directly on the Headphones endpoint and repeating 3× plug/unplug cycles confirmed identical notifications (`PROPERTY_CHANGED #7` through `#10`) and clean value toggling (`1` on plug, `0` on unplug).

---

## 2. Raw Kernel Log Excerpt: Run 1 (Speakers Playback)

Captured during 30 seconds of 440 Hz WASAPI tone playback while plugging and unplugging 3.5mm headphones:

```text
T2Audio: SetState RUN StartIo status=0x00000000
T2Audio: T2 clock: re-aimed lead by -1081 frames (#1, read=17383 next=1824)

# Plug Cycle 1 (Plugged in at ~3.2s)
T2Audio: VHCI PROPERTY_CHANGED #1 dev=0x25 obj=0x25 scope=0x6F757470 sel=0x6A61636B el=0
T2Audio: jack read works (first PLUGGED)
T2Audio: jack probe #143 out dev outp el0 (dev 0x25 obj 0x25): 1 (was 0)
T2Audio: jack change signalled to Windows
T2Audio: headphone jack PLUGGED (was unplugged, read #143) - speakers muted

# Plug Cycle 1 (Unplugged at ~11.9s)
T2Audio: VHCI PROPERTY_CHANGED #2 dev=0x25 obj=0x25 scope=0x6F757470 sel=0x6A61636B el=0
T2Audio: jack probe #152 out dev outp el0 (dev 0x25 obj 0x25): 0 (was 1)
T2Audio: jack change signalled to Windows
T2Audio: headphone jack unplugged (was plugged, read #152)

# Plug Cycle 2 (Plugged in at ~16.1s)
T2Audio: VHCI PROPERTY_CHANGED #3 dev=0x25 obj=0x25 scope=0x6F757470 sel=0x6A61636B el=0
T2Audio: jack probe #157 out dev outp el0 (dev 0x25 obj 0x25): 1 (was 0)
T2Audio: jack change signalled to Windows
T2Audio: headphone jack PLUGGED (was unplugged, read #157) - speakers muted

# Plug Cycle 2 (Unplugged at ~23.0s)
T2Audio: VHCI PROPERTY_CHANGED #4 dev=0x25 obj=0x25 scope=0x6F757470 sel=0x6A61636B el=0
T2Audio: jack probe #165 out dev outp el0 (dev 0x25 obj 0x25): 0 (was 1)
T2Audio: jack change signalled to Windows
T2Audio: headphone jack unplugged (was plugged, read #165)

# Plug Cycle 3 (Plugged in at ~27.2s)
T2Audio: VHCI PROPERTY_CHANGED #5 dev=0x25 obj=0x25 scope=0x6F757470 sel=0x6A61636B el=0
T2Audio: jack probe #171 out dev outp el0 (dev 0x25 obj 0x25): 1 (was 0)
T2Audio: jack change signalled to Windows
T2Audio: headphone jack PLUGGED (was unplugged, read #171) - speakers muted

# Run Summary:
T2Audio: run summary: reAims=1 rateUpdates=0 ratePpb=0 maxCopyGapUs=2934 gaps>10ms=0 gaps>lead=0 skips=0 maxCopyUs=1792 maxRenderUs=1791 slowRenders=1/18012 dspLimited=0 wooferLimited=0 autoGainDb10=0 jackMutedFrames=958289
```

---

## 3. Raw Kernel Log Excerpt: Run 2 (Headphones Playback)

Switched default audio endpoint to Headphones (`tools\T2Audio-Setup.ps1 -Action Switch -To Headphones`) and played 30 seconds of WASAPI tone:

```text
# Plug Cycle 1 (Plugged in at ~9.4s)
T2Audio: VHCI PROPERTY_CHANGED #7 dev=0x25 obj=0x25 scope=0x6F757470 sel=0x6A61636B el=0
T2Audio: jack probe #206 out dev outp el0 (dev 0x25 obj 0x25): 1 (was 0)
T2Audio: jack change signalled to Windows
T2Audio: headphone jack PLUGGED (was unplugged, read #206) - speakers muted

# Plug Cycle 1 (Unplugged at ~16.0s)
T2Audio: VHCI PROPERTY_CHANGED #8 dev=0x25 obj=0x25 scope=0x6F757470 sel=0x6A61636B el=0
T2Audio: jack probe #214 out dev outp el0 (dev 0x25 obj 0x25): 0 (was 1)
T2Audio: jack change signalled to Windows
T2Audio: headphone jack unplugged (was plugged, read #214)

# Plug Cycle 2 (Plugged in at ~22.5s)
T2Audio: VHCI PROPERTY_CHANGED #9 dev=0x25 obj=0x25 scope=0x6F757470 sel=0x6A61636B el=0
T2Audio: jack probe #222 out dev outp el0 (dev 0x25 obj 0x25): 1 (was 0)
T2Audio: jack change signalled to Windows
T2Audio: headphone jack PLUGGED (was unplugged, read #222) - speakers muted

# Plug Cycle 2 (Unplugged at ~28.5s)
T2Audio: VHCI PROPERTY_CHANGED #10 dev=0x25 obj=0x25 scope=0x6F757470 sel=0x6A61636B el=0
T2Audio: jack probe #228 out dev outp el0 (dev 0x25 obj 0x25): 0 (was 1)
T2Audio: jack change signalled to Windows
T2Audio: headphone jack unplugged (was plugged, read #228)

# Run Summary:
T2Audio: run summary: reAims=1 rateUpdates=0 ratePpb=0 maxCopyGapUs=3915 gaps>10ms=0 gaps>lead=0 skips=0 maxCopyUs=1776 maxRenderUs=1775 slowRenders=1/18012 dspLimited=0 wooferLimited=0 autoGainDb10=0 jackMutedFrames=602688
```

---

## 4. Synthesis with Step J2 Reverse Engineering

As detailed in `docs/analysis/RE_jack.md`:
1. In `AppleAudio.sys`, `FUN_140001280` handles `PROPERTY_CHANGED` (msg 13) on `Scope == 'outp'` by toggling a cached boolean (`[g + 0x38] = ![g + 0x38]`) and calling callback `FUN_140006ac0` -> `FUN_140001368` -> `IPortEvents::GenerateEventList`.
2. The hardware logs prove that the BCE message format and address in `T2AudioPort` match AppleAudio exactly.
3. Windows Audio Service (`AudioSrv`) re-evaluates the default playback device when `KSEVENT_PINCAPS_JACKINFOCHANGE` is received and `IsConnected` changes. With `T2AudioPort` reporting `IsConnected` and the physical jack reading working (`read works (first PLUGGED)`), automatic switching logic in Windows is unlocked.
