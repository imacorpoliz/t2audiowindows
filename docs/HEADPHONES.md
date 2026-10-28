# Headphones (3.5 mm jack): plan

## How it works (KAIT2EN `modules/t2bce_audio`)

* Headphones are a **separate BCE device** with UID `"Codec Output"` (the speakers are `"Speaker"`), with their own output stream, their own ring in BufferStruct (`audio.c`: table `{"Speaker", 0}`, `{"Codec Output", 2}`) and their own START_IO/STOP_IO.
* The jack state is a device property, `scope 'outp'`, `selector 'jack'` (u32, 1 = plugged in), read with `GET_PROPERTY`.
* KAIT2EN learns about changes by subscribing with `PROPERTY_LISTENER` (msg 11) to `'outp'/'jack'`; bridgeOS sends `PROPERTY_CHANGED` (msg 13), and the property is read again.

## Steps

1. **Done (2026-10-28, not tested on hardware):** during the BCE probe the driver finds `"Codec Output"` and logs
   `BCE probe: headphone jack device 0x… : PLUGGED/unplugged`. This will confirm the UID, the device id and the property read.
2. **Mute the speakers while headphones are plugged in.** Subscribe with `PROPERTY_LISTENER`, handle `PROPERTY_CHANGED` in the `0x222020` handler (`T2AudioHandleVhciMessage`), and write silence into the speaker ring while plugged in. Logic only, no new audio path.
3. **A second audio endpoint, "Headphones".** A second WaveRT filter in PortCls, the `"Codec Output"` ring from BufferStruct (generalise `T2AudioFindSpeakerBuffer`), its `'pft '` format, START_IO/STOP_IO for its id, its own clock (`UPDATE_TIMESTAMP` by `dev`), without the speaker DSP. Plus `KSPROPERTY_JACK_DESCRIPTION`, so that Windows shows "plugged in / unplugged".
4. Microphones work the same way, as an input stream (`set_input_stream_address_ranges` in KAIT2EN).

Steps 2–4 only after step 1 has shown real data from the hardware.
