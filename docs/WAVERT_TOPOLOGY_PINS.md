# WaveRT / Topology Pin Scheme (Documentation Only)

**Last Updated**: 2026-10-06
**Status**: Describes the *current* static pin/connection layout. No code changes accompany this document.
**Branch**: diagnostics

This document records the pin, node, and connection descriptors as implemented in
`src/WaveRTMiniport.c` and `src/Topology.c`. It is a description of the existing
diagnostic-mode driver, **not** a design proposal or a change request.

---

## 1. WaveRT filter (`src/WaveRTMiniport.c`)

Single-pin filter. Descriptor `g_T2AudioFilterDescriptor` (WaveRTMiniport.c:121).

| Pin | DataFlow | Communication | Category | Node type | Data ranges |
|-----|----------|---------------|----------|-----------|-------------|
| 0 | `KSPIN_DATAFLOW_IN` | `KSPIN_COMMUNICATION_SINK` | `KSCATEGORY_AUDIO` (`g_T2AudioCategoryAudio`) | `KSNODETYPE_SPEAKER` (`g_T2AudioNodeSpeaker`) | `g_T2AudioSpeakerRange` |

- Pin instance counts: `MaxGlobal=1, MaxFilter=1, Min=1` (WaveRTMiniport.c:100-103).
- Node count: 0; connection count: 0 (WaveRTMiniport.c:127-133).
- Data range (`KSDATARANGE_AUDIO`, WaveRTMiniport.c:78-93):
  - MajorFormat `KSDATAFORMAT_TYPE_AUDIO`
  - SubFormat `KSDATAFORMAT_SUBTYPE_PCM`
  - Specifier `KSDATAFORMAT_SPECIFIER_WAVEFORMATEX`
  - Channels 6, bits/sample 32, container 32, valid bits (min/max) 32, sample rate 48000..48000.
- `DataRangeIntersection` (WaveRTMiniport.c:149) only answers `PinId == 0`; it returns a
  `KSDATAFORMAT_WAVEFORMATEX` with `WAVE_FORMAT_PCM`, 6 ch, 48000 Hz, 32-bit,
  `nBlockAlign=24`, `nAvgBytesPerSec=48000*24` (WaveRTMiniport.c:175-187).

## 2. Topology filter (`src/Topology.c`)

Two-pin, one-node filter. Descriptor `g_T2AudioTopologyFilterDescriptor` (Topology.c:212).

| Pin | DataFlow | Communication | Category | Node type | Data ranges |
|-----|----------|---------------|----------|-----------|-------------|
| 0 (bridge, input from WaveRT) | `KSPIN_DATAFLOW_IN` | `KSPIN_COMMUNICATION_NONE` | `NULL` (none) | `NULL` (none) | `g_TopologyBridgePinDataRange` |
| 1 (speaker output) | `KSPIN_DATAFLOW_OUT` | `KSPIN_COMMUNICATION_NONE` | `KSCATEGORY_AUDIO` | `KSNODETYPE_SPEAKER` | `g_TopologySpeakerPinDataRange` |

- Pin instance counts: `MaxGlobal=1, MaxFilter=1, Min=0` (Topology.c:166, 181).
- Node 0: `KSNODETYPE_SPEAKER` (Topology.c:197-204).
- Connections (Topology.c:207-210):
  - `{ PCFILTER_NODE, 0, 0, 0 }` — filter Pin 0 -> Node 0 input
  - `{ 0, 0, PCFILTER_NODE, 1 }` — Node 0 output -> filter Pin 1
- Both topology data ranges use MajorFormat `KSDATAFORMAT_TYPE_AUDIO`,
  SubFormat `KSDATAFORMAT_SUBTYPE_ANALOG`, Specifier `KSDATAFORMAT_SPECIFIER_NONE`
  (Topology.c:15-26, 33-44).
- `DataRangeIntersection` always returns `STATUS_NOT_IMPLEMENTED` (Topology.c:112-133);
  topology exposes no negotiable format.

## 3. Format roles (why the two subtypes differ)

The WaveRT pin advertises **PCM/WAVEFORMATEX** because it is the streaming pin that
KS consumes (render stream). The Topology pins advertise **ANALOG/NONE** because they
model the internal bridge and the physical speaker endpoint, not a KS data stream.

This difference is **by role and is expected** — it is not a mismatch defect. The
bridge pin (Topology Pin 0) is declared `KSPIN_COMMUNICATION_NONE` with no category,
matching the PortCls convention for a bridge pin that would be wired to another filter
by a physical connection.

## 4. Physical connections (current state)

There is **no** `PcRegisterPhysicalConnection` / `IPort::NewConnection` call anywhere in
`src/` (verified by search). `T2AudioStartDevice` registers only two subdevices:

- `PcRegisterSubdevice(DeviceObject, L"Topology", topologyPort)` (Driver.c:122)
- `PcRegisterSubdevice(DeviceObject, L"Wave", port)` (Driver.c:177)

Consequently the WaveRT and Topology filters are registered independently and are not
wired together. This is the direct reason **no user-visible audio endpoint is created**
and is consistent with the diagnostic-mode scope (BCE transport and audio I/O remain
disabled). Wiring the two filters via a physical connection is explicitly out of scope
for the current task.

## 5. Scope note

This document is descriptive only. Per the current session constraints, no pin,
connection, endpoint, BCE, or audio-path functionality is to be added or enabled.
