# T2AudioPort Driver - Current State (Authoritative)

**Last Updated**: 2026-10-07
**Status**: Diagnostic mode — Topology + WaveRT registered and wired, BCE transport and audio I/O disabled by design
**Branch**: `diagnostics`
**Last Commit**: `c6875d1`

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
| BCE Transport | Disabled | `SpeakerDeviceId = 0`; name matching not implemented |
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
3. `T2AudioFindSpeakerDeviceId` returns `STATUS_NOT_IMPLEMENTED` — BCE name matching via
   `GET_PROPERTY` is not implemented (`BceTransport.c:222-227`). No device id is ever derived.
4. `T2AudioCreateSpeakerMdl` zeroes outputs and returns `STATUS_NOT_SUPPORTED` when
   `SpeakerDeviceId == 0` (`Phase4.c:37-43`).
5. `T2AudioStartIo` (`Phase4.c:119`) and `T2AudioStopIo` (`Phase4.c:168`) return
   `STATUS_NOT_SUPPORTED` under the same condition.
6. `T2AudioStreamAllocateAudioBuffer` returns `STATUS_NOT_SUPPORTED` (outputs
   initialized) in diagnostic mode (`WaveRTStream.c:143`).
7. `T2AudioStreamSetState` validates `KSSTATE_STOP..KSSTATE_RUN`; it only calls
   `StartIo`/`StopIo` on RUN/STOP, which are blocked above (`WaveRTStream.c:75`).
8. `T2AudioMiniportNewStream` rejects capture and any pin other than the render
   sink (`WaveRTMiniport.c:276`).

The WaveRT and Topology filters **are** now wired together via
`PcRegisterPhysicalConnection` (`Driver.c:198`), so the physical connection is no
longer a diagnostic gate. Playback stays off because the stream gates above
(items 4-8) still block all audio I/O. No BCE transport and no audio path are active.

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
- Output: `src\bin\Release\T2AudioMiniport.sys` (~18,432 bytes).
- Debug output: `src\bin\Debug\T2AudioMiniport.sys` (~32,256 bytes).
- Only Debug builds emit `KdPrint` output (`DBG` is not defined in Release).
- Builds are non-reproducible (see Executive Summary); do not compare hashes across rebuilds.

---

## Known Limitations

1. Endpoint structure is registered (WaveRT and Topology filters wired) but not yet
   verified on hardware; playback remains disabled in diagnostic mode.
2. BCE transport disabled; speaker device id not discovered.
3. Audio I/O blocked in diagnostic mode.
4. Single fixed format: 48 kHz / 6 channel / 32-bit container (24-byte frame).
5. No volume control, power management, or hotplug handling.
6. `C4152` vtable warnings not resolved (unrelated to C4133, which is fixed).

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
