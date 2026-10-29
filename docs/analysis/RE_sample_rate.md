# Reverse Engineering: AppleAudio.sys Sample Rate & Clock Architecture

**Binary Analyzed:** `AppleAudio.sys`  
**SHA256:** `cd8b81fabc9be6127a082da22c4cc046a4d46e0225eb6baf6d27e0dc9862a150`  
**Size:** 122,752 bytes  
**Date:** 2026-10-29  
**Tooling:** REA Ghidra Decompiler / Disassembler  

---

## 1. Executive Summary

This report documents the reverse engineering of sample rate negotiation, clock synchronization, and property setting in `AppleAudio.sys` on MacBookPro16,1 (Apple T2 Audio).

### Key Findings:
1. **Nominal Sample Rate Property (`'nsrt'`):**
   `AppleAudio.sys` manages sample rates using BCE Message 9 (`SET_PROPERTY`) with selector `'nsrt'` (`0x6E737274`). This is the **only property write operation** implemented in the entire driver.
2. **64-bit Floating Point Encoding:**
   Sample rates in BCE requests are encoded as IEEE 754 double-precision floating-point numbers (`double`, 8 bytes), converted from integer frequency values via the x64 `cvtsi2sd` instruction.
3. **Sample Rate Discovery Queries:**
   During device probe, `AppleAudio.sys` reads:
   - `'nsr#'` (`0x6E737223`): Count of available sample rates.
   - `'nsra'` (`0x6E737261`): Nominal sample rate array.
   - `'pfta'` (`0x70667461`): Physical format array.
4. **Hardware Clock Fixed at 48,000 Hz:**
   The Apple T2 audio coprocessor on MacBookPro16,1 operates at a fixed nominal rate of **48,000.0 Hz** across all hardware endpoints (Speakers, Headphones, Internal Mic, Headset Mic).
5. **Role of Windows Audio Resampler:**
   `AppleAudio.sys` only exposes 48 kHz pin data ranges to PortCls. All user-mode audio streams with non-48 kHz sample rates (such as 44.1 kHz CD audio, 88.2 kHz, 96 kHz) are resampled to 48,000 Hz in software by the Windows Audio Engine (AudioSrv / WASAPI) before reaching the driver.

---

## 2. Decompilation Analysis

### 2.1 Sample Rate Configuration: `FUN_1400045fc` (Msg 9, `SET_PROPERTY`)

The decompiled implementation of `FUN_1400045fc` shows the construction of the BCE Message 9 request:

```c
// Decompiled excerpt of FUN_1400045fc
NTSTATUS __fastcall FUN_1400045fc(
    __int64 devContext,
    unsigned __int64 deviceId,
    unsigned int sampleRateHz)
{
    unsigned char request[0x38];
    double rateFloat;
    
    memset(request, 0, sizeof(request));

    // Convert integer rate to IEEE 754 double precision float
    // Assembler: cvtsi2sd xmm0, r8d
    rateFloat = (double)sampleRateHz;

    // Build BCE Message Header
    *(unsigned int *)&request[0x00] = GetNextTag();           // Rolling tag
    request[0x04] = 0x01;                                      // BCE_MSG_TYPE_COMMAND
    *(unsigned __int64 *)&request[0x08] = deviceId;            // Device ID (e.g. 0x25 or 0x26)
    *(unsigned int *)&request[0x10] = 0x09;                    // 9 = SET_PROPERTY
    *(unsigned int *)&request[0x14] = 0;                       // Status

    // Target Property Specification
    *(unsigned __int64 *)&request[0x18] = deviceId;            // Target Object ID
    *(unsigned int *)&request[0x20] = 0;                       // Element index
    *(unsigned int *)&request[0x24] = 0x676C6F62;              // Scope: 'glob'
    *(unsigned int *)&request[0x28] = 0x6E737274;              // Selector: 'nsrt'

    // Payload: 8-byte double precision float
    *(double *)&request[0x2C] = rateFloat;                     // e.g. 48000.0 (0x40E7700000000000)

    // Transmit 52-byte request to BridgeOS
    return SendBceCommand(devContext, request, 0x34, &reply, &replySize);
}
```

### 2.2 Sample Rate Enumeration

`AppleAudio.sys` enumerates supported rates by querying:
1. **`'nsr#'` (`0x6E737223`):**
   Returns a 32-bit integer indicating the number of discrete sample rate entries.
   On MacBookPro16,1, this returns `1`.
2. **`'nsra'` (`0x6E737261`):**
   Returns an array of IEEE 754 64-bit floats.
   On MacBookPro16,1, the single entry returned is:
   `0x40E7700000000000` = `48000.0`.
3. **`'pfta'` (`0x70667461`):**
   Returns available physical stream formats. All physical formats returned indicate 48,000 Hz frame timing.

---

## 3. Clock Anchoring & Timestamp Synchronization

In `AppleAudio.sys`, clock drift tracking is performed via periodic BCE Message 4 (`UPDATE_TIMESTAMP`):

```c
// Handling Message 4 in FUN_1400045e0
void __fastcall FUN_1400045e0(__int64 streamContext, struct BceTimestampMsg *msg)
{
    // msg->HostTime: KeQueryPerformanceCounter timestamp
    // msg->SampleTime: Hardware frame position (monotonic 64-bit uint)
    streamContext->LastAnchorHostTime = msg->HostTime;
    streamContext->LastAnchorSampleTime = msg->SampleTime;
}
```

`AppleAudio.sys` uses these hardware anchors to correlate audio frame positions with the system performance counter (`QPC`). When Windows WaveRT queries `GetPosition` or `GetClockRegister`, the driver projects elapsed time using:
$$\text{CurrentSampleTime} = \text{AnchorSampleTime} + \frac{(\text{CurrentQPC} - \text{AnchorHostTime}) \times 48000}{\text{QPCFrequency}}$$

---

## 4. Architectural Conclusions for `T2AudioPort`

1. **Strict 48 kHz Pin Configuration:**
   `T2AudioPort` correctly declares `KSDATARANGE_AUDIO` descriptors with minimum and maximum sample rate set strictly to `48000 Hz`. Permitting other native pin rates without hardware PLL reconfiguration would cause audio glitches and buffer overruns.
2. **Double-Precision Float in BCE Messages:**
   When sending `'nsrt'` via Message 9 in `T2AudioPort`, ensure the sample rate payload is packed as a 64-bit IEEE 754 `double` rather than a 32-bit integer.
3. **Clock Rate Tracking Alignment:**
   The `ClockRateTracking` implementation in `T2AudioPort` mirrors `AppleAudio.sys` behavior: updating stream anchors via `UPDATE_TIMESTAMP` (Message 4) and smoothly projecting WaveRT positions without introducing jitter.
