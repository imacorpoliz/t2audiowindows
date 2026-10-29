# T2AudioPort — detailed driver guide

> This document describes the **current source code** of the branch and how to work with it: the hardware, the protocol, the driver architecture, the streaming path, the DSP, the safety model, building, hardware testing and diagnostics.
> Chronology and hardware evidence are in [`CURRENT_STATE.md`](CURRENT_STATE.md) (the only authoritative status document). What is left to do is in [`ROADMAP.md`](ROADMAP.md). Reverse-engineering reports are in [`analysis/`](analysis/).

## Contents

1. [Status and limitations](#1-status-and-limitations)
2. [Hardware: the T2 audio controller](#2-hardware-the-t2-audio-controller)
3. [The BCE protocol](#3-the-bce-protocol)
4. [Driver architecture](#4-driver-architecture)
5. [Device life cycle](#5-device-life-cycle)
6. [Streaming path (WaveRT → BAR0)](#6-streaming-path-wavert--bar0)
7. [Synchronisation and IRQL](#7-synchronisation-and-irql)
8. [Safety model, stages and registry parameters](#8-safety-model-stages-and-registry-parameters)
9. [Build and tests](#9-build-and-tests)
10. [Installation and hardware tests](#10-installation-and-hardware-tests)
11. [Diagnostics: what the log should show](#11-diagnostics-what-the-log-should-show)
12. [Known issues](#12-known-issues)
13. [Comparison with kaiT2en](#13-comparison-with-kait2en)
14. [How to make changes](#14-how-to-make-changes)
15. [Glossary](#15-glossary)

---

## 1. Status and limitations

| What | State |
|---|---|
| Build | In CI (`.github/workflows/build.yml`): WDK, Release x64, plus host tests on MSVC/gcc/clang. |
| Host tests | Six suites in `tests/` (§9.3); all pass. |
| Stages 1–5 on hardware (RAM, BAR, BCE discovery, transport, MMIO copy) | **Verified** on MacBookPro16,1 (1.0.7.0 and later). |
| `START_IO` on hardware | **Verified** (Stage D, 1.0.7.0): no BSOD, the T2 clock locks, `STOP_IO` stops cleanly. |
| Sound through the speakers | **Verified** (Stage E, 1.0.8.0), DSP level 3 approved by the owner (1.0.11.0). |
| Persistent mode (start on every boot), installer | Implemented, **not verified on hardware**. |
| Sleep / wake | Power callbacks stop and restart I/O (`src/Power.c`); **not verified on hardware**. |
| Headphones | Only the jack state is read at probe time (`HEADPHONES.md`); no output. |
| Microphone, hardware volume | Not implemented. |

By default the driver **does not start on an ordinary boot**: it needs the volatile registry key `TestSession` (§8.1), or the persistent mode `Parameters\EnableProduction=1` that the installer sets, which is protected by the crash guard (§8.3). This is deliberate: a crash must not turn into an endless reboot loop.

---

## 2. Hardware: the T2 audio controller

### 2.1 The device

* PCI `VEN_106B&DEV_1803` (on the test machine `SUBSYS_1887106B&REV_01`). This is the T2 function through which bridgeOS exposes audio to the host.
* The stock Boot Camp driver is `AppleAudio.sys`. It exposes a **stereo** endpoint. Attempts to open 6 channels through it are rejected (`archive/planning/NOTES.md` §1).
* The command channel does not go through this PCI device but through the virtual host controller **`AppleUSBVHCI.sys`** (BCE, Buffer Copy Engine), IOCTL `0x222018` (§3).

### 2.2 Memory resources

| Resource | Purpose | How the driver finds it |
|---|---|---|
| Memory resource `[0]` (kaiT2en: BAR0) | Window of the audio rings and `BufferStruct` | The first memory resource. Mapped R/W. If the resource is prefetchable (`CM_RESOURCE_MEMORY_PREFETCHABLE`), write-combining; otherwise uncached. |
| Config BAR (kaiT2en: BAR4) | GPR block at offset `+0xC000` | All memory resources are tried: the first one with `GPR[1] == 0x19870423` and `GPR[0] >= 2` is used. The order of resources in Windows need not match the BAR order. Mapped read-only. |

GPR (`config + 0xC000`, 32 bits each):

| Offset | Field |
|---|---|
| `+0x00` | version (the driver requires ≥ 2, kaiT2en ≥ 3) |
| `+0x04` | signature `0x19870423` |
| `+0x08` | offset of `BufferStruct` inside BAR0 |
| `+0x10` | sleep marker `0x19770525` (written before sleep; the driver does not use it yet) |

### 2.3 `BufferStruct` (in BAR0)

Packing 4 bytes; every offset is checked by `C_ASSERT` in `src/T2AudioMiniport.h`:

```
T2AUDIO_BUFFER_STRUCT            (header 0x10)
  +0x00 u32 Version
  +0x04 u32 Signature            = 0x19870423
  +0x08 u32 Flags
  +0x0C u8  NumDevices (+3 reserved)
  +0x10 DEVICE_METADATA Devices[20]          stride 0xBDEC

DEVICE_METADATA                  (0xBDEC)
  +0x0000 char Name[128]          "Speaker", "Codec Output", ...
  +0x0080 u8   NumInputStreams
  +0x0081 u8   NumOutputStreams
  +0x0084 STREAM_METADATA InputStreams[5]    stride 0x12E4
  +0x5EF8 STREAM_METADATA OutputStreams[5]
  +0xBD6C u8   Filler[128]

STREAM_METADATA                  (0x12E4)
  +0x0000 u8  NumBuffers
  +0x0004 BUFFER_ENTRY Buffers[100]          stride 0x30
  +0x12C4 u8  Filler[32]

BUFFER_ENTRY                     (0x30)
  +0x00 u64 Address               buffer offset FROM THE START OF BAR0
  +0x08 u64 Size
  +0x10 u64 Pad[4]
```

`T2AudioMapResources` reads `NumDevices` **once**. It validates that copy with `T2AudioDeviceTableFits` (the table must fit entirely inside the mapped BAR) and passes the same copy to `T2AudioFindSpeakerBuffer`. This protects against the value changing in device memory between the check and the walk. The device is found by the exact name `"Speaker"` with its terminator. Its buffer is `OutputStreams[0].Buffers[0]`, and `Address + Size` must fit inside BAR0.

### 2.4 The speaker ring

* Size `0x61800` = **399 360 bytes** = 16 640 frames × 24 bytes ≈ **346.7 ms** at 48 kHz.
* Format (property `'pft '` of stream `0x3A`, captured on MacBookPro16,1, `archive/planning/NOTES.md` §4):

| Field | Value |
|---|---|
| sample rate | 48 000 |
| format id | `'lpcm'` (`0x6C70636D`) |
| flags | `0x4` = SIGNED (not float, little-endian, interleaved, not packed, not aligned-high) |
| bytes per frame / packet | 24 / 24, frames per packet 1 |
| channels | **6** |
| bits per channel | **24** |

* In total: 6 channels of 4 bytes, each with 24 significant bits in the **low** three bytes (`S24_LE` in ALSA terms, which is how kaiT2en interprets it). Windows delivers 32-bit PCM aligned to the **high** bits, so each sample is shifted right by 8 bits with sign on copy (§6.5).
* bridgeOS reads the ring in a circle after `START_IO`. The hardware read position is not directly visible to the host: it is derived from `UPDATE_TIMESTAMP` messages (§6.2).

---

## 3. The BCE protocol

### 3.1 Transport (`src/BceTransport.c`)

* Device `\Device\AppleUSBVHCI`, opened with `IoGetDeviceObjectPointer(FILE_ALL_ACCESS)`. The file object reference is held for as long as the transport is open.
* `IOCTL 0x222018` = `CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)`. The input buffer is the message, the output buffer the reply. `IoStatus.Information` is the reply length (clipped to the buffer size).
* The IRP is built with `IoBuildDeviceIoControlRequest`; `FileObject` in the IRP stack is **not** set (as in `AppleAudio.sys`).
* All sends are serialised by a `KMUTEX`. Acquiring it waits at most 7000 ms, the send itself at most **6000 ms**: longer than the 5 s that AppleUSBVHCI itself waits for a reply (`0x140002128`). So a slow reply completes the IRP on the VHCI side instead of "poisoning" the transport.
* **Request tag** (first 4 bytes): a counter 1..127, as in AppleAudio; set by `T2AudioSendBceMessage`. A reply with a different tag is rejected (`STATUS_DEVICE_PROTOCOL_ERROR`): AppleUSBVHCI wakes a waiting request with any RESPONSE, and also with `NOTIFICATION_ALIVE`.
* **Poisoning.** If no reply arrives within 6000 ms, the IRP is abandoned as is (no `IoCancelIrp`, which would race with the IRP being freed), its context is deliberately leaked, and the transport is marked "poisoned". From then on every send returns `STATUS_IO_TIMEOUT` immediately, and the transport is never closed: the file object reference keeps the target device alive for the hung IRP.
* Sending above `PASSIVE_LEVEL` is refused (`STATUS_INVALID_DEVICE_STATE`).

### 3.2 Message format (little-endian, unaligned)

```
header (13 bytes)
  +0  u32  tag      request counter 1..127 (bridgeOS returns it in the reply)
  +4  u8   type     1 = COMMAND, 2 = RESPONSE, 3 = NOTIFICATION
  +5  u64  device_id
base (8 bytes)
  +13 u32  msg       message code
  +17 u32  status    0 = success
payload from +21
```

### 3.3 Commands used

| Command (msg) | Reply | device_id | Request payload | Reply payload | Where |
|---|---|---|---|---|---|
| `GET_DEVICE_LIST` (101) | 102 | 0 | — | `count u64`, `count × id u64` | `T2AudioGetDeviceList` |
| `GET_PROPERTY` (7) | 8 | device | `obj u64, element u32, scope u32, selector u32, qualifier_size u64` (28 bytes) | `obj, element, scope, selector, data_size u64`, then the data (from +49) | `T2AudioGetProperty` |
| `GET_OUTPUT_STREAM_LIST` (26) | 27 | device | — | as for the device list | `T2AudioGetOutputStreamList` |
| `SET_REMOTE_ACCESS` (32) | 33 | 0 | `mode u64` | — | `T2AudioSetRemoteAccess`, **not used**: on Windows AppleUSBVHCI sends it itself (`mode 1` at start/wake, `0` at sleep); AppleAudio never sends it |
| `START_IO` (0) | 1 | speaker | — | — | `T2AudioStartIo` (`Phase4.c`) |
| `STOP_IO` (2) | 3 | speaker | — | — | `T2AudioStopIo` |

Every reply is checked: tag, `type == RESPONSE`, expected code, `status == 0`, length. Simple commands also check the `device_id` echo; properties check the `obj/element/scope/selector` echo. All parsers are pure functions in `src/BceProtocolLogic.h`, covered by tests.

### 3.4 Properties used

| Selector | Scope | Object | Value |
|---|---|---|---|
| `'uid '` `0x75696420` | `'glob'` | device | name, e.g. `Speaker` (≤ 128 bytes, no NUL) |
| `'pft '` `0x70667420` | `'glob'` | stream | 40-byte format descriptor (§3.5) |
| `'ltnc'` `0x6C746E63` | `'glob'` / `'outp'` | stream / device | latency, frames (u32) |
| `'saft'` `0x73616674` | `'outp'` | device | safety offset, frames (u32) |
| `'jack'` `0x6A61636B` | `'outp'` | headphone device (`"Codec Output"`) | 1 = plugged in (read at probe only) |

### 3.5 The `'pft '` descriptor

```
+0  u64 sample_rate   IEEE-754 double (decoded without the FPU: T2AudioBceDecodeIntegralDouble)
+8  u32 format_id     'lpcm'
+12 u32 format_flags  1 FLOAT, 2 BIG_ENDIAN, 4 SIGNED, 8 PACKED, 16 ALIGNED_HIGH,
                      32 NON_INTERLEAVED, 64 NON_MIXABLE
+16 u32 bytes_per_packet
+20 u32 frames_per_packet
+24 u32 bytes_per_frame
+28 u32 channels_per_frame
+32 u32 bits_per_channel
+36 u32 reserved
```

`T2AudioBceDeviceSampleShift` accepts only 6 channels / 48 kHz / 4-byte container / signed / little-endian / interleaved / not packed / not float:

* 32 bits → shift 0;
* 24 bits + `ALIGNED_HIGH` → 0;
* 24 bits without it → **8** (the speaker case);
* anything else → `-1`: copying to MMIO is forbidden.

### 3.6 Devices on MacBookPro16,1

`0x21` Digital Mic, `0x25` Codec Output (headphones), `0x39` **Speaker** (stream `0x3A`), `0x43` Codec Input, `0x47` Bridge Loopback.

---

## 4. Driver architecture

### 4.1 Files

| File | Language | Responsible for |
|---|---|---|
| `src/Driver.c` | C++ | `DriverEntry`, `AddDevice`, `StartDevice` (stage order, port registration), PnP hook for releasing resources, shutdown notification, registry reads, persistent mode and crash guard, the `0x222020` interception. |
| `src/Device.c` | C++ | Mapping/unmapping the BARs, finding GPR and `BufferStruct`, `T2AudioFindSpeakerBuffer`. |
| `src/BceTransport.c` | C | BCE transport, `GET_DEVICE_LIST` / `GET_PROPERTY` / `GET_OUTPUT_STREAM_LIST` / `SET_REMOTE_ACCESS`, probe (including the headphone jack) and the stream format query, `UPDATE_TIMESTAMP` handling. |
| `src/Phase4.c` | C | `START_IO` / `STOP_IO`, the (obsolete) hardware MDL. |
| `src/Power.c` | C | Power callbacks (`IAdapterPowerManagement`): STOP_IO before the device leaves D0 (query D3 and D1–D3), no MMIO writes while powered down, crash guard disarmed on D1–D3 and re-armed on D0. I/O starts again when the stream re-enters RUN. |
| `src/WaveRTMiniport.c` | C | `IMiniportWaveRT`: filter description, pins, `DataRangeIntersection`, format validation. `INITGUID` lives here. |
| `src/WaveRTStream.c` | C | `IMiniportWaveRTStream`: buffer, states, position, copying into BAR0 (plain or DSP path), clock following, latency, `run summary`. |
| `src/Topology.c` | C | `IMiniportTopology`: bridge pin → speaker pin. |
| `src/T2AudioMiniport.h` | — | Hardware structures (`C_ASSERT` on sizes), device context, prototypes. |
| `src/BceProtocolLogic.h` | C (pure) | Parsing/building BCE messages, `'pft '`, sample shift, UID comparison. Compiled into both the driver and the tests. |
| `src/T2AudioBufferLogic.h` | C (pure) | Buffer arithmetic: alignment, copy chunks, QPC → frames (with rate), sample conversion, buffer release and I/O decisions. |
| `src/T2AudioSpeakerLogic.h` | C (pure) | Plain stereo → 6-channel layout, tweeter high-pass, attenuation, clock correction and rate tracking maths. |
| `src/T2AudioDsp.h`, `src/T2AudioDspProfile16_1.h` | C (pure) | Integer speaker DSP (§8.3 `SpeakerDsp`) and its generated KAIT2EN tables (`tools/dsp/gen_kait2en_dsp.py`). |
| `src/T2AudioCrashGuard.h` | C (pure) | Crash guard decision (strikes, Safe Mode). |

Project rule: all non-trivial arithmetic and parsing live in pure headers with no kernel dependencies, and every such function is covered by a host test.

### 4.2 PortCls objects

```
Adapter (PcAddAdapterDevice, extension = PortCls + T2AUDIO_DEVICE_CONTEXT)
 ├─ Subdevice "Topology"  (PortTopology + T2AUDIO_TOPOLOGY)
 │    pin 0 bridge IN  (analog) ──► pin 1 Speaker OUT (KSNODETYPE_SPEAKER)
 ├─ Subdevice "Wave"      (PortWaveRT + T2AUDIO_MINIPORT)
 │    pin 0 render sink (PCM 2ch/32bit/48k, KSCATEGORY_AUDIO) ──► pin 1 bridge OUT
 └─ PcRegisterPhysicalConnection(Wave pin 1 → Topology pin 0)
```

The device context `T2AUDIO_DEVICE_CONTEXT` sits right after `PORT_CLASS_DEVICE_EXTENSION_SIZE` in the device extension (`T2AudioGetContext`). Main fields:

| Field | Meaning |
|---|---|
| `Bar1Mapped/Size/Physical`, `Bar1WriteCombined` | BAR0 (the rings). The name "Bar1" is historical. |
| `Bar2Mapped/Size` | config BAR (GPR) |
| `BufferStruct`, `SpeakerBufferOffset/Size` | the speaker ring found |
| `HardwareLock`, `HardwareReady` | protects MMIO against teardown (§7) |
| `BceSpeakerDeviceId`, `SpeakerDeviceId` | speaker found / "connected" (the latter only at stage ≥ 4) |
| `SpeakerStreamId`, `SpeakerFormatValid`, `SpeakerSampleShift`, `SpeakerLatencyFrames`, `SpeakerSafetyOffsetFrames` | result of the stream query (§5.3) |
| `BceHeadphoneDeviceId`, `HeadphoneJackPlugged` | headphone device and jack state at probe time |
| `TestModeEnabled`, `DiagStage`, `MmioCopyEnabled`, `BceIoEnabled`, `RawSpeakerChannels`, `ForceSystemBuffer`, `MdlVariant`, `TweeterMode`, `OutputAttenuation`, `ClockRateTracking`, `SpeakerDsp`, `DspGainDb`, `TweeterTrimDb`, `WooferLimitDb`, `DspAutoGain`, `BassDb`, `TrebleDb`, `ClarityDb`, `CompStyle`, `ExtendedProbe`, `JackEvents`, `HeadphoneAutoMute`, `HeadphoneOutput`, `HeadphoneAttenuation`, `MicInput`, `MicDsp`, `ExtraFormats`, `ResumeAfterSleep` | gates and parameters (§8) |
| `ProductionMode`, `CrashGuardArmed`, `ShutdownRegistered` | persistent mode and crash guard state |
| `ClockLock`, `ClockGeneration`, `ClockAnchorFrames/Qpc` | T2 clock anchor from `UPDATE_TIMESTAMP` (§6.2) |

### 4.3 Pin format

* The endpoint is **stereo**: `KSDATARANGE_AUDIO`, PCM, `MaximumChannels` 2, 32 bits, 48 000 Hz. With `TestSession\RawSpeakerChannels=1` it is 6 (the range is patched in `T2AudioMiniportInit`).
* `DataRangeIntersection` (pin 0) returns **`KSDATAFORMAT_WAVEFORMATEXTENSIBLE`**: 2 channels, 48 kHz, 32/32 bits, `nBlockAlign` 8, mask `KSAUDIO_SPEAKER_STEREO`, subformat PCM. In raw mode: 6 channels, `nBlockAlign` 24, mask `0x60F`. With `OutputBufferLength == 0` it returns the required size and `STATUS_BUFFER_OVERFLOW`.
* `T2AudioValidateRenderFormat(format, allowRaw, &channels)` accepts:
  * stereo (mask `0x3`);
  * in raw mode, 6 channels (mask `0x60F` or `0x3F`);
  * `wValidBitsPerSample` from 1 to 32, and a "flat" `WAVE_FORMAT_PCM` with the same channel count.

Speaker layout of the MacBookPro16,1 (from the kaiT2en DSP profile, [`analysis/RE_speaker_layout_and_clock.md`](analysis/RE_speaker_layout_and_clock.md)):

| Ring channel | 0 | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|---|
| Speaker | woofer L | woofer L | **tweeter L** | woofer R | woofer R | **tweeter R** |
| From stereo (plain path) | L | L | L through 3 kHz HPF | R | R | R through 3 kHz HPF |
| From stereo (DSP path) | L woofer chain | L woofer chain | L tweeter chain | R woofer chain | R woofer chain | R tweeter chain |

After an upgrade from the old 6-channel version Windows may keep the old endpoint format cached, and the new driver rejects it (`Validate: 6 channels not offered`). Then choose 2 channels / 32 bit / 48 kHz under "Advanced", or reinstall the device.

Why not 5.1: Windows does not upmix stereo to 5.1, so stereo would play only on channels 0 and 1 (both left woofers). The tweeters must never get the full range: Apple's curve has −90 dB at 500 Hz there. Apple's full processing (EQ, XTC, compressors, virtual bass) is reproduced by the in-driver DSP (`SpeakerDsp`, §8.3); do not add Equalizer APO with the same profile on top of it.

---

## 5. Device life cycle

### 5.1 `DriverEntry`

1. `T2AudioInitializeBceTransport`: initialises the transport mutex.
2. `PcInitializeAdapterDriver(…, T2AudioAddDevice)`.
3. `MajorFunction[IRP_MJ_PNP] = T2AudioPnpHandler` (must come **after** PortCls; the hook passes the IRP to `PcDispatchIrp`), plus the `IRP_MJ_DEVICE_CONTROL` hook for `0x222020` and the `IRP_MJ_SHUTDOWN` handler.

### 5.2 `T2AudioStartDevice` step by step

1. `T2AudioUnmapResources` (in case of a restart), the context is zeroed, `KeInitializeSpinLock(HardwareLock)`.
2. Safe Mode → refuse (`STATUS_UNSUCCESSFUL`).
3. Either a `TestSession` with `EnableTestMode=1` (then `DiagStage`, `EnableMmioCopy` (only at stage ≥ 5) and `EnableBceIo` (only at stage ≥ 4) are read), or the persistent mode `Parameters\EnableProduction=1` (stage 5 with MMIO copy and BCE I/O, after the crash guard allows the start and arms itself). Otherwise refuse (Code 10).
4. `Parameters\…`: everything in §8.3.
5. **Stage ≥ 2:** `T2AudioMapResources`: map BAR0, find the config BAR by GPR, checks, `BufferStruct`, the speaker ring. On success `HardwareReady = TRUE`.
6. Create and register Topology, then WaveRT, then the physical connection. On any error everything created is released and the BARs unmapped.
7. **Stage ≥ 3 and `HardwareReady`:** `T2AudioProbeBceDevices` (§5.3).
8. **Stage ≥ 4, speaker found:** `SpeakerDeviceId = BceSpeakerDeviceId`. The driver does not send `SET_REMOTE_ACCESS` (AppleUSBVHCI does, see `analysis/RE_appleaudio_protocol.md` §1). The transport stays open. Otherwise the transport is closed.

A failed start disarms the crash guard (wrapper `T2AudioStartDevice` → `T2AudioStartDeviceInner`).

### 5.3 BCE probe and stream query (stage ≥ 3)

```
Open \Device\AppleUSBVHCI
GET_DEVICE_LIST                       → [0x21, 0x25, 0x39, 0x43, 0x47]
for each: GET_PROPERTY 'uid '         → log; "Speaker" → BceSpeakerDeviceId,
                                        "Codec Output" → BceHeadphoneDeviceId
GET_PROPERTY(headphones, outp 'jack') → log PLUGGED/unplugged (read only)
GET_OUTPUT_STREAM_LIST(speaker)       → [0x3A]            → SpeakerStreamId
GET_PROPERTY(0x3A, 'pft ')            → format → SpeakerSampleShift, SpeakerFormatValid
GET_PROPERTY(0x3A, 'ltnc')            ┐
GET_PROPERTY(speaker, outp 'ltnc')    ├→ SpeakerLatencyFrames (sum), SpeakerSafetyOffsetFrames
GET_PROPERTY(speaker, outp 'saft')    ┘   (optional, error = 0)
```

If the format cannot be read or is not supported, the driver keeps working but **never writes PCM into BAR0**.

### 5.4 Stop and removal

`T2AudioPnpHandler` on `IRP_MN_STOP_DEVICE`, `IRP_MN_SURPRISE_REMOVAL`, `IRP_MN_REMOVE_DEVICE`:

1. Disarm the crash guard and unregister the shutdown notification.
2. `T2AudioUnmapResources`: under `HardwareLock`, `HardwareReady = FALSE` and the pointers are cleared. Then `KeFlushQueuedDpcs`, the transport is closed (unless poisoned), the MDL freed and `MmUnmapIoSpace` called. The function is idempotent.
3. `PcDispatchIrp`.

---

## 6. Streaming path (WaveRT → BAR0)

### 6.1 Why a mirror and not a direct buffer

PortCls maps the WaveRT buffer into user mode with `MmMapLockedPagesSpecifyCache(UserMode)`, and device pages (BAR) cannot be mapped that way. Handing out an MDL on the BAR produced `0x8007001F` at stream initialisation. Therefore:

* `AllocateAudioBuffer` allocates ordinary memory with `AllocatePagesForMdl` of exactly the requested size (rounded to a frame). The kernel address comes from `MapAllocatedPages(MmCached)`, and the buffer is zeroed.
* The driver copies the data from that buffer into the BAR0 ring itself.
* `FreeAudioBuffer` / the final `Release`: stop the timer → detach the buffer under `StreamLock` → `UnmapAllocatedPages` → `FreePagesFromMdl`.

The engine typically asks for 11 520 bytes (480 frames = 10 ms).

### 6.2 Clock and the "consumed" cursor

* On entering RUN, `AnchorQpc` is taken **before** `START_IO`, because the T2 cannot start reading earlier. `AnchorFrames = LastCopiedFrame` is set with it.
* Current source frame: `playFrame = AnchorFrames + TicksToFrames(QPC − AnchorQpc)` (with `ClockRateTracking=1`: at the tracked rate). `T2AudioTicksToFrames` never overflows, whatever the duration.
* `LastCopiedFrame` is the **consumed cursor**: every frame before it has been copied into BAR0 (or skipped if MMIO is off). It is what the engine is told as the play position.
* Source frame `N` is written to ring frame `N + DestOffset` (modulo 16 640). On entering RUN, `DestOffset` is chosen so that the next write lands 1000 frames (~21 ms) ahead of slot 0, where the T2 starts reading after `START_IO`.
* **T2 clock.** Each `UPDATE_TIMESTAMP` (about once per ring revolution) gives an anchor: `sampleTime` plus the delivery delay `now_ns − hostTime` if it is within 0…50 ms. `hostTime` is QPC nanoseconds, published for the T2 by AppleUSBVHCI itself; the driver unwraps the 64-bit overflow of `qpc × 10⁷` that happens after ~25.6 h of uptime. On a new anchor the DPC extrapolates the T2 read position by QPC and, if the lead left 1000 ± 500, re-aims `DestOffset` (as AppleAudio does). Slots skipped by a forward jump are zeroed. Measured T2 drift is ~10 ppm (one re-aim every ~18 min); `ClockRateTracking=1` removes even those.

### 6.3 The `T2AudioStreamAdvanceLocked` algorithm

Called under `StreamLock` and only in RUN: from `GetPosition`, from the DPC (a 2 ms timer as a safety net) and when leaving RUN (final flush).

```
playFrame = AnchorFrames + elapsed
frame     = LastCopiedFrame
if playFrame <= frame → nothing
pending = playFrame - frame
if pending > client_buffer_frames:        # the client has already overwritten old frames
    frame = playFrame - client_buffer_frames
if MMIO writes are allowed (under HardwareLock):
    if a new T2 anchor arrived → T2AudioClockCorrection(frame, DestOffset, readT2)
    while frames remain:
        chunk = T2AudioCalculateCopyChunk(
                    src = frame,                (client frame: 8 bytes stereo / 24 raw)
                    dst = frame + DestOffset,   (ring frame: 24 bytes)
                    …)                          # never crosses the end of either ring
        stereo + DSP: T2DspRenderStereoToSpeakers(dst, src, chunk, shift, OutputAttenuation, level, …)
        stereo plain: T2AudioRenderStereoToSpeakers(dst, src, chunk, shift, TweeterMode, OutputAttenuation)
        raw:          T2AudioConvertSamples(dst, src, chunk, SpeakerSampleShift + OutputAttenuation)
    _mm_sfence()                                 # flush the WC buffers
LastCopiedFrame = playFrame
```

Properties of the algorithm:

* **The engine cannot overwrite a frame before it has been copied.** The position it sees is the copied cursor, and `GetPosition` itself copies pending data before answering. So the path does not depend on the system timer ticking every 15.6 ms by default rather than every 2 ms.
* Source frame `F` lands in the T2 ring at index `(F + DestOffset) mod 16640`. The data is ~21 ms ahead of the T2 read position, and that lead is kept by the T2 clock.
* On PAUSE → RUN playback resumes exactly at `LastCopiedFrame`: no gaps, no repeats.

### 6.4 States (`SetState`)

| Transition | Actions |
|---|---|
| → RUN (from STOP/ACQUIRE/PAUSE) | needs an allocated buffer. If `LastCopiedFrame == 0` (fresh start), the BAR0 ring is zeroed (under `HardwareLock`, only when MMIO is allowed) and the DSP state is reset. Then the QPC anchor. `START_IO` if `EnableBceIo` is set, the speaker is connected and I/O is not running yet (a `START_IO` failure fails the transition). Then the anchor is published, `State = RUN` and the timer started. |
| RUN → PAUSE/ACQUIRE/STOP | stop the timer (`KeCancelTimer` + `KeFlushQueuedDpcs`), final `Advance`, `AnchorFrames = LastCopiedFrame`, `run summary` line. On STOP the cursors are reset and the BAR0 ring is zeroed. `STOP_IO` if this stream started I/O; on error the flag is cleared anyway. |

`KSSTATE`: 0 STOP, 1 ACQUIRE, 2 PAUSE, 3 RUN; these numbers appear in the log line `SetState %u`.

### 6.5 Sample conversion

`T2AudioConvertSamples(dst, src, n, shift)` does one aligned 32-bit write per sample and an arithmetic right shift with portable sign handling:

| Input (Windows, 32 bit) | Output to the ring (S24_LE) |
|---|---|
| `0x7FFFFF00` (+full scale) | `0x007FFFFF` |
| `0x80000000` (−full scale) | `0xFF800000` |
| `0x00000100` (+1 LSB of 24 bits) | `0x00000001` |

### 6.6 Position and latency

* `GetPosition`: `PlayOffset = (LastCopiedFrame × client frame size) mod buffer size`, `WriteOffset = PlayOffset + 512 frames`.
* `GetPositionRegister` / `GetClockRegister` → `STATUS_NOT_SUPPORTED`: a register in ordinary kernel memory would have to be exposed to user mode, so the engine polls `GetPosition`.
* `GetHWLatency`: `FifoSize = 1000 × client frame size` (the lead in the ring), `CodecDelay = (stream latency + device latency) × 10⁷ / 48000` in 100 ns units. The DSP limiter's extra 24 frames (0.5 ms) are not reported.

### 6.7 Numbers for one example

Client buffer 11 520 bytes (480 frames), ring 399 360 bytes (16 640 frames):

* 100 ms after the start, `playFrame = 4800`;
* source frame 4800 is at byte offset `(4800 mod 480) × 8 = 0` in the client buffer;
* on a fresh start `DestOffset = 1000`, and the frame is written at offset `((4800 + 1000) mod 16640) × 24 = 139 200` (until the T2 clock asks for a correction);
* after frame 16 639 the ring is written from 0 again; `T2AudioCalculateCopyChunk` splits the chunk at the boundary.

---

## 7. Synchronisation and IRQL

| Object | Type | Protects | Taken by |
|---|---|---|---|
| `g_BceLock` | `KMUTEX` (PASSIVE) | the BCE transport, the whole IOCTL | all sends, open/close |
| `StreamLock` (per stream) | spin lock | `State`, anchors, `LastCopiedFrame`, buffer pointers, DSP state | `GetPosition`, DPC, `SetState`, buffer release |
| `HardwareLock` (per device) | spin lock | `HardwareReady` + BAR0 mappings during writes | copying, ring zeroing, `T2AudioUnmapResources` |
| `ClockLock` (per device) | spin lock | the T2 clock anchor | the `0x222020` handler (PASSIVE), the copy DPC |
| `HardwareIoActive` (per device) | interlocked LONG | "START_IO in effect" | stream leaving RUN and power callbacks (`src/Power.c`); whoever flips 1 → 0 sends STOP_IO |

* Lock order: **`StreamLock` → `HardwareLock` → `ClockLock`**. The VHCI handler takes only `ClockLock`. The reverse order never occurs.
* All BCE sends happen at `PASSIVE_LEVEL` only (checked explicitly).
* BAR0 writes only under `HardwareLock` and only while `HardwareReady`. Teardown clears the flag under the same lock before `MmUnmapIoSpace`, so an unmap in the middle of a write is impossible.
* Copy timer: `KeSetTimerEx` with a 2 ms period. Stopping: `KeCancelTimer` + `KeFlushQueuedDpcs` at PASSIVE (`SetState` and `FreeAudioBuffer` are PASSIVE callbacks).
* The stream holds a reference to `PortStream` (AddRef on creation, Release on destruction).
* The DSP runs in the copy DPC at `DISPATCH_LEVEL` under both spin locks; its cost is reported as `maxCopyUs` (§8.3).

---

## 8. Safety model, stages and registry parameters

### 8.1 `TestSession` (volatile)

`HKLM\SYSTEM\CurrentControlSet\Services\T2AudioMiniport\TestSession`. Created **only** by the agent `tools/T2Audio-TestAgent.ps1`, as a volatile key. The kernel erases it on any reboot, including after a BSOD.

| Value (DWORD) | Meaning | Default |
|---|---|---|
| `EnableTestMode` | 1 = the driver may start | absent → Code 10 (unless persistent mode) |
| `DiagStage` | 1–5, cumulative stages | 1 |
| `EnableMmioCopy` | 1 = allow writing PCM into BAR0 (stage 5 only) | 0 |
| `EnableBceIo` | 1 = allow `START_IO`, `STOP_IO` (stage ≥ 4) | 0 |
| `RawSpeakerChannels` | 1 = an endpoint with the 6 raw ring channels instead of stereo (agent: `-RawChannels`). **Tweeters unprotected**: quiet single-channel probes only | 0 |

### 8.2 Stages

| Stage | What is added | What touches the hardware |
|---|---|---|
| 1 RAM only | endpoint and stream on system memory, position by QPC | nothing |
| 2 BAR metadata | mapping the BARs, reading GPR and `BufferStruct` | device memory reads only |
| 3 BCE discovery | opening the transport, `GET_DEVICE_LIST`, `uid`, stream list, `'pft '`/`'ltnc'`/`'saft'`, jack | BCE read requests only |
| 4 hardware I/O | speaker connected, transport kept open. With `EnableBceIo`: `START_IO`/`STOP_IO` | control commands |
| 5 MMIO copy | with `EnableMmioCopy` and a valid format: PCM written into BAR0, ring zeroing | device memory writes |

Raise the stage one at a time, so that any failure can be attributed to a single step. The persistent mode always runs stage 5 with BCE I/O.

### 8.3 `Parameters` (persistent)

`…\T2AudioMiniport\Parameters`:

| Value | Meaning |
|---|---|
| `ForceSystemBuffer` | 1 = never do hardware I/O (pure software stream) |
| `MdlVariant` | experiments with an MDL on the BAR (`Phase4.c`). The current path always uses the system buffer, so this is effectively unused. |
| `TweeterMode` | plain stereo path: 1 (default) = tweeters through a 4th-order 3 kHz HPF, 0 = tweeters silent (woofers only) |
| `OutputAttenuation` | safety cut applied to every sample before it is written into the ring, in 6 dB steps (arithmetic right shift). Default **1 (−6 dB)** since 2026-10-28, after Stage E; it used to be 3 (−18 dB). 0 = off, anything above 16 is clipped to 16. Applies to the stereo, DSP and raw paths. |
| `ClockRateTracking` | 0 (default): the behaviour verified in Stage D: the source cursor runs on QPC, and under drift the lead is corrected by a jump (`re-aimed`). 1: the source cursor runs at the T2 rate: `rate = drift + 1 ppm × (1000 − lead)`, the drift is measured from the first timestamp of the run. The jump remains as an emergency path. The log shows `T2 clock: rate=… ppb drift=… ppb lead=…`. **Not verified on hardware.** Rationale and simulation: `tests/T2AudioPipelineTest.c`. |
| `SpeakerDsp` | **Default 3** since 2026-10-28: the level the owner approved on hardware (1.0.11.0, "все хорошо" — "all good"). 0: the plain path: woofers get the full range, tweeters go through a 3 kHz HPF. **1**: the stereo path through `src/T2AudioDsp.h`, the linear part of the kaiT2en 16_1 DSP: Tone1 (12th-order HPF at ~70 Hz, protects the woofers from sub-bass) → XTC (crosstalk cancellation) → Tone2 (midrange EQ) → crossover and EQ per driver → −1 dBFS peak limiter with a 0.5 ms lookahead (like ControlFreak's `lk` in kaiT2en: the gain ramps down linearly before a peak instead of jumping on it; the sound is delayed by 0.5 ms). **2**: the same plus the virtual bass (bankstown, MIT) at the start of the chain: harmonics are synthesised from the 40–150 Hz band, limited to 200–450 Hz and mixed into the signal. The small woofers cannot reproduce that bass physically, but the ear "fills it in". **3**: plus the 4-band compressor `Comp` of the graph after Tone2. Splits at 338 / 1590 / 6597 Hz, a 4th-order Linkwitz–Riley crossover with phase compensation (flat response without compression). Thresholds −24 / −12.9 / −7.4 / −6.8 dB, ratios 4 / 1.7 / 1.72 / 2.15, +2 dB makeup on the bass, +2 dB input gain, attack 5 ms, release 50 ms. Peak envelope linked over L/R, hard knee, gain recomputed every 16 frames with a ramp (`CompStyle=0`; `CompStyle=1` switches to the lsp RMS sidechain with a soft knee, below). Quiet gets louder, loud gets denser. **4**: plus the woofer resonance compressor `Buzz` (nodes `Buzz_0_1`/`Buzz_3_4`). The 110–145 Hz band is split out with an LR4 crossover, threshold −12 dB, ratio 8, attack 2 ms, release 50 ms, separately for the left and right pairs. This protects against chassis rattle at ~125 Hz. The lsp band numbering had to be inferred: the parameters of band k are tied to its start frequency `sf_k`, and the bands are sorted by frequency; the generator checks that the enabled bands are contiguous. The `Norm` autogain is enabled separately with `DspAutoGain` (below). `OutputAttenuation` applies at the input of the chain. `run summary` has `dspLimited`: how many frames the limiter reduced. Cost ~35 (level 1), ~40 (2), ~70 (3), ~60 (4) µs per 2 ms tick on the build machine. Table licence: GPL-3.0-or-later (`THIRD_PARTY_LICENSES.md`). Do not enable the Equalizer APO config from `SpeakerDSP-APO` (`imacorpoliz/mbp`) together with this: the kaiT2en processing would be applied twice. |
| `DspGainDb` | makeup gain after the DSP (only with `SpeakerDsp ≥ 1`), 0–12 dB, default **6**. Applied in front of the −1 dBFS limiter. Without the `Norm` autogain the kaiT2en chain is 5–9 dB quieter than the plain path on music. With −6 dB `OutputAttenuation` and +6 dB here the loudness is roughly (pink noise at −20 dBFS, summed over the speakers): level 0 −22.8 dB, 1 −26.1, 2 −22.4, 3–4 −18.6. |
| `TweeterTrimDb` | extra gain on the tweeters only, in the DSP path, 0–12 dB, default 0. The kaiT2en chain keeps the tweeters about 13 dB below the woofers. If the sound seems dull ("as if the sample rate were a bit low"), try 3 or 6. Peaks are held by the −1 dBFS limiter. |
| `WooferLimitDb` | how many dB below −1 dBFS to limit the woofers in the DSP path, 0–12, default 0. Against hum and rattle at maximum volume with heavy bass. The tweeters and midrange are not affected. `run summary` shows `wooferLimited=`: how many frames the woofer limiter reduced. Try `SpeakerDsp=4` (compressor and rattle protection) first, then 3–6 here. |
| `DspAutoGain` | 0 (default) or 1: autogain in front of the DSP chain (kaiT2en's `Norm` node; works with any `SpeakerDsp ≥ 1`). Since 1.0.12.0 the level is measured like lsp `autogain` (weight 5): **K-weighted loudness (ITU-R BS.1770, LUFS)**, the K-filter (shelf + RLB, 48 kHz coefficients from the standard) on L and R, the mean squares summed over the channels, time constant ~341 ms. The gain slowly brings the loudness toward −12 LUFS: quiet recordings at most +6 dB louder, loud ones at most −12 dB quieter. Down faster, up slower; below −72 LUFS (lsp's silence threshold) the gain freezes, so silence and fade-outs are not pulled up. Remaining differences from lsp: no fast surge path (peaks are caught by the limiter), one time constant instead of short/long periods. `run summary` has `autoGainDb10=`: the gain at the end of the run in tenths of a dB. **Not verified on hardware.** |
| `BassDb`, `TrebleDb`, `ClarityDb` | user tone controls, −12…+12 dB each, default 0 (flat: the stage is skipped, so the output is bit-exact with the earlier builds). Applied to the stereo input in front of the virtual bass and the kaiT2en chain (works with any `SpeakerDsp ≥ 1`). `BassDb` = low shelf at 150 Hz, `TrebleDb` = high shelf at 6 kHz (RBJ cookbook, slope S = 1), `ClarityDb` = peaking EQ at 2.5 kHz, Q 1 (speech presence). The coefficients are pre-computed for every whole dB by `tools/dsp/gen_user_eq.py` into `src/T2AudioDspUserEq.h` (Q28, no floating point in the kernel). A boost raises peaks; the limiter keeps them at −1 dBFS. Values outside ±12 are clamped. The registry DWORD holds a negative value as its two's complement (the installer writes it that way). **Not verified on hardware.** |
| `CompStyle` | 0 (default) or 1: how the `Comp` (level ≥ 3) and `Buzz` (level 4) compressors measure the level. 0: the original approximation (peak envelope, hard knee). 1: like lsp `mb_compressor` with the kaiT2en settings: **RMS** sidechain over a 10 ms window (`Comp` on the mid signal (L+R)/2, `Buzz` on each pair's own signal), **soft knee** ±6 dB (quadratic), the same thresholds, ratios, attack and release. Usually gentler on peaks and steadier on bass. Cost: about the same. **Not verified on hardware**; A/B against 0 is the test in GEMINI.md. |
| `ExtendedProbe` | 0 (default) or 1: at probe time, for every BCE device, log its input and output streams (`GET_INPUT_STREAM_LIST` 24 / `GET_OUTPUT_STREAM_LIST` 26), each stream's `'pft '` and `'ltnc'`, and the device's `'outp'`/`'inpt'` `ltnc`, `saft`, `jack`; then the whole BufferStruct table (every device, its streams and first buffers). Read-only. Groundwork for the headphone and microphone endpoints (`ROADMAP.md` §4.3, §5.1). Log lines start with `BCE detail:` and `BufferStruct device[`. **Not yet run on hardware.** |
| `JackEvents` | 0 (default) or 1: since 2026-10-28 also **polls** `'outp'/'jack'` every 1 s (bridgeOS sent no `PROPERTY_CHANGED` on hardware; the log line `jack poll started`, then `headphone jack …` only on changes; skipped while powered down; stopped in the PnP stop path). Subscribe to the headphone jack (`PROPERTY_LISTENER`, msg 11, on `'outp'/'jack'` of `"Codec Output"`, as kaiT2en and AppleAudio do). Each `PROPERTY_CHANGED` (msg 13) arriving through `0x222020` queues a PASSIVE work item that re-reads the jack. Log: `jack listener on device … : registered`, `VHCI PROPERTY_CHANGED #n …`, `headphone jack PLUGGED/unplugged (was …)`. **Not yet run on hardware.** |
| `HeadphoneAutoMute` | 0 (default) or 1, only with `JackEvents=1`: the speakers get silence while the jack reads plugged in (the stream keeps running, so sound resumes in sync on unplug). There is no headphone output yet, so with this on, plugging in headphones means no sound at all; it is a test of the jack path and a preparation for the headphone endpoint. `run summary` shows `jackMutedFrames=`. **Not yet run on hardware.** |
| `HeadphoneOutput` | 0 (default) or 1 (ROADMAP 4.4): a second endpoint **"T2 Audio Headphones"** (subdevices `WaveHeadphone` + `TopologyHeadphone`, INF interfaces of the same names). It renders into the BCE device `"Codec Output"`: the ring is output stream 0 / buffer 0 of that BufferStruct entry (hardware: BAR1 0x190000, 0x20800 bytes = 16 640 frames of 2 × 32-bit), the format comes from its `'pft '` (must be 2 ch, 48 kHz, S24 in 32-bit slots, else the copy stays off), START_IO/STOP_IO go to the headphone device id, the clock anchor comes from that device's UPDATE_TIMESTAMP. Plain stereo copy, no speaker DSP. Log: `headphone endpoint registered`, `headphone stream 0x…: shift=8`, `audio path ENABLED for headphones`. Requires the BCE I/O configuration (persistent mode). **Not verified on hardware.** |
| `HeadphoneAttenuation` | safety cut on the headphones in 6 dB steps, default **2 (−12 dB)**, 0–16. Headphones are far more sensitive than the speakers. |
| `MicInput` | 0 (default) or 1 (ROADMAP 5.2–5.4): the built-in microphone array as a capture endpoint **"T2 Audio Microphone"** (`WaveMic` + `TopologyMic`, mono, 48 kHz, 32-bit). At probe time the driver allocates a contiguous host ring (16 640 frames × 12 bytes), writes its physical address into BufferStruct `"Digital Mic"` → `input_streams[0]` (one buffer; AppleAudio's stale entries are dropped) and sends `SET_INPUT_STREAM_ADDRESS_RANGES` (msg 18), exactly as kaiT2en (`t2audio_init_bs_stream_host`) and AppleAudio (`0x46F8` + `0x44E4`). **This writes BufferStruct (device memory) and sends a new command: a new kind of hardware access.** RUN sends START_IO to the mic device; a 2 ms DPC copies the frames the T2 has written (position = UPDATE_TIMESTAMP + elapsed − input latency, as kaiT2en's capture pointer) 512 frames behind the writer. After every wake msg 18 is sent again (AppleAudio does this). PnP stop: STOP_IO, entry cleared, ring freed — in that order. Log: `mic ring registered: pa=…`, `mic SET_INPUT_STREAM_ADDRESS_RANGES …: 0x00000000`, `mic RUN START_IO`, `mic run summary: captured=… silent=… reAims=… maxCopyUs=…`. **Not verified on hardware.** |
| `MicDsp` | 1 (default): kaiT2en's 16_1 mic chain (`mic.json`): mono = (mic 0 + mic 1) / 2, ~20 Hz high-pass, +39 dB, saturation (`src/T2AudioMicLogic.h`, tested in `tests/T2AudioMicTest.c`). 0: mic 0 raw (diagnostics; very quiet). |
| `ExtraFormats` | 0 (default) or 1 (ROADMAP 2.8): the render pins (speakers, headphones) also accept 16- and 24-bit packed PCM, for exclusive-mode apps. The samples are widened to 32-bit before the normal path. The pin then advertises 16–32 bits, so Windows may offer "16/24 bit" in the device's Advanced tab; shared mode still runs at 32-bit. **Not verified on hardware.** |
| `JackProbeAll` | 0 (default) or 1, only with `JackEvents=1` (experiment, read-only): each jack poll also reads `'jack'` at 7 other addresses (`'inpt'`/`'glob'` scope, elements 1–2, the stream object, "Codec Input") and logs `jack probe #n <name> …: value (was …)` on every change. On hardware the main read (`'outp'`, element 0, device object) stayed 0 through plug/unplug. Since 2026-10-29 the headphone endpoint reports "unplugged" only after a jack read once returned "plugged" (`jack read works (first PLUGGED)`); until then it is always shown. |
| `ResumeAfterSleep` | 0 (default) or 1 (ROADMAP 2.1): if a stream is still in RUN when the device returns to D0 (its I/O was stopped before D3), a work item restarts it 0.5 s later like a PAUSE → RUN (silent ring, START_IO, new clock anchor), up to 3 tries. Without it such a stream stays silent until the app restarts playback. `SetState` and this work item are serialised by a KMUTEX. Log: `resume after sleep: restarting speaker I/O`, `status=0x00000000`. **Not verified on hardware.** |
| `EnableProduction` | 1: persistent mode: without a `TestSession` the driver starts the stage 5 configuration with BCE I/O (stereo endpoint) on every boot, under the crash guard. Set by the installer `tools/T2Audio-Setup.ps1`. 0 or absent: as before, test sessions only. |
| `CrashGuardArmed`, `CrashGuardStrikes` | written by the driver itself in persistent mode (`src/T2AudioCrashGuard.h`): the "session running" flag and the number of consecutive aborted sessions. At 2 the driver does not start until the installer resets it. Details: `docs/INSTALL.md`. |

The `run summary` line (on every exit from RUN) has `maxCopyUs=`: the longest single render + copy into the ring (DSP included), in µs, at `DISPATCH_LEVEL` under a spin lock. Windows guidance is roughly ≤ 100 µs per DPC; much more would be a reason to optimise the DSP.

### 8.4 Why there was no "normal mode" at first

While `START_IO` could crash the system, autostart on an ordinary boot would have meant a BSOD on every Windows start. The volatile key guarantees that after a crash the system boots without the driver. Now that `START_IO` is proven, the persistent mode adds autostart with the same guarantee in another form: the crash guard counts sessions that did not end cleanly and stops starting after two.

---

## 9. Build and tests

### 9.1 CI

`.github/workflows/build.yml` runs on every push and PR:

* **Linux:** all host tests with gcc and clang, `-Wall -Wextra -Werror`.
* **windows-2022:** installs the WDK, `msbuild src\T2AudioMiniport.vcxproj /p:Configuration=Release /p:Platform=x64 /p:WindowsTargetPlatformVersion=10.0.26100.0`, the tests on MSVC (`/W4`), uploads the **unsigned** `T2AudioMiniport.sys` and `.pdb` as an artifact. It also compiles the C# embedded in `Test-WasapiRender.ps1`, `Test-WasapiTone.ps1` and `T2Audio-Setup.ps1`, and runs PSScriptAnalyzer, InfVerif and WDK code analysis in report-only mode.

### 9.2 Locally

You need Visual Studio 2022 (or newer) with C++ and a WDK of the same SDK version.

```powershell
msbuild src\T2AudioMiniport.vcxproj /p:Configuration=Release /p:Platform=x64 /t:Rebuild
# result: src\bin\Release\T2AudioMiniport.sys
```

The project sets `WindowsTargetPlatformVersion 10.0.28000.0`. If that SDK is not installed, pass the installed version with `/p:WindowsTargetPlatformVersion=…`, as CI does.

### 9.3 Host tests

```powershell
foreach ($t in 'BceProtocolLogicTest','T2AudioBufferLogicTest','T2AudioSpeakerLogicTest',
               'T2AudioPipelineTest','T2AudioDspTest','T2AudioCrashGuardTest') {
    cl /nologo /W4 /Fe:tests\$t.exe tests\$t.c
    & "tests\$t.exe"; if ($LASTEXITCODE) { throw "$t failed" }
}
```

```bash
for t in BceProtocolLogicTest T2AudioBufferLogicTest T2AudioSpeakerLogicTest \
         T2AudioPipelineTest T2AudioDspTest T2AudioCrashGuardTest; do
  gcc -Wall -Wextra -Werror -O2 -o /tmp/$t tests/$t.c -lm && /tmp/$t || break
done
```

What is covered:

| Suite | Checks |
|---|---|
| `BceProtocolLogicTest` | LE accessors, device list, `GET_PROPERTY` reply (bounds, echo, status), command replies, `uid == "Speaker"`, the `'pft '` descriptor from hardware, the double decoder, the shift choice. |
| `T2AudioBufferLogicTest` | frame alignment, buffer and I/O decisions, `DeviceTableFits` (overflows), copy chunks (both boundaries, separate src/dst, lead), QPC → frames, S24 conversion. |
| `BceProtocolLogicTest` (jack) | `PROPERTY_CHANGED` parser (scope at offset 0x21 as in AppleAudio), `PROPERTY_LISTENER` builder and reply check. |
| `T2AudioSpeakerLogicTest` | stereo → 6-channel layout, tweeter HPF, output attenuation, clock correction and rate tracking maths. |
| `T2AudioPipelineTest` | the whole copy path simulated on the host: ring continuity across wraps and re-aims, drift ±80/±300 ppm, wrapped hostTime, zeroing of skipped slots, no out-of-bounds writes. |
| `T2AudioDspTest` | fixed-point DSP vs a double-precision model of the KAIT2EN graph at every level, response shape, stability, headroom and limiter (lookahead), virtual bass, compressors, makeup/trim/woofer limit, autogain, log/exp tables, cost. |
| `T2AudioCrashGuardTest` | crash guard decisions: strikes, reset, Safe Mode. |

---

## 10. Installation and hardware tests

> Before every run read the latest entry in [`CURRENT_STATE.md`](CURRENT_STATE.md). Keep the `AppleAudio.sys` restore at hand.
> For daily use the installer is enough: [`INSTALL.md`](INSTALL.md). The rest of this section is the test procedure.

### 10.1 Preparation (once)

1. Turn on test signing (`bcdedit /set testsigning on`, reboot). The project certificate: `docs/T2AudioCert_thumbprint.txt`.
2. Sign the built `.sys` and the catalog in `packaging/`: INF `T2AudioMiniport.inf`, CDF `t2audio.cdf`. Whenever the `.sys` changes, regenerate and re-sign the catalog, and raise `DriverVer` in the INF.
3. `tools/PreInstall-Check.ps1`: checks and backups.

### 10.2 Installation

* `tools/Install-T2AudioDriver.ps1` by default only adds the package to the driver store (`pnputil /add-driver` without `/install`) and does **not** bind it to the device.
* Binding to the device is explicit (see `tools/INSTALL_SCRIPTS_README.md`). A reboot may be needed afterwards (Code 14).
* `tools/T2Audio-Setup.ps1 -Action Install` does both and turns on persistent mode.

### 10.3 Test run

```powershell
.\tools\T2Audio-TestAgent.ps1 -Mode Status
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Stage 1 -ExpectedSysHash <SHA256 of the .sys>
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Stage 2 -ExpectedSysHash <…>
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Stage 3 -ExpectedSysHash <…>   # stream format
.\tools\T2Audio-TestAgent.ps1 -Mode Recover                               # after a failure/reboot
```

The agent creates the session, enables the device, writes the kernel log (DbgView) into `docs/logs/capture_*`, checks WASAPI rendering (`tools/Test-WasapiRender.ps1`). In its `finally` block it **always** disables the device and deletes the session. It refuses to work if the hash of the installed `.sys` does not match `-ExpectedSysHash`. `-BceIo` is accepted only together with `-Mmio` (the ring is zeroed before START_IO, otherwise the T2 would play its leftovers) and `-OwnerApprovedBceIo` (the owner's permission). `-RawChannels` turns on the 6 raw channels. `-Listen` keeps the session open for the owner to play music; `-SpeakerCheck` plays a tone per speaker group.

Also: `tools/Capture-T2AudioLog.ps1` (log capture), `tools/Test-WasapiTone.ps1` (test tone), `tools/Set-DefaultAudioDevice.ps1` (default endpoint), `tools/PostInstall-Validate.ps1`.

### 10.4 Rollback

* `tools/T2Audio-Setup.ps1 -Action Uninstall`: removes T2AudioPort and returns the device to AppleAudio.
* `tools/Rollback-T2AudioDriver.ps1`: removes the T2AudioPort package (works from Safe Mode too).
* `tools/Restore-AppleAudioDriver.ps1`: downloads the original `AppleAudio` from Boot Camp (Brigadier) and reinstalls it.
* After any crash: `-Mode Recover` (or the installer's `Status`), then check that the device is back on `AppleAudio` and Started.

---

## 11. Diagnostics: what the log should show

All messages start with `T2Audio:`. The expected sequence by stage (values from MacBookPro16,1):

| Stage | Line | Normal |
|---|---|---|
| all | `diagnostic session active stage=N mmioCopy=X bceIo=Y` or `persistent mode active` | matches the agent arguments / installer |
| all | `mode parameters: … SpeakerDsp=… DspGainDb=… DspAutoGain=… BassDb=… TrebleDb=… ClarityDb=… CompStyle=…` | the expected parameter values |
| persistent | `crash guard armed` | |
| ≥ 2 | `Resource[0] mapped at … (write-combining\|uncached)` | either variant |
| ≥ 2 | `GPR read: version=… signature=0x19870423 bufferOffset=…` | signature matches |
| ≥ 2 | `Device[i]: Name='Speaker' …`, `Speaker buffer[0]: Address=… Size=0x61800` | size `0x61800` |
| ≥ 2 | `MapResources SUCCESS: buffer=… size=0x61800` | |
| ≥ 3 | `BCE probe: 5 device(s)`, `device 0x39 uid="Speaker"` | |
| ≥ 3 | `BCE probe: headphone jack device 0x… : PLUGGED/unplugged` | matches reality |
| ≥ 3 | `speaker stream 0x3A pft: rate=48000 id=0x6C70636D flags=0x4 bpp=24 fpp=1 bpf=24 ch=6 bits=24` | **required before stage 5** |
| ≥ 3 | `speaker sample shift=8 latency=A+B frames safety=C frames` | **shift=8** |
| ≥ 3 | `BCE probe done; speaker id 0x39 stream 0x3A format=1` | `format=1` |
| ≥ 4 | `audio path ENABLED for speaker 0x39 (bceIo=…)` | |
| ≥ 4 + BceIo | `START_IO sent to device 0x39`, then `VHCI UPDATE_TIMESTAMP #1 dev=0x39 …` | no BSOD |
| stream | `AllocateAudioBuffer: system MDL=… size=11520 …`, `SetState 3 …` | |
| stream | `T2 clock: re-aimed …` | once at the start of each stream, then rarely (~every 18 min) |
| leaving RUN | `run summary: reAims=… maxCopyGapUs=… gaps>10ms=… gaps>lead=… skips=… maxCopyUs=… dspLimited=… wooferLimited=… autoGainDb10=… jackMutedFrames=…` | `gaps>lead=0`, `skips=0` |
| ≥ 3 + `ExtendedProbe` | `BCE detail: device 0x… input/output streams: N`, `… stream 0x… pft: …`, `… outp/inpt ltnc/saft/jack=…`, `BufferStruct device[i] '…' in=… out=…` | data for the headphone/mic work: copy all of them into the report |
| ≥ 3 + `JackEvents` | `jack listener on device 0x… : registered`; on plug/unplug `VHCI PROPERTY_CHANGED #n dev=0x… scope=0x6F757470 sel=0x6A61636B` and `headphone jack PLUGGED/unplugged` | one event per plug or unplug |
| stop | `PnP minor 0x02 - releasing hardware` | 0x02 = REMOVE, 0x04 = STOP |

Typical problems:

| Symptom | Where to look |
|---|---|
| `MapResources FAIL_F: no memory resource exposes the GPR signature` | resource order/size, the `Resource[i] GPR probe` output |
| `'pft ' unavailable` or `format unsupported; MMIO copy disabled` | the T2's reply to `GET_PROPERTY`; copying into BAR0 is skipped |
| `BCE send timed out; transport poisoned` | AppleUSBVHCI did not reply within 6 s. Every further command until the driver restarts → `STATUS_IO_TIMEOUT` |
| Code 10 right after installation | normal without a `TestSession` or persistent mode (§8) |
| The driver no longer starts in persistent mode | two crash-guard strikes: `T2Audio-Setup.ps1 -Action Status`, then `ResetGuard` or `Uninstall` |
| Crackle; `gaps>lead` > 0 or `skips` > 0 in `run summary` | DPC latency: the copy did not run for longer than the lead (~20.8 ms) |
| `maxCopyUs` well above 100 | DSP too slow for the DPC: lower `SpeakerDsp` and report |

---

## 12. Known issues

1. **Sleep/wake is not verified.** kaiT2en turns remote access off in prepare (otherwise bridgeOS stops answering while the VHCI sleeps), writes the sleep marker into GPR `+0x10`, and re-registers the buffers on wake. On Windows AppleUSBVHCI handles remote access; the driver stops I/O before leaving D0 and starts it again when the stream re-enters RUN (`src/Power.c`). Whether the ring and the T2 state survive sleep is unknown until the hardware test.
2. **The ring is not handed to the engine directly** (a PortCls limitation): an extra copy and a dependency on the cursor accuracy. It costs ~3 ms of jitter at most; measured copy gaps are ~3 ms.
3. **Assumption about the T2 cursor.** We assume that the T2 starts reading the ring at 0 at `START_IO` and runs at the nominal rate; the clock correction keeps the lead regardless. Consistent with every hardware run so far.
4. **Software volume only.** The Windows slider scales the samples before the driver; at low volume the 24-bit resolution is partly lost. Hardware volume needs msg 28 (`controls`, R7 in `REVERSE_PLAN.md`).
5. **One model.** The DSP tables, the speaker layout and the hardware id are for MacBookPro16,1 only.
6. **Windows Update may restore AppleAudio** (Microsoft-signed); rerun the installer.

Historical: BSOD `0x7E` on `START_IO` (fixed by intercepting `IOCTL 0x222020`, proven on hardware; [`analysis/START_IO_BSOD_ROOT_CAUSE.md`](analysis/START_IO_BSOD_ROOT_CAUSE.md)).

---

## 13. Comparison with kaiT2en

Reference implementation: [kaiT2en/KaiT2en-Fedora](https://github.com/kaiT2en/KaiT2en-Fedora) `modules/t2bce_audio` (GPL-2.0).

| kaiT2en | Here |
|---|---|
| GPR/`BufferStruct`, lookup by name | ✅ |
| `'pft '` → ALSA format (`S24_LE`) | ✅ the format is read, samples shifted `>> 8` |
| latency, safety offset | ✅ → `GetHWLatency` |
| ring zeroing in prepare, anchor before `START_IO` | ✅ |
| `pci_iomap_wc` for a prefetchable BAR | ✅ WC + `sfence` |
| `SET_REMOTE_ACCESS(ON=2)` before I/O, `OFF` in quiesce | ⚠️ on Windows AppleUSBVHCI does it (`mode 1`); the driver does not send it |
| position from `UPDATE_TIMESTAMP` | ✅ lead correction (+ optional rate tracking) |
| DSP graph (lsp/bankstown in PipeWire) | ✅ in the driver, integer (`src/T2AudioDsp.h`); compressors and limiters approximated |
| `NOTIFICATION_ALIVE` before commands | ❌ (not needed so far) |
| bridgeOS ring = ALSA buffer | ❌ PortCls limitation |
| suspend/resume, sleep marker | ⚠️ I/O stop/start only, not verified (§12.1) |
| jack (`'jack'` + property listener) | ⚠️ read at probe only |
| headphone output, inputs (mics) | ❌ |

kaiT2en code was not copied into the driver. Protocol facts and the order of operations were carried over; the DSP tables are generated from kaiT2en's GPL-3.0 graph (see [`THIRD_PARTY_LICENSES.md`](../THIRD_PARTY_LICENSES.md)).

---

## 14. How to make changes

1. **Arithmetic and parsing go into pure headers** (`BceProtocolLogic.h`, `T2AudioBufferLogic.h`, `T2AudioSpeakerLogic.h`, `T2AudioDsp.h`), without kernel APIs. Every branch gets a test in `tests/`. Use fixed-width types by meaning: `unsigned long` = ULONG only on Windows (on Linux it is 64-bit, so 32-bit samples use `unsigned int`).
2. **A new BCE command** only at `PASSIVE_LEVEL`, through `T2AudioSendBceMessage`, with a full reply check (tag, type, code, status, echo, length). Add the parser to `BceProtocolLogic.h` together with a test.
3. **Any new hardware access goes behind a stage gate** (§8). BAR0 writes only under `HardwareLock` and while `HardwareReady`.
4. **Never hold a spin lock during a BCE send**, and never take `HardwareLock` before `StreamLock`.
5. Before pushing: tests locally. CI builds the driver with the WDK. A new binary goes onto hardware only through the agent with `-ExpectedSysHash`, stage by stage (or through the installer once its build is verified).
6. Record every status change as a dated entry in [`CURRENT_STATE.md`](CURRENT_STATE.md). Do not write "fixed" until it is confirmed on hardware.
7. Documentation is written in English.

---

## 15. Glossary

| Term | Meaning |
|---|---|
| **T2 / bridgeOS** | Apple's coprocessor and its OS; drives the audio path |
| **BCE** | Buffer Copy Engine: the host ↔ T2 message channel; on Windows reached through `AppleUSBVHCI.sys` |
| **BAR0 / "Bar1"** | memory window with the rings and `BufferStruct`; in code the field is historically called `Bar1*` |
| **GPR** | registers in the config BAR at `+0xC000`: version, signature, `BufferStruct` offset |
| **`BufferStruct`** | table of devices/streams/buffers in BAR0, filled in by bridgeOS |
| **WaveRT** | Windows audio driver model: the engine writes into a cyclic buffer and polls the position |
| **Consumed cursor** | `LastCopiedFrame`: frames before it have been copied into the T2 ring; reported to the engine as the position |
| **Lead** | how far the writes in the T2 ring are ahead of the T2 read position: 1000 frames ≈ 21 ms, kept by `UPDATE_TIMESTAMP` |
| **S24_LE** | a 24-bit sample in the low three bytes of a 32-bit container |
| **Stage (DiagStage)** | level of hardware access allowed in a test session, 1–5 |
| **Persistent mode** | `Parameters\EnableProduction=1`: start on every boot under the crash guard |
| **Crash guard** | registry flag + strike counter that stops autostart after two sessions that did not end cleanly |
| **Poisoned transport** | the BCE transport after a timeout: it accepts no new commands until the driver restarts |
