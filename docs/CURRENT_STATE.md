# T2AudioPort Driver - Current State (Authoritative)

**Last Updated**: 2026-10-07
**Status**: Diagnostic mode — Topology + WaveRT registered and wired, BCE transport and audio I/O disabled by design
**Branch**: `diagnostics`
**Last Commit**: `c647f9a`

> This file (`docs/CURRENT_STATE.md`) is the single authoritative status document.
> The former root `CURRENT_STATE.md` is superseded and now only points here.

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
implemented. `T2AudioGetDeviceList` validates the reply message id and confirms the
advertised device count actually fits within the bytes returned before reading any id
(`BceProtocolLogic.h:T2AudioBceDeviceListCount`). `T2AudioFindSpeakerDeviceId` now walks
the device list and matches the speaker by UID — `GET_PROPERTY(GLOBAL, UID, element 0)`
per device, exact case-sensitive compare against `"Speaker"` — instead of guessing
`deviceList[0]`. The pure byte-access/length/UID helpers live in
`src/BceProtocolLogic.h` and are covered by a host-side unit test
(`tests/BceProtocolLogicTest.c`, 25 assertions). The BCE transport itself is **still not
wired in**: `Driver.c`/`Device.c` continue to hard-code `SpeakerDeviceId = 0`, so the
endpoint stays in diagnostic mode until BCE is enabled and validated on hardware.

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
| BCE Transport | Disabled | Name/UID matching implemented; `SpeakerDeviceId` still hard-coded to 0, so never invoked |
| Audio I/O (StartIo/StopIo) | Blocked | Return `STATUS_NOT_SUPPORTED` while `SpeakerDeviceId == 0` |
| Audio Playback | Not implemented | Out of scope |

---

## Current Driver State on the Test Machine

The original Apple audio driver was restored on 2026-10-06 with
`tools/Restore-AppleAudioDriver.ps1` (Brigadier, Boot Camp `061-62383`):

| Property | Value |
|----------|-------|
| Device | `PCI\VEN_106B&DEV_1803...` -> "Apple Audio Device", Class MEDIA, Status OK |
| Driver | `C:\Windows\System32\drivers\AppleAudio.sys` (112,512 bytes, Apple Boot Camp) |
| Package | original `AppleAudio.inf` published as `oem16.inf` |
| Custom driver | `T2AudioMiniport` uninstalled; not present on the machine |

The earlier custom-driver capture (Part 1 evidence) remains in
`docs/logs/capture_20261006_023822/` (43 `T2Audio:` lines); sanitized excerpt at
`docs/logs/EXCERPT_20261006_023822.md`.

Re-installing the custom driver and running a hardware test requires user approval.

---

## Diagnostic-mode boundaries (Part 4, static verification)

The following gates keep the driver in diagnostic mode. All verified by static reading
of the current source:

1. `T2AudioMapResources` sets `Context->SpeakerDeviceId = 0` unconditionally
   (`Device.c:220`) — the `BufferStruct` contract has no device-id field.
2. `T2AudioStartDevice` also sets `context->SpeakerDeviceId = 0` (`Driver.c:190`).
3. `T2AudioFindSpeakerDeviceId` is now implemented (`BceTransport.c`) — it enumerates the
   BCE device list and matches the speaker by UID (`GET_PROPERTY(GLOBAL, UID)`, exact
   `"Speaker"` compare) — but it is **never called**: `Driver.c:190` and `Device.c:220`
   still hard-code `SpeakerDeviceId = 0`, so no device id is ever derived. The BCE
   transport is not wired in. Reply parsing rejects a mismatched message id and a device
   count that exceeds the returned bytes (`BceProtocolLogic.h`).
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
- Output: `src\bin\Release\T2AudioMiniport.sys` (~18,944 bytes).
- Debug output: `src\bin\Debug\T2AudioMiniport.sys` (~33,280 bytes).
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
3. BCE transport disabled and not wired in; speaker device id not discovered. Discovery
   logic (device-list + UID match) is implemented and host-tested but not called, so it
   is unverified against real BCE replies.
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
