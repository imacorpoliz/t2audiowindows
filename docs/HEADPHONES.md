# Headphones (3.5 mm jack): Architecture & Implementation

## How it works (Apple T2 & KAIT2EN `modules/t2bce_audio`)

* Headphones are a **separate BCE device** with UID `"Codec Output"` (`0x25` on MacBookPro16,1), with its own output stream (`0x26`, 2-channel 48 kHz 24-bit in 32-bit slots), its own ring in BufferStruct at BAR1 `0x190000` (16,640 frames), and its own START_IO/STOP_IO.
* The jack state is a hardware property: `scope 'outp'`, `selector 'jack'` (`0x6A61636B`, u32: 1 = plugged, 0 = unplugged).
* BridgeOS notifies the host of jack plug/unplug events asynchronously via BCE Message 13 (`PROPERTY_CHANGED`). The driver registers a listener using Message 11 (`ADD_PROPERTY_LISTENER`) during device start and polls every 500 ms as backup.

## Verified Implementation Status (✅ Hardware Verified)

1. **Hardware Presence Detection:**
   - Topology pin exposes `KSPROPERTY_JACK_DESCRIPTION` and `KSPROPERTY_JACK_DESCRIPTION2` with `JACKDESC2_PRESENCE_DETECT_CAPABILITY`.
   - On plug/unplug transitions, `T2AudioNotifyJackChange` signals `KSEVENT_PINCAPS_JACKINFOCHANGE` to Windows AudioEndpointBuilder.
   - Windows automatically routes system audio to Headphones when plugged in, and returns to Speakers when unplugged.
2. **Headphone DSP:**
   - Integrated **Bauer bs2b** (Stereophonic-to-Binaural) crossfeed DSP (`HeadphoneCrossfeed=1`) running in 64-bit Q16 fixed point to eliminate listener fatigue.
   - Built-in −12 dB safety attenuation cut (`HeadphoneAttenuation=2`).
   - Automatic speaker silencing while plugged in (`HeadphoneAutoMute=1`).
3. **Endpoint Separation:**
   - Dedicated PortCls WaveRT filter and topology filter pair registered as "T2 Audio Headphones".
   - Completely independent ring buffer copy and timestamp tracking.
