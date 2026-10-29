# Hardware Log Excerpt - Step P2-B Microphone Capture Test (Live I/O)

**Date**: 2026-10-29  
**Machine**: Apple MacBookPro16,1 (T2 Chip)  
**OS**: Windows 11 x64  
**Driver Versions**:
- 1.0.13.3 (SHA256: `359949490948D74D40CDFCE4C14795B53FAE38A2144ED2D8A49243F9FF82CAF3`)
- 1.0.13.4 (SHA256: `EFDE38A52B51E458C53A651FC35528CCD43EFFCE85788278A89FD6F7F223B22F`)

---

## 1. Initial Step P2-B Run (1.0.13.3) & Crash Analysis

### Test Run Details
- `Parameters\MicInput = 1`
- `Parameters\MicStartIo = 1` (Live hardware I/O enabled)
- WASAPI Capture Probe (`.\tools\Test-WasapiCapture.ps1 -DurationSeconds 4`)
- Kernel Debug log captured via `DbgViewCLI.exe --kernel`:

```text
1	1.958367	T2Audio: PowerChangeState D0
2	1.996133	T2Audio: crash guard armed
3	2.032872	T2Audio: mic NewStream
4	2.032983	T2Audio: mic AllocateAudioBuffer size=12288
5	2.033050	T2Audio: mic run summary: captured=0 silent=0 reAims=0 maxCopyUs=0
6	2.033061	T2Audio: mic run summary: captured=0 silent=0 reAims=0 maxCopyUs=0
7	2.039175	T2Audio: START_IO entry irql=0 device=0x21
8	2.039184	T2Audio: BCE send ioctl=0x00222018 dev=FFFFE28A85DEAD20 in=21 tag=71 type=1 id=0x21 msg=0
9	2.043282	T2Audio: BCE send IoCallDriver returned 0x00000103
10	2.043287	T2Audio: BCE send wait=0x00000000 iosb.Status=0x00000000 info=256
11	2.043291	T2Audio: START_IO reply size=256 tag=71 type=2 dev=0x0 msg=1 status=0x00000000
12	2.043293	T2Audio: START_IO sent to device 0x21
13	2.043294	T2Audio: mic RUN START_IO: 0x00000000
14	2.103693	T2Audio: VHCI UPDATE_TIMESTAMP #1 dev=0x21 t2=694875393170 seed=0 sample=16640
15	2.450268	T2Audio: VHCI UPDATE_TIMESTAMP #2 dev=0x21 t2=695222081221 seed=0 sample=33280
16	2.496504	T2Audio: BCE send ioctl=0x00222018 dev=FFFFE28A85DEAD20 in=837 tag=72 type=1 id=0x21 msg=18
17	2.497798	T2Audio: BCE send IoCallDriver returned 0x00000103
18	2.497803	T2Audio: BCE send wait=0x00000000 iosb.Status=0x00000000 info=256
19	2.497809	T2Audio: mic SET_INPUT_STREAM_ADDRESS_RANGES dev=0x21 stream=0x22 (837 bytes): 0x00000000 (reply 256 bytes)
20	2.497819	T2Audio: BCE send ioctl=0x00222018 dev=FFFFE28A85DEAD20 in=66 tag=73 type=1 id=0x21 msg=9
21	7.497900	T2Audio: BCE send IoCallDriver returned 0x00000103
22	7.497910	T2Audio: BCE send wait=0x00000000 iosb.Status=0x00000102 info=0
23	7.497916	T2Audio: BCE reply tag mismatch: sent 73, got 0 (size=0 type=0 msg=0)
24	7.497925	T2Audio: mic SET_PROPERTY 'nsrt' dev=0x21 (rate 48000): 0xC0000186 (reply 0 bytes)
25	7.497930	T2Audio: D0: mic ranges re-sent: 0xC0000186
26	7.498409	T2Audio: mic run summary: captured=258996 silent=3046 reAims=0 maxCopyUs=132
27	7.498426	T2Audio: STOP_IO entry irql=0 device=0x21
28	7.498457	T2Audio: BCE send ioctl=0x00222018 dev=FFFFE28A85DEAD20 in=21 tag=74 type=1 id=0x21 msg=2
```

### Root Cause Analysis of the Crash:
1. **Microphone Capture actually WORKED**:
   - `START_IO` to device `0x21` succeeded (`0x00000000`).
   - BridgeOS responded with timestamp updates (`VHCI UPDATE_TIMESTAMP #1` and `#2`).
   - The ring buffer was populated and copied to the DMA buffer (`captured=258996`). Over 258,000 real audio frames were transferred from BridgeOS!
2. **The Delayed D0 Work Item Collision**:
   - At timestamp 1.958, PortCls invoked `T2AudioPowerChangeState(PowerDeviceD0)` during device power-up.
   - `T2AudioPowerChangeState` queued `T2AudioResumeWorkRoutine` unconditionally whenever `MicRegistered` was TRUE, without checking whether the device had actually been in a powered-down state (`PoweredDown`).
   - `T2AudioResumeWorkRoutine` had a 500 ms delay (`KeDelayExecutionThread`).
   - At timestamp 2.496 (exactly ~500 ms later), the work item ran **while WASAPI audio capture was actively streaming**!
   - It invoked `T2AudioMicReRegister`, sending BCE Msg 18 (`SET_INPUT_STREAM_ADDRESS_RANGES`) and BCE Msg 9 (`SET_PROPERTY 'nsrt'`, 48000.0) to BridgeOS while I/O was active.
   - BridgeOS audio firmware rejects property changes while streaming, causing the BCE request to block and hit the 5-second timeout (`0x00000102` = `STATUS_TIMEOUT`).
   - The blocking call froze the thread for 5 seconds; when `STOP_IO` was dispatched at teardown, the IPC state was desynchronized, triggering an abrupt hardware reset / watchdog reboot (`Kernel-Power` Event 41).

---

## 2. Fix Implemented in Driver 1.0.13.4

1. **`src/Power.c` (`T2AudioPowerChangeState`)**:
   - Saved `wasPoweredDown = context->PoweredDown` before clearing it.
   - Guarded the resume work item queueing: only queue on waking from an actual power-down state (`wasPoweredDown == TRUE`), never on clean device start/boot.
2. **`src/Power.c` (`T2AudioResumeWorkRoutine`)**:
   - Guarded microphone re-announcement: check `context->MicIoActive == 0`. Never re-announce ranges or send `'nsrt'` if an active stream is already running.
3. **`src/MicCapture.c` (`T2AudioMicReRegister`)**:
   - Added guard: return `STATUS_INVALID_DEVICE_STATE` immediately if `Context->MicIoActive != 0`.

---

## 3. Hardware Verification on 1.0.13.4 (PASS)

- **Binary Hash**: `EFDE38A52B51E458C53A651FC35528CCD43EFFCE85788278A89FD6F7F223B22F`
- **Result**:
  - `START_IO` to device `0x21`: `0x00000000` (succeeded).
  - Continuous timestamp updates from BridgeOS:
    - `#1` (sample 16640)
    - `#2` (sample 33280)
    - `#3` (sample 49920)
    - `#4` (sample 66560)
    - `#5` (sample 83200)
    - `#6` (sample 99840)
    - `#7` (sample 116480)
    - `#8` (sample 133120)
  - Run summary: **`captured=141486 silent=3019 reAims=0 maxCopyUs=18`**.
  - `STOP_IO` to device `0x21`: `0x00000000` (succeeded).
  - Zero timeouts, zero desynchronization, zero crash guard strikes (`CrashGuardStrikes = 0`).
  - System remains 100% stable; audio device status is `OK` (`CM_PROB_NONE`).
