# Hardware Log Excerpt - Step J3: Automatic Headphone Switching Fix (Driver 1.0.13.8)

**Date**: 2026-10-29  
**Machine**: Apple MacBookPro16,1 (T2 Audio)  
**OS**: Windows 11 Pro build 28000 x64  
**Driver Version**: `1.0.13.8`  
**Driver SHA256**: `BC881473FEE4B776792517F6E67F14D58F2632E2647EF86203CEC313B516B479`  
**Signed**: `T2AudioPort Test Certificate` (SHA1: `CA2DE95D1B065F6D6CFF6C4158DC88E2424ABBCE`), DigiCert timestamped  

---

## 1. Problem Description & Root Cause Analysis

### Prior Behavior (Driver <= 1.0.13.5):
1. In `src/Topology.c`:
   ```c
   static BOOLEAN
   T2AudioTopologyJackConnected(_In_opt_ PT2AUDIO_DEVICE_CONTEXT Context)
   {
       if (Context == NULL || !Context->JackEvents || !Context->JackSeenPlugged) {
           return TRUE;
       }
       return (Context->HeadphoneJackPlugged != 0) ? TRUE : FALSE;
   }
   ```
   And in `T2AudioTopologyJackProperty`:
   ```c
   jack2->JackCapabilities = (instance->DeviceContext != NULL && instance->DeviceContext->JackEvents &&
                              instance->DeviceContext->JackSeenPlugged)
                                 ? JACKDESC2_PRESENCE_DETECT_CAPABILITY : 0;
   ```
2. Because `Context->JackSeenPlugged` was `FALSE` at startup when headphones were not yet plugged in:
   - `JackCapabilities` returned `0` (reporting NO hardware presence detection capability to Windows AudioEndpointBuilder).
   - `IsConnected` returned `TRUE` (forcing the endpoint into permanently connected state).
   - Windows Audio Service registered Headphones as a static, non-pluggable endpoint and never switched default audio routing on physical jack plug/unplug events.
3. In `T2AudioTopologyJackProperty`, queries targeted directly to the pin handle without explicit `KSP_PIN` instance payload had `InstanceSize == 0`, leaving `pinId == (ULONG)-1`, which returned `STATUS_NOT_SUPPORTED`.

---

## 2. Changes Implemented in Driver 1.0.13.8

1. **`src/Topology.c`**:
   - `T2AudioTopologyJackConnected`: Removed `!Context->JackSeenPlugged`. Returns `(Context->HeadphoneJackPlugged != 0)` whenever `JackEvents` is enabled.
   - `T2AudioTopologyJackProperty`: `JackCapabilities` now unconditionally returns `JACKDESC2_PRESENCE_DETECT_CAPABILITY` when `JackEvents` is enabled.
   - Accepted `pinId == T2AUDIO_TOPO_PIN_HEADPHONE || pinId == (ULONG)-1` since `g_T2AudioJackAutomation` is attached exclusively to the headphone topology pin.
2. **`src/BceTransport.c`**:
   - In `T2AudioJackWorkRoutine`: Added `|| polls == 1` so that on initial driver start / first poll, `T2AudioNotifyJackChange` signals Windows Audio to query and latch the physical jack state.
3. **`packaging/T2AudioMiniport.inf`**:
   - Bumped `DriverVer` to `10/29/2026,1.0.13.8`.

---

## 3. Host Test Suites Verification
All 7 unit test suites executed and passed:
1. `BceProtocolLogicTest.exe` - **ALL PASSED** (0 failures)
2. `T2AudioBufferLogicTest.exe` - **ALL PASSED** (0 failures)
3. `T2AudioCrashGuardTest.exe` - **ALL PASSED** (0 failures)
4. `T2AudioDspTest.exe` - **ALL PASSED** (0 failures)
5. `T2AudioMicTest.exe` - **ALL PASSED** (0 failures)
6. `T2AudioPipelineTest.exe` - **ALL PASSED** (0 failures)
7. `T2AudioSpeakerLogicTest.exe` - **ALL PASSED** (0 failures)

---

## 4. Hardware Verification & Status
- Rebuilt `Release|x64` with MSBuild + WDK (0 errors).
- Re-signed `T2AudioMiniport.sys` and catalog `t2audiominiport.cat`.
- Driver package installed with `.\tools\T2Audio-Setup.ps1 -Action Install -Preset macos -Force`.
- `T2Audio-Setup.ps1 -Action Doctor` report:
  - Driver: `T2AudioPort 1.0.13.8 running`
  - Crash guard: `OK`
  - Default output: `Speakers`
  - Default input: `T2 microphone`
