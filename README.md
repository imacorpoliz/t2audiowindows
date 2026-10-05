# T2AudioPort - Windows Audio Driver for Apple T2

## Overview
Native Windows PortCls audio driver for Apple T2 chip (PCI\VEN_106B&DEV_1803) found in MacBook Pro 2019 (16,1) and similar models. Targets 6-channel speaker array at 48kHz/24-bit.

## Current Status
**Boot test completed (2026-10-05):** Driver loads successfully through PortCls framework. `STATUS_INVALID_PARAMETER (0xC000000D)` error **FIXED**. Current blocker: `STATUS_DEVICE_CONFIGURATION_ERROR (0xC0000182)` in resource mapping phase.

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
  ├── Driver.c          - DriverEntry, AddDevice, StartDevice
  ├── Device.c          - Hardware resource mapping
  ├── BceTransport.c    - T2 BCE protocol (disabled in test build)
  ├── Phase4.c          - Audio I/O commands (disabled)
  ├── WaveRTMiniport.c  - WaveRT miniport (disabled)
  ├── WaveRTStream.c    - Streaming interface (disabled)
  └── T2AudioMiniport.h - Shared definitions

packaging/              - Signed driver package (INF, SYS, CAT)
tools/                  - Install/rollback PowerShell scripts
docs/                   - Documentation and test logs
  ├── logs/             - Boot test captures
  ├── research/         - Phase1 buffer analysis tools
  └── archive/          - Outdated status documents

local_artifacts/        - Build outputs (excluded from git)
local_backups/          - Apple driver backups (excluded from git)
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

## Current Limitations
- ✅ Driver loads through PortCls (DriverEntry, AddDevice succeed)
- ❌ StartDevice fails: T2AudioMapResources returns 0xC0000182
- ❌ No PCI memory resources allocated by Windows (needs investigation)
- ❌ Audio endpoints not created (expected until resource mapping fixed)
- ⚠️ BCE transport, WaveRT port creation, and audio I/O disabled in test build

## Verified Build
**SHA256:** `30867A4BC1794839E400BE83E1D64EE9CF9E86843B61819AB5F6E3B7D5088E61`  
**Size:** 28,168 bytes  
**Last tested:** 2026-10-05 20:50 UTC

## References
- Boot test results: `docs/logs/BOOT_TEST_20261005.md`
- Full debugging history: `docs/DEBUGGING_LOG.md`
- Third-party attributions: `THIRD_PARTY_LICENSES.md`

## License
Original implementation. See THIRD_PARTY_LICENSES.md for protocol research attributions.
