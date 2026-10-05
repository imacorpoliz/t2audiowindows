# T2AudioPort Driver - Current State (Authoritative)

**Last Updated**: 2026-10-06
**Status**: Diagnostic mode — Topology + WaveRT registered, BCE transport and audio I/O disabled by design
**Branch**: `diagnostics`
**Last Commit**: `96451da`

> This file (`docs/CURRENT_STATE.md`) is the single authoritative status document.
> The former root `CURRENT_STATE.md` is superseded and now only points here.

---

## Executive Summary

`T2AudioStartDevice` completes with `STATUS_SUCCESS`. It registers a **Topology**
subdevice first, then a **WaveRT** subdevice (`Driver.c:122`, `Driver.c:177`).
No user-visible audio endpoint is created, because the two filters are not wired by a
physical connection and the device is intentionally held in diagnostic mode.

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
| Audio Endpoint | Not created | No physical connection between filters (by design) |
| BCE Transport | Disabled | `SpeakerDeviceId = 0`; name matching not implemented |
| Audio I/O (StartIo/StopIo) | Blocked | Return `STATUS_NOT_SUPPORTED` while `SpeakerDeviceId == 0` |
| Audio Playback | Not implemented | Out of scope |

---

## Installed / Package Binary (Part 1 evidence)

| Property | Value |
|----------|-------|
| System file | `C:\Windows\System32\drivers\T2AudioMiniport.sys` |
| SHA256 | `62C1EE88…` |
| Size | 38,416 bytes |
| INF | `oem16.inf` |
| `packaging/` | `T2AudioMiniport.sys` / `.inf` / `t2audiominiport.cat` — byte-identical to the installed files |
| Capture | `docs/logs/capture_20261006_023822/` (43 `T2Audio:` lines, device restart, DbgView exit 0) |

Sanitized excerpt of the capture: `docs/logs/EXCERPT_20261006_023822.md`.

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
8. `T2AudioMiniportNewStream` rejects capture and any pin other than 0
   (`WaveRTMiniport.c:219`).
9. There is **no** `PcRegisterPhysicalConnection` / `IPort::NewConnection` anywhere in
   `src/` — the WaveRT and Topology filters are not wired together.

No BCE transport, no audio path, and no physical-connection registration is active.

---

## Pin scheme

The WaveRT / Topology pin, node, and connection layout is documented in
`docs/WAVERT_TOPOLOGY_PINS.md` (descriptive only; no implementation change).

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
- Output: `src\bin\Release\T2AudioMiniport.sys` (~17,920 bytes).
- Debug output: `src\bin\Debug\T2AudioMiniport.sys` (~30,720 bytes).
- Only Debug builds emit `KdPrint` output (`DBG` is not defined in Release).
- Builds are non-reproducible (see Executive Summary); do not compare hashes across rebuilds.

---

## Known Limitations

1. No user-visible audio endpoint (no physical connection between filters).
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
