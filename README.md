# T2AudioPort - Windows Audio Driver for Apple T2

## Overview
Native Windows PortCls audio driver for Apple T2 chip (PCI\VEN_106B&DEV_1803) found in MacBook Pro 2019 (16,1) and similar models. Targets 6-channel speaker array at 48kHz/24-bit.

## Current Status
**Phase 3 BLOCKED (2026-10-06):** WaveRT miniport registration successful. **Critical blocker:** Topology miniport `Port->Init()` returns `STATUS_INVALID_DEVICE_STATE (0xC000028C)` in all tested configurations. This prevents audio endpoint creation. Hardware access working (buffer @ 0xC1000000+0x12C000, config @ 0xC1670000).

## Requirements
- **Hardware:** MacBook Pro with Apple T2 audio (PCI\VEN_106B&DEV_1803)
- **OS:** Windows 11 with test signing enabled
- **Build Tools:**
  - Visual Studio 2022 (v143 toolset)
  - Windows Driver Kit (WDK) 10.0.28000.0
  - MSBuild 18.10.1+

## Project Structure
```
src/                    - Driver source code and build project
  ├── Driver.c          - DriverEntry, AddDevice, StartDevice, port registration
  ├── Device.c          - Hardware resource mapping (working)
  ├── Topology.c        - Topology miniport (BLOCKED at Port->Init)
  ├── WaveRTMiniport.c  - WaveRT miniport (working)
  ├── WaveRTStream.c    - WaveRT stream implementation (working)
  ├── BceTransport.c    - T2 BCE protocol (disabled, stub)
  ├── Phase4.c          - Audio I/O commands (disabled, stub)
  └── T2AudioMiniport.h - Shared definitions

packaging/              - Signed driver package (INF, SYS, CAT)
  ├── T2AudioMiniport.inf - PCI device binding (VEN_106B&DEV_1803)
  └── T2AudioMiniport.sys - Latest signed driver (oem16.inf in system)

tools/                  - Install/rollback PowerShell scripts (if present)
```

## Building
```powershell
cd src
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" `
  T2AudioMiniport.vcxproj /p:Configuration=Debug /p:Platform=x64
```

Output: `src\x64\Debug\T2AudioMiniport.sys`

## Signing
```powershell
# Requires test certificate in LocalMachine\My, Root, TrustedPublisher
signtool sign /fd SHA256 /t http://timestamp.digicert.com /a `
  /n "T2AudioPort Test Certificate" T2AudioMiniport.sys

Inf2Cat /driver:. /os:10_X64

signtool sign /fd SHA256 /t http://timestamp.digicert.com /a `
  /n "T2AudioPort Test Certificate" t2audiominiport.cat
```

## Installation
```powershell
# Enable test signing (reboot required)
bcdedit /set testsigning on
bcdedit /set nointegritychecks on
bcdedit /set loadoptions DISABLE_INTEGRITY_CHECKS

# Install driver package
pnputil /add-driver packaging\T2AudioMiniport.inf /install
```

## Progress

### ✅ Working (Phases 1-2, complete)
- Driver loads through PortCls (DriverEntry, AddDevice, StartDevice)
- PCI resource mapping: 3 memory regions mapped successfully
  - Resource[0]: Buffer memory (0xC1000000, 4MB) - BAR0
  - Resource[2]: Config memory (0xC1670000, 64KB) - BAR4
  - GPR signature validated: 0x19870423, version 3, bufferOffset 0x4000
- Speaker buffer located: offset 0x12C000, size 0x61800 bytes (48kHz/24-bit/6ch)
- WaveRT port/miniport registered successfully
- KSCATEGORY_AUDIO interface created

### ❌ Blocked (Phase 3.1, topology registration)
**Critical issue:** Topology `Port->Init()` fails with `STATUS_INVALID_DEVICE_STATE (0xC000028C)`

Tested configurations (all failed):
- Registration order: Topology FIRST / Topology AFTER WaveRT
- UnknownAdapter: NULL / WaveRT miniport pointer
- Topology structure: Direct pins / Pin→Node→Pin
- Pin categories: KSCATEGORY_AUDIO / NULL (bridge pin)
- Pin instance counts: {0,0,0} / {1,1,0}

Current descriptor:
- 2 pins: Pin0=bridge(IN,COMMUNICATION_NONE), Pin1=speaker(OUT,COMMUNICATION_NONE)
- 1 node: KSNODETYPE_SPEAKER
- 2 connections: Filter→Node, Node→Filter
- Automation: none (0 properties)

Without topology registration, no audio endpoint appears in Windows Sound Settings.

### ⏸️ Not implemented (Phases 4-5, deferred)
- BCE transport (disabled, returns STATUS_NOT_SUPPORTED)
- Hardware I/O (START_IO/STOP_IO commands disabled)
- Audio streaming (requires BCE transport + hardware initialization)

## Verified Build
**SHA256:** `BC5F0E43...` (latest with topology miniport)
**Size:** ~35KB
**Last tested:** 2026-10-06 (timestamp 1922.28s in debug log)
**Device status:** CM_PROB_FAILED_START (Code 10) due to topology Init failure

## References
- Boot test results: `docs/logs/BOOT_TEST_20261005.md`
- Full debugging history: `docs/DEBUGGING_LOG.md`
- Third-party attributions: `THIRD_PARTY_LICENSES.md`

## License
Original implementation. See THIRD_PARTY_LICENSES.md for protocol research attributions.
