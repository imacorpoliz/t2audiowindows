# Project documentation index (docs/INDEX.md)

This directory holds the technical documentation, reverse-engineering reports and development plans for the MacBook Pro 16,1 audio system on Windows 11.

---

## ⭐️ Main guides for developers and agents

| Document | Purpose |
| :--- | :--- |
| **[DRIVER_GUIDE.md](DRIVER_GUIDE.md)** | **Detailed guide to the current code**: T2 hardware, BCE protocol, architecture, streaming path, synchronisation, DSP, stages and registry parameters, build/CI, hardware tests, log reference, known issues. |
| **[ROADMAP.md](ROADMAP.md)** | **What is left until the driver is fully functional**, in order, with what each item needs (hardware, reverse engineering, code). |
| **[CURRENT_STATE.md](CURRENT_STATE.md)** | The only authoritative status document: chronology and evidence from hardware. |
| **[INSTALL.md](INSTALL.md)** | Daily-use install with `tools/T2Audio-Setup.ps1`: presets, crash guard, uninstall. |
| **[HEADPHONES.md](HEADPHONES.md)** | Plan for the headphone jack and output (and microphones). |
| **[REVERSE_PLAN.md](REVERSE_PLAN.md)** | Reverse-engineering plan for Gemini: tasks R1–R9 (minidumps, message tag, reply to `UPDATE_TIMESTAMP`, jack, initialisation, clock, format/channels, power). |
| **[analysis/RE_appleaudio_protocol.md](analysis/RE_appleaudio_protocol.md)** | The full set of AppleAudio → T2 commands, handshake and remote access in AppleUSBVHCI (`mode 1`), S24_LE sample format from the copy loop, DSP on the CPU, jack, choice of the `0x222020` receiver, power (R8), msg 28 (controls list). |
| **[analysis/RE_speaker_layout_and_clock.md](analysis/RE_speaker_layout_and_clock.md)** | The T2 clock (`UPDATE_TIMESTAMP` = host QPC ns, AppleAudio's lead model 1000 ± 500) and the 6-speaker layout of the MacBookPro16,1 (woofers 0/1 and 3/4, tweeters 2/5); what was done in code. |
| **[analysis/START_IO_BSOD_ROOT_CAUSE.md](analysis/START_IO_BSOD_ROOT_CAUSE.md)** | Root cause of BSOD `0x7E` on `START_IO` (IOCTL `0x222020` from AppleUSBVHCI) and the fix. |
| **[REVIEW_20261028.md](REVIEW_20261028.md)** | Review of 2026-10-28: problems found (clock drift, tone script bugs, WDK code analysis), what was fixed, what is left, how to roll back. |
| **[CLAUDE_REPORT_STAGE_E.md](CLAUDE_REPORT_STAGE_E.md)** | Analysis of the first-sound session (Stage E) and the plan that followed. |
| **[AGENT_HANDOVER_GUIDE.md](AGENT_HANDOVER_GUIDE.md)** | **Context handover guide** (early October 2026): T2 hardware architecture, BCE protocol spec, matrix of fixed critical defects, cyclic DMA ring mirroring maths, build and test instructions. Parts are outdated; DRIVER_GUIDE wins on conflicts. |

---

## 1. Reverse-engineering reports (docs/analysis/)

| Document | Description |
| :--- | :--- |
| **[RE_minidump.md](analysis/RE_minidump.md)** | **Task R1 (PROVEN)**: confirmation of the BSOD cause from kernel minidumps (`ks!DispatchDeviceIoControl+0x15`, `FileObject == NULL`, called from `AppleUSBVHCI+0x259a`). |
| **[RE_tag_semantics.md](analysis/RE_tag_semantics.md)** | **Task R2 (PROVEN)**: semantics of the `tag` field (`1..127` in `AppleAudio.sys`), the synchronous wait of `0x222018` in `AppleUSBVHCI.sys` without tag filtering. |
| **[RE_update_timestamp.md](analysis/RE_update_timestamp.md)** | **Task R3 (PROVEN)**: proof that `UPDATE_TIMESTAMP_RESPONSE (34)` is sent by `AppleUSBVHCI.sys` itself (`0x140001DE8`); the audio driver must not send it. |
| [AppleAudio_Reverse_Engineering_Report.md](analysis/AppleAudio_Reverse_Engineering_Report.md) | **Main REA + Ghidra 12.1.4 report**: decompilation of `FUN_140004a4c` (the `AppleUSBVHCI` gateway), stream start/stop, `BufferStruct` memory layout, solving BSOD 0x7E. |
| [AppleAudio_BufferRegistration_Report.md](analysis/AppleAudio_BufferRegistration_Report.md) | Detailed analysis of buffer and DMA ring registration in `AppleAudio.sys`. |
| [AppleAudio_Message_Path.md](analysis/AppleAudio_Message_Path.md) | Trace of the message path from the top level down to the PCI bus. |
| [AppleAudio_Playback_Path_Report.md](analysis/AppleAudio_Playback_Path_Report.md) | Architecture of the playback path in the original driver. |
| [AppleUSBVHCI_IOCTL_Analysis.md](analysis/AppleUSBVHCI_IOCTL_Analysis.md) | Preliminary analysis of the virtual host controller's IOCTL codes. |
| [AppleUSBVHCI_IOCTL_COMPLETE_ANALYSIS.md](analysis/AppleUSBVHCI_IOCTL_COMPLETE_ANALYSIS.md) | Full static analysis of IOCTL `0x222004`–`0x222018`, vtable `0x23F78`, CFG dispatch and ABCE descriptors. |
| [BUFFER_PATH_ANALYSIS.md](analysis/BUFFER_PATH_ANALYSIS.md) | Study of the buffer data paths between the driver and the T2. |
| [BUFFER_PATH_CORRECTIONS.md](analysis/BUFFER_PATH_CORRECTIONS.md) | Comparison of buffer hypotheses and corrections to the architecture models. |
| [Function_46F8_Analysis.md](analysis/Function_46F8_Analysis.md) | Analysis of `FUN_1400046F8`: GPR signature check `0x19870423`, locating BufferStruct in BAR0, device and stream registration. |
| [IOCTL_222008_CHAIN.md](analysis/IOCTL_222008_CHAIN.md) | Trace of the IOCTL `0x222008` call chain in `AppleUSBVHCI`. |
| [Linux_Windows_Structure_Mapping.md](analysis/Linux_Windows_Structure_Mapping.md) | Mapping of Linux (`kait2en`) structures to Windows (`AppleAudio.sys`) structures. |
| [T2_Audio_Protocol_Analysis.md](analysis/T2_Audio_Protocol_Analysis.md) | Basic analysis of the BCE Audio binary message protocol. |
| [TOPOLOGY_BLOCKER_REPORT.md](analysis/TOPOLOGY_BLOCKER_REPORT.md) | Why the PortCls topology was blocked (`STATUS_INVALID_DEVICE_STATE`) and the fix with pin counters. |
| [TRANSPORT_DECISION.md](analysis/TRANSPORT_DECISION.md) | Rationale for the transport choice (IOCTL vs direct BAR) for the open driver. |

---

## 2. Archive (docs/archive/)

Obsolete documents from the early stages (October 2026, before stages B–D passed on hardware). Kept for history only: their conclusions may contradict the current code. The current status is only in [CURRENT_STATE.md](CURRENT_STATE.md).

| Directory / document | Contents |
| :--- | :--- |
| [archive/planning/](archive/planning/) | The initial porting plan, work logs, early status reports. |
| [archive/research/](archive/research/) | Sources of early tools for parsing BufferStruct and the ring (binaries removed). |
| [archive/DEBUGGING_LOG.md](archive/DEBUGGING_LOG.md) | Debugging log up to 2026-10-07 (MapResources, BAR, topology). |
| `archive/BAR_MAPPING_*`, `MAPRESOURCES_ROOT_CAUSE`, `RESOURCE_ANALYSIS`, `PHASE3_*`, `PHASE4_*`, `SESSION_COMPLETE`, `DIAGNOSTIC_SESSION_20261005`, `TOPOLOGY_BLOCKER_REPORT_20261006` | Analysis of early device-start blockers, all resolved. |
| `archive/INSTALLATION_SUMMARY`, `PHASE*_COMPLETE`, `PROJECT_COMPLETE` | Early "done" reports, **not confirmed** on hardware. |

---

## 3. Raw dumps (evidence/traces/)

The directory `evidence/traces/` (not in the repository, kept locally) held 36 text files of disassembly excerpts, virtual method tables (`vtable_*.txt`), probing reports (`trace_*.txt`) and function dumps (`func_*.txt`) used as primary evidence for the reports.
