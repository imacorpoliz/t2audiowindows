# T2AudioPort Installation Package
# Complete driver installation suite for Apple T2 Audio Device
# MacBook Pro 16,1 (2019) - Windows 11

## Contents

```
Install/
├── T2AudioMiniport.sys          - Driver binary (16896 bytes)
├── T2AudioMiniport.inf          - Driver installation information
├── PreInstall-Check.ps1         - Safety check and backup script
├── Install-T2AudioDriver.ps1    - Main installation script
├── PostInstall-Validate.ps1     - Validation and testing script
├── Rollback-T2AudioDriver.ps1   - Emergency rollback script
└── README.md                    - This file
```

## Quick Start

### Prerequisites

- **Hardware:** MacBook Pro 16,1 (2019) with Apple T2 chip
- **OS:** Windows 11 (or Windows 10 with latest updates)
- **Privileges:** Administrator rights required
- **Test Signing:** Must be enabled (driver is unsigned)
- **Kernel Debugger:** STRONGLY recommended for first installation

### Installation Steps

1. **Run Pre-Installation Check:**
   ```powershell
   cd C:\Users\<your-username>\Desktop\mbp\T2AudioPort\Install
   .\PreInstall-Check.ps1
   ```
   This will:
   - Verify system requirements
   - Create backup of AppleAudio.sys
   - Check test signing status
   - Optionally enable kernel debugging

2. **Install Driver:**
   ```powershell
   .\Install-T2AudioDriver.ps1
   ```
   This will:
   - Stop audio services
   - Disable AppleAudio.sys
   - Install T2AudioMiniport driver
   - Reboot system (required)

3. **After Reboot - Validate Installation:**
   ```powershell
   .\PostInstall-Validate.ps1
   ```
   This will:
   - Check driver status
   - Verify audio endpoints
   - Run diagnostic tests
   - Optionally test audio playback

### If Something Goes Wrong

**Option 1: PowerShell Rollback**
```powershell
.\Rollback-T2AudioDriver.ps1
```

**Option 2: Safe Mode Rollback**
1. Boot to Safe Mode (F8 during boot)
2. Run rollback script
3. Reboot normally

**Option 3: WinRE Manual Recovery**
1. Boot to WinRE (Shift+Restart → Troubleshoot → Command Prompt)
2. Navigate to backup directory
3. Copy AppleAudio.sys backup to C:\Windows\System32\drivers\
4. Reboot

## What This Driver Does

T2AudioMiniport is a native Windows PortCls audio driver that enables **6-channel speaker output** on MacBook Pro 16,1 under Windows. It replaces the default AppleAudio.sys driver which is limited to stereo.

### Features
- ✅ 6-channel audio (5.1 surround)
- ✅ 48 kHz sample rate
- ✅ 24-bit depth (in 32-bit containers)
- ✅ Direct MMIO access to T2 chip
- ✅ BCE protocol integration
- ✅ QPC-based position tracking

### Known Limitations
- ⚠️ No hardware volume control (software only)
- ⚠️ Fixed 48 kHz rate (no multi-rate support)
- ⚠️ No jack detection
- ⚠️ No microphone input support yet

## Architecture

The driver implements a Pure PortCls adapter with WaveRT miniport:

```
Windows Audio Engine
       ↓
   PortCls.sys (WaveRT)
       ↓
T2AudioMiniport.sys
       ↓
   ┌─────────────┬─────────────────┐
   │             │                 │
BAR1/BAR2     AppleUSBVHCI    QPC Timer
(MMIO)        (BCE Protocol)  (Position)
   │             │                 │
   └─────────────┴─────────────────┘
                 ↓
          Apple T2 Chip
                 ↓
            6 Speakers
```

### Components

**BceTransport.c:** Kernel-mode wrapper for AppleUSBVHCI.sys, sends START_IO/STOP_IO commands via IOCTL 0x222018.

**Device.c:** BAR1/BAR2 MMIO mapping, GPR validation, BufferStruct parsing, Speaker buffer discovery.

**Phase4.c:** MMIO MDL construction, T2 protocol message serialization.

**WaveRTStream.c:** IMiniportWaveRTStream implementation, buffer allocation, position tracking, state machine.

## Safety & Risks

⚠️ **WARNING: This driver modifies system audio drivers and directly accesses hardware.**

### Potential Risks
- BSOD during or after installation
- No audio output
- Audio distortion or clicks
- System instability
- **Hardware damage (speaker failure)** - unlikely but possible

### Safety Measures
- Automatic backup before installation
- Test signing validation
- Volume limited to -30 dB during testing
- Rollback script included
- Kernel debugger support

### When NOT to Install
- ❌ Production system with important work
- ❌ No kernel debugger available
- ❌ No WinRE recovery USB prepared
- ❌ Not comfortable with potential BSOD
- ❌ Cannot afford audio hardware failure

## Troubleshooting

### Driver Won't Load

**Problem:** Device Manager shows error code 52 (unsigned driver)
**Solution:** Enable test signing:
```powershell
bcdedit.exe /set testsigning on
```
Reboot required.

**Problem:** Device Manager shows error code 10 (device cannot start)
**Solution:** Check Event Viewer (System log) for details. Common causes:
- BAR mapping failed (bad PCI resources)
- BCE transport unavailable (AppleUSBVHCI not running)
- START_IO command rejected

### No Audio Output

**Problem:** Endpoint appears but no sound
**Solution:** 
1. Check volume (should not be muted)
2. Verify device is default playback device
3. Test with simple audio (Windows sounds)
4. Check kernel debugger for START_IO status

**Problem:** Audio distorted or clicking
**Solution:** STOP PLAYBACK IMMEDIATELY. Possible causes:
- Incorrect buffer mapping (MMIO cache type wrong)
- T2 chip not in correct mode
- Buffer overflow/underflow
Run rollback script and file issue report.

### BCE Transport Failed

**Problem:** "BCE transport unavailable: 0xC0000034"
**Solution:** AppleUSBVHCI.sys not loaded. Check:
```powershell
Get-Service | Where-Object {$_.Name -like "*USBVHCI*"}
```
Driver will still load but START_IO commands will fail. Audio may not work without BCE.

## Technical Details

### Hardware Specifications
- **PCI ID:** VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01
- **BAR0:** Not used
- **BAR1:** Audio buffers (~1 MB, PAGE_READONLY | PAGE_WRITECOMBINE)
- **BAR2:** Control registers/GPR (PAGE_READONLY | PAGE_NOCACHE)
- **GPR Offset:** 0xC000
- **BufferStruct Signature:** 0x19870423

### Audio Format
```
Sample Rate:    48000 Hz
Channels:       6 (FL, FR, FC, LFE, SL, SR)
Bit Depth:      24-bit
Container:      32-bit (LSB-aligned)
Frame Size:     24 bytes (6 × 4)
Buffer Size:    ~983 KB (from T2 firmware)
FIFO Latency:   512 frames (~10.6 ms)
```

### Protocol Messages (BCE)
```
GET_DEVICE_LIST:   Message ID 101 (command)
GET_DEVICE_LIST_R: Message ID 102 (response)
START_IO:          Message ID 0   (command)
STOP_IO:           Message ID 2   (command)
```

Message format: Tag[4] + Type(1) + DeviceId(8) + MessageId(4) + Status(4)

## Development Notes

This driver was developed by reverse engineering:
- Linux KAIT2EN driver (github.com/kekrby/linux-t2)
- AppleAudio.sys behavior via kernel debugging
- User-mode AppleUSBVHCI IOCTL testing

Build environment:
- Visual Studio 2022 Community
- WDK 10.0.28000.0
- WindowsKernelModeDriver10.0 toolset
- PortCls and KS libraries

## Version History

**v1.0.0 (2026-10-05)** - Initial release
- Phase 1-4: Core driver implementation
- Phase 5: BCE transport integration
- Phase 6: Device enumeration
- Phase 7: MMIO buffer activation
- Phase 8: QPC position tracking
- Phase 9: Hardware latency configuration
- Phase 10: INF and installation scripts

## Credits

- **Linux KAIT2EN:** kekrby and contributors
- **T2 Reverse Engineering Community**
- **Windows Driver Kit Documentation:** Microsoft

## License

This driver is provided "AS IS" without warranty of any kind. Use at your own risk.

The authors are not responsible for any damage to hardware, software, or data.

## Support

For issues, questions, or contributions:
- Check PHASE5_10_COMPLETE.md for detailed documentation
- Review kernel debugger output
- Check Event Viewer (System log)
- Run PostInstall-Validate.ps1 for diagnostics

---

**Last Updated:** 2026-10-05  
**Driver Version:** 1.0.0  
**Build:** 16896 bytes, x64, Debug configuration
