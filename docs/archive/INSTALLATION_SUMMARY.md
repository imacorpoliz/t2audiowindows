# T2AudioPort Installation Package - Final Summary

**Date:** 2026-10-05  
**Version:** 1.0.0  
**Status:** Ready for Installation

---

## Package Contents

```
C:\Users\othysa\Desktop\mbp\T2AudioPort\Install\  (58.46 KB total)

Driver Files:
  T2AudioMiniport.sys          16896 bytes    - Main driver binary
  T2AudioMiniport.inf            839 bytes    - Installation information file

Automation Scripts:
  PreInstall-Check.ps1          7569 bytes    - Pre-installation safety verification
  Install-T2AudioDriver.ps1     7340 bytes    - Driver installation automation
  PostInstall-Validate.ps1     10049 bytes    - Post-installation validation and testing
  Rollback-T2AudioDriver.ps1    9091 bytes    - Emergency rollback automation

Documentation:
  README.md                     8083 bytes    - Complete installation guide
```

---

## Installation Workflow

```
┌─────────────────────────────────────────────────────────────────┐
│                    1. PreInstall-Check.ps1                      │
│  ✓ Verify admin rights                                          │
│  ✓ Check Windows version                                        │
│  ✓ Detect Apple T2 Audio Device                                │
│  ✓ Backup AppleAudio.sys (with SHA256 verification)            │
│  ✓ Check test signing status                                    │
│  ✓ Optionally enable kernel debugger                            │
│  ✓ Generate rollback script                                     │
└─────────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────────┐
│                   2. Install-T2AudioDriver.ps1                  │
│  ✓ Verify driver files                                          │
│  ✓ Stop audio services                                          │
│  ✓ Disable AppleAudio.sys                                       │
│  ✓ Install driver via pnputil                                   │
│  ✓ Update device driver                                         │
│  ✓ Reboot system                                                │
└─────────────────────────────────────────────────────────────────┘
                              ↓
                        [REBOOT]
                              ↓
┌─────────────────────────────────────────────────────────────────┐
│                  3. PostInstall-Validate.ps1                    │
│  ✓ Check driver loaded in kernel                                │
│  ✓ Verify Device Manager status                                 │
│  ✓ Check audio endpoints                                        │
│  ✓ Validate BCE transport                                       │
│  ✓ Check Event Viewer for errors                                │
│  ✓ Optional audio playback test                                 │
│  ✓ Dump diagnostic information                                  │
└─────────────────────────────────────────────────────────────────┘
                              ↓
                    [If Problems Occur]
                              ↓
┌─────────────────────────────────────────────────────────────────┐
│                 4. Rollback-T2AudioDriver.ps1                   │
│  ✓ Stop audio services                                          │
│  ✓ Uninstall T2AudioMiniport package                            │
│  ✓ Restore AppleAudio.sys from backup                           │
│  ✓ Re-enable AppleAudio service                                 │
│  ✓ Restart audio services                                       │
│  ✓ Reboot system                                                │
└─────────────────────────────────────────────────────────────────┘
```

---

## Quick Start Commands

```powershell
# Navigate to installation directory
cd C:\Users\othysa\Desktop\mbp\T2AudioPort\Install

# Step 1: Pre-installation check (MANDATORY)
.\PreInstall-Check.ps1

# Step 2: Install driver (requires confirmation)
.\Install-T2AudioDriver.ps1

# [SYSTEM WILL REBOOT]

# Step 3: Validate installation
.\PostInstall-Validate.ps1

# If needed: Rollback (can run from Safe Mode)
.\Rollback-T2AudioDriver.ps1
```

---

## What Happens During Installation

### Pre-Installation
1. **Backup created:** `C:\Users\<you>\Desktop\mbp\T2AudioPort\Backup\AppleAudio.sys.YYYYMMDD_HHMMSS.bak`
2. **Test signing checked:** Driver won't load if disabled
3. **Kernel debugger configured:** Optional but recommended
4. **Rollback script generated:** Ready for emergency use

### Installation
1. **AppleAudio.sys disabled:** Service set to "start=disabled"
2. **T2AudioMiniport installed:** Via `pnputil /add-driver /install`
3. **Device updated:** PnP manager will bind new driver after reboot
4. **System reboots:** Driver loads during boot

### Post-Installation
1. **Driver loads:** `T2AudioMiniport.sys` → `PcInitializeAdapterDriver`
2. **Hardware detected:** BAR1/BAR2 mapped, GPR validated, BufferStruct parsed
3. **BCE transport:** AppleUSBVHCI device opened, GET_DEVICE_LIST sent
4. **Audio endpoint created:** "Speakers (Apple T2 Audio Device)" appears
5. **Stream ready:** Applications can open 6-channel, 48kHz stream

---

## Expected Kernel Debug Output

If kernel debugger is attached, expect these messages during boot:

```
T2Audio: PortCls DriverEntry success
T2Audio: PortCls AddDevice success
T2Audio: hardware validated; buffer=0x... size=0x...
T2Audio: BCE transport opened
T2Audio: found 2 BCE devices
T2Audio: using BCE device ID 0x... for Speaker
T2Audio: WaveRT subdevice registered
```

During first audio playback:
```
T2Audio: AllocateAudioBuffer: MDL=0x... size=... offset=...
T2Audio: START_IO sent to device 0x...
```

---

## Common Issues & Solutions

### Issue: Test Signing Warning
**Symptom:** PreInstall-Check shows "Test signing is disabled"
**Solution:**
```powershell
bcdedit.exe /set testsigning on
# Reboot required
```

### Issue: AppleUSBVHCI Not Found
**Symptom:** "BCE transport unavailable: 0xC0000034"
**Impact:** Driver loads but START_IO commands fail, audio may not work
**Solution:** Boot Camp drivers may not include AppleUSBVHCI. Check:
```powershell
Get-Service | Where-Object {$_.Name -like "*Apple*"}
```

### Issue: Device Manager Error Code 10
**Symptom:** Yellow exclamation mark in Device Manager
**Solution:** Check Event Viewer → System log for specific error. Common causes:
- BAR mapping failed (hardware issue)
- Driver failed to start (check dependencies)
- Resource conflict with AppleAudio.sys (ensure it's disabled)

### Issue: No Audio Output
**Symptom:** Endpoint appears but plays silence
**Diagnosis:**
1. Check default device: Settings → Sound → Output device
2. Check volume: Should not be muted
3. Kernel debugger: Verify START_IO was sent
4. Event Viewer: Look for T2Audio errors

**Solution:** If audio doesn't work after basic checks, run rollback.

---

## Files You Should NOT Delete

During installation, these files are created:

```
C:\Users\<you>\Desktop\mbp\T2AudioPort\Backup\
  ├── AppleAudio.sys.YYYYMMDD_HHMMSS.bak    ← BACKUP (CRITICAL)
  └── rollback.ps1                           ← AUTO-GENERATED ROLLBACK

C:\Windows\System32\drivers\
  ├── T2AudioMiniport.sys                    ← ACTIVE DRIVER
  └── AppleAudio.sys.before_rollback         ← ROLLBACK CREATES THIS
```

**DO NOT DELETE** backup files until you are confident the driver is stable.

---

## Performance Expectations

### Audio Quality
- **Latency:** ~10.6 ms (512 frames FIFO)
- **Sample Rate:** 48000 Hz (fixed)
- **Bit Depth:** 24-bit in 32-bit containers
- **Channels:** 6 (5.1 surround)
- **Position Accuracy:** Sub-millisecond (QPC-based)

### System Impact
- **CPU Usage:** Negligible (<1% during playback)
- **Memory:** ~1 MB for audio buffer (MMIO, no RAM copy)
- **Interrupts:** Minimal (no DMA interrupts, polling-based position)

---

## Testing Recommendations

### Phase 1: Driver Loading (Day 1)
- ✓ Install driver
- ✓ Verify Device Manager shows no errors
- ✓ Check kernel debugger output
- ✓ Confirm audio endpoint appears
- ❌ DO NOT test audio yet

### Phase 2: Basic Audio (Day 1-2)
- ✓ Set volume to -30 dB
- ✓ Play Windows notification sound (100ms)
- ✓ Listen for distortion or clicks
- ✓ If clean, try 1 second test tone
- ❌ DO NOT play music yet

### Phase 3: Extended Testing (Day 2-7)
- ✓ Play short music clips (1-2 minutes)
- ✓ Test different audio sources (YouTube, Spotify, local files)
- ✓ Monitor for glitches or dropouts
- ✓ Check system stability (no BSOD)
- ✓ Gradually increase volume if all stable

### Phase 4: Production Use (Week 2+)
- ✓ Use normally if Week 1 was stable
- ✓ Monitor Event Viewer weekly
- ✓ Keep backup available for 1 month
- ✓ Report any issues

---

## Rollback Scenarios

### Scenario 1: Driver Won't Load (BSOD)
1. Boot to Safe Mode (F8 during boot)
2. Run `Rollback-T2AudioDriver.ps1`
3. Reboot normally

### Scenario 2: No Audio
1. Run `PostInstall-Validate.ps1` to diagnose
2. If validation fails, run `Rollback-T2AudioDriver.ps1`
3. Reboot

### Scenario 3: Audio Distortion
1. STOP PLAYBACK IMMEDIATELY
2. Run `Rollback-T2AudioDriver.ps1`
3. Reboot
4. Report issue with kernel debugger output

### Scenario 4: System Won't Boot
1. Boot to WinRE (Shift+Restart → Troubleshoot → Command Prompt)
2. Navigate: `cd C:\Users\<you>\Desktop\mbp\T2AudioPort\Backup`
3. Restore: `copy AppleAudio.sys.*.bak C:\Windows\System32\drivers\AppleAudio.sys`
4. Reboot

---

## Final Checklist Before Installation

- [ ] Running on MacBook Pro 16,1 (2019)
- [ ] Windows 11 or Windows 10 latest update
- [ ] Administrator account
- [ ] Test signing enabled (`bcdedit /enum | findstr testsigning`)
- [ ] Kernel debugger enabled (recommended)
- [ ] WinRE recovery USB prepared (optional but recommended)
- [ ] Important work saved and backed up
- [ ] Read and understood risks
- [ ] Comfortable with potential BSOD
- [ ] Can afford to spend time troubleshooting if needed
- [ ] Have run `PreInstall-Check.ps1` successfully

---

## Contact & Support

**Project Location:** `C:\Users\othysa\Desktop\mbp\T2AudioPort\`

**Documentation:**
- `PHASE3_4_STATUS.md` - Phase 3-4 implementation details
- `PHASE5_10_COMPLETE.md` - Phase 5-10 complete technical documentation
- `Install\README.md` - User installation guide (this file)

**Source Code:**
- `Phase2\Driver\` - Complete driver source code
- `Phase2\bin\Debug\T2AudioMiniport.sys` - Driver binary
- `Phase1\` - Offline contract validation and testing

**For Issues:**
- Review kernel debugger output
- Check Event Viewer (System log)
- Run PostInstall-Validate.ps1
- Check Device Manager device status

---

## Acknowledgments

This driver was made possible by:
- Linux KAIT2EN project (kekrby and contributors)
- Apple T2 reverse engineering community
- Windows Driver Kit documentation
- PortCls audio architecture reference

---

**Installation package ready. Good luck!** 🎵

Remember: This is experimental software. Use at your own risk. Always have a rollback plan.
