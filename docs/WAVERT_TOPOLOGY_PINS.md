# WaveRT / Topology Pin Scheme and Physical Connection

**Last Updated**: 2026-10-07
**Status**: Endpoint structure implemented (bridge pins + physical connection). Hardware playback still disabled.
**Branch**: diagnostics

This document records the pin, node, and connection descriptors in
`src/WaveRTMiniport.c` and `src/Topology.c`, and the physical connection wired in
`src/Driver.c`. The layout follows the Microsoft `sysvad` WaveRT + topology model.

---

## 1. WaveRT filter (`src/WaveRTMiniport.c`)

Two-pin filter. Descriptor `g_T2AudioFilterDescriptor`.

| Pin | DataFlow | Communication | Category | Data ranges |
|-----|----------|---------------|----------|-------------|
| 0 (`T2AUDIO_WAVE_PIN_RENDER_SINK`) | `KSPIN_DATAFLOW_IN` | `KSPIN_COMMUNICATION_SINK` | `KSCATEGORY_AUDIO` | `g_T2AudioSpeakerRange` |
| 1 (`T2AUDIO_WAVE_PIN_BRIDGE`) | `KSPIN_DATAFLOW_OUT` | `KSPIN_COMMUNICATION_NONE` | `KSCATEGORY_AUDIO` | `g_T2AudioBridgeRange` |

- Pin 0 is the render streaming pin the audio engine opens. Instance counts `Max=1, MaxFilter=1, Min=0`.
- Pin 1 is the bridge pin. Instance counts `0, 0, 0` (bridge pins are never instantiated as streams).
- Node count: 0. Connections: `{ PCFILTER_NODE, 0, PCFILTER_NODE, 1 }` (streaming pin -> bridge pin).
- Streaming data range (`KSDATARANGE_AUDIO`): `TYPE_AUDIO` / `SUBTYPE_PCM` / `SPECIFIER_WAVEFORMATEX`,
  6 channels, 32 bits/sample, container 32, valid bits 32, 48000..48000 Hz.
- Bridge data range (`KSDATARANGE`): `TYPE_AUDIO` / `SUBTYPE_ANALOG` / `SPECIFIER_NONE`.
- `DataRangeIntersection` answers pin 0 with a `KSDATAFORMAT_WAVEFORMATEX`
  (`WAVE_FORMAT_PCM`, 6 ch, 48000 Hz, 32-bit, `nBlockAlign=24`, `nAvgBytesPerSec=48000*24`)
  and pin 1 with a `KSDATAFORMAT` (`TYPE_AUDIO` / `SUBTYPE_ANALOG` / `SPECIFIER_NONE`).
- `NewStream` accepts only pin 0 (`T2AUDIO_WAVE_PIN_RENDER_SINK`) and rejects capture.

## 2. Topology filter (`src/Topology.c`)

Two-pin, no-node filter. Descriptor `g_T2AudioTopologyFilterDescriptor`.

| Pin | DataFlow | Communication | Category | Data ranges |
|-----|----------|---------------|----------|-------------|
| 0 (`T2AUDIO_TOPO_PIN_BRIDGE`) | `KSPIN_DATAFLOW_IN` | `KSPIN_COMMUNICATION_NONE` | `KSCATEGORY_AUDIO` | `g_TopologyBridgePinDataRange` |
| 1 (`T2AUDIO_TOPO_PIN_SPEAKER`) | `KSPIN_DATAFLOW_OUT` | `KSPIN_COMMUNICATION_NONE` | `KSNODETYPE_SPEAKER` | `g_TopologySpeakerPinDataRange` |

- Both pins use instance counts `0, 0, 0`.
- Node count: 0. Connections: `{ PCFILTER_NODE, 0, PCFILTER_NODE, 1 }` (bridge pin -> speaker pin).
- Both topology data ranges use `TYPE_AUDIO` / `SUBTYPE_ANALOG` / `SPECIFIER_NONE`.
- `DataRangeIntersection` returns `STATUS_NOT_IMPLEMENTED` (topology exposes no negotiable format).

This matches the `sysvad` speaker topology convention: the bridge pin carries
`KSCATEGORY_AUDIO`, and the physical speaker connector pin carries
`KSNODETYPE_SPEAKER` (no intermediate node).

## 3. Physical connection (`src/Driver.c`)

`T2AudioStartDevice` registers both subdevices and then wires their bridge pins:

```c
PcRegisterPhysicalConnection(DeviceObject,
                             (PUNKNOWN)port,         T2AUDIO_WAVE_PIN_BRIDGE,   // From: Wave bridge
                             (PUNKNOWN)topologyPort, T2AUDIO_TOPO_PIN_BRIDGE);  // To:   Topology bridge
```

- Registered after `PcRegisterSubdevice(..., L"Topology", ...)` and `PcRegisterSubdevice(..., L"Wave", ...)`.
- Both port objects are kept referenced until the connection is registered, then released.
- On any WaveRT-path failure the topology port is also released (no leak).

This is the step that lets PortCls build the render endpoint
(Wave streaming pin -> Wave bridge -> Topology bridge -> speaker connector).

## 4. Scope note

The endpoint **structure** is now complete and the WaveRT buffer contract is
implemented. Audio **playback** remains disabled by design: while
`SpeakerDeviceId == 0` (BCE transport not implemented) `AllocateAudioBuffer` serves a
host-side system-memory buffer instead of device memory, and `StartIo`/`StopIo` still
return `STATUS_NOT_SUPPORTED`. See `docs/CURRENT_STATE.md`.
