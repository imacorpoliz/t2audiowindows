# Project Documentation Index (docs/INDEX.md)

This directory holds the technical documentation, architecture specifications, and reverse-engineering reports for the MacBook Pro 16,1 audio system on Windows 11.

---

## ⭐️ Main Guides & Documentation

| Document | Purpose |
| :--- | :--- |
| **[DRIVER_GUIDE.md](DRIVER_GUIDE.md)** | **Detailed guide to the driver codebase**: T2 hardware, BCE protocol, architecture, streaming path, synchronisation, DSP, stages and registry parameters, build/CI, hardware tests, log reference, known issues. |
| **[CURRENT_STATE.md](CURRENT_STATE.md)** | Authoritative status document: verified features on physical hardware and version release history. |
| **[ROADMAP.md](ROADMAP.md)** | Technical roadmap and future milestones (production signing, additional T2 hardware models). |
| **[GUIDE.md](GUIDE.md)** | Step-by-step user installation and setup walkthrough. |
| **[INSTALL.md](INSTALL.md)** | Configuration reference, presets, crash guard operations, and registry parameters. |
| **[HEADPHONES.md](HEADPHONES.md)** | Architecture and implementation for the 3.5 mm headphone output, jack auto-switching, and bs2b crossfeed DSP. |
| **[WAVERT_TOPOLOGY_PINS.md](WAVERT_TOPOLOGY_PINS.md)** | WaveRT and PortCls topology pin layout and KS endpoint bindings. |

---

## 🔬 Reverse-Engineering Reports (docs/analysis/)

| Document | Description |
| :--- | :--- |
| **[RE_appleaudio_protocol.md](analysis/RE_appleaudio_protocol.md)** | AppleAudio → T2 BCE command protocol, handshake, and remote access. |
| **[RE_speaker_layout_and_clock.md](analysis/RE_speaker_layout_and_clock.md)** | T2 hardware clock and 6-transducer acoustic mapping on MacBookPro16,1. |
| **[RE_volume.md](analysis/RE_volume.md)** | DAC unity-gain analysis and software volume model in Windows Audio Engine. |
| **[RE_sample_rate.md](analysis/RE_sample_rate.md)** | Hardware clock 48 kHz lock analysis and shared-mode transparent resampling. |
| **[RE_headset_mic.md](analysis/RE_headset_mic.md)** | External TRRS headset microphone device ("Codec Input" 0x44) architecture. |
| **[RE_jack.md](analysis/RE_jack.md)** | Hardware 3.5 mm jack presence detection and property event model. |
| **[RE_mic_capture.md](analysis/RE_mic_capture.md)** | 3-microphone studio array buffer registration and DMA common buffer architecture. |
| **[START_IO_BSOD_ROOT_CAUSE.md](analysis/START_IO_BSOD_ROOT_CAUSE.md)** | Resolution of BugCheck 0x7E during START_IO via AppleUSBVHCI IOCTL 0x222020. |
| **[AppleAudio_Reverse_Engineering_Report.md](analysis/AppleAudio_Reverse_Engineering_Report.md)** | Decompilation of AppleAudio.sys gateway, stream controls, and BufferStruct layout. |
| **[AppleUSBVHCI_IOCTL_COMPLETE_ANALYSIS.md](analysis/AppleUSBVHCI_IOCTL_COMPLETE_ANALYSIS.md)** | Full static analysis of IOCTLs 0x222004–0x222018 in AppleUSBVHCI. |
| **[Linux_Windows_Structure_Mapping.md](analysis/Linux_Windows_Structure_Mapping.md)** | Cross-platform structure mapping between Linux t2bce_audio and Windows PortCls. |
