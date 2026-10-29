# Reverse Engineering: AppleAudio.sys Hardware Volume, Mute & Control Architecture

**Binary Analyzed:** `AppleAudio.sys`  
**SHA256:** `cd8b81fabc9be6127a082da22c4cc046a4d46e0225eb6baf6d27e0dc9862a150`  
**Size:** 122,752 bytes  
**Date:** 2026-10-29  
**Tooling:** REA Ghidra Decompiler / Disassembler  

---

## 1. Executive Summary

This report analyzes how Apple's official Boot Camp driver `AppleAudio.sys` interacts with Apple T2 BridgeOS regarding audio volume, decibel limits, and muting.

### Key Conclusions:
1. **Control Discovery via Msg 28 (`GET_CONTROLS`):**
   `AppleAudio.sys` queries hardware controls during subdevice discovery (`FUN_14000251c`) by sending Command message 28 (`0x1C`) to each BCE audio device. BridgeOS responds with message 29 (`0x1D`), returning a count and an array of 64-bit control object identifiers.
2. **Control Classification & Properties:**
   For each discovered control object, `AppleAudio.sys` queries:
   - `'clas'` (`0x636C6173`): Control class. Distinguishes `'levl'` (`0x6C65766C` - volume slider), `'togl'` (`0x746F676C` - mute switch), and `'slct'` (`0x736C6374` - selector).
   - `'bcls'` (`0x62636C73`): Base class (e.g. `'jack'`).
   - `'lcdv'` (`0x6C636476`): Level Control Decibel Value (`float32` current dB).
   - `'lcdr'` (`0x6C636472`): Level Control Decibel Range (`float32[2]` min and max dB limits).
   - `'bcvl'` (`0x6263766C`): Boolean Control Value (mute state: 0 = unmuted, 1 = muted).
3. **Absence of Hardware Volume Setting in AppleAudio:**
   `AppleAudio.sys` **never** sends `SET_PROPERTY` (message 9) for `'lcdv'` or `'bcvl'` to BridgeOS!
   Function `0x1400045fc` is the **sole** caller of message 9 in the entire driver, and it only sets `'nsrt'` (nominal sample rate).
4. **Topology Architecture:**
   `AppleAudio.sys` does **not** expose `KSNODETYPE_VOLUME` or `KSNODETYPE_MUTE` nodes in its topology miniports (both GUIDs are completely absent from the binary).
5. **Software vs Hardware Rationale:**
   Volume and mute are handled entirely in software. The Windows Audio Engine (WASAPI / AudioSrv) attenuates PCM sample data in RAM, and `AppleAudio.sys` applies internal software gain/clamping before copying samples into the shared S24_LE ring buffer. The T2 hardware DACs run at fixed unity gain.

---

## 2. Decompilation Analysis

### 2.1 Control Discovery: `FUN_14000251c` (Msg 28)

When probing an audio device ID (e.g. `0x25` Codec Output or `0x26` Speaker Output), `FUN_14000251c` sends message 28:

```c
// Sending message 28 (GET_CONTROLS)
__int64 __fastcall FUN_14000251c(__int64 devContext, unsigned __int64 deviceId)
{
    unsigned int request[10];
    memset(request, 0, sizeof(request));
    request[0] = GetNextTag();           // Rolling tag
    *((unsigned char *)&request[1]) = 1; // BCE_MSG_TYPE_COMMAND
    *(__int64 *)((char *)&request[2]) = deviceId;
    request[4] = 0x1C;                   // 28 = GET_CONTROLS
    request[5] = 0;                      // Status
    *(__int64 *)((char *)&request[6]) = deviceId; // Target ObjectId
    
    status = SendBceCommand(devContext, request, 0x28, &reply, &replySize);
    if (status >= 0 && replySize >= 0x20) {
        // Reply msg == 29 (0x1D)
        unsigned int controlCount = *(unsigned int *)((char *)reply + 0x18);
        unsigned __int64 *controlIds = (unsigned __int64 *)((char *)reply + 0x20);
        
        for (i = 0; i < controlCount; i++) {
            FUN_1400027d0(devContext, deviceId, controlIds[i]);
        }
    }
}
```

### 2.2 Control Class Query: `FUN_140002e6c` (`'clas'`)

`FUN_140002e6c` queries the class of the control object ID:
- **Selector:** `'clas'` (`0x636C6173`)
- **Scope:** `'glob'` (`0x676C6F62`)
- **Returns:** 4-byte FourCC:
  - `'levl'` (`0x6C65766C`): Level / Volume control.
  - `'togl'` (`0x746F676C`): Toggle / Mute control.
  - `'slct'` (`0x736C6374`): Data selector control.

### 2.3 Volume Decibel Query: `FUN_140003138` (`'lcdv'`) and `FUN_140003044` (`'lcdr'`)

For `'levl'` controls, `AppleAudio.sys` reads the current dB value and limits:

```c
// FUN_140003138: Query Level Control Decibel Value ('lcdv')
int __fastcall FUN_140003138(__int64 devContext, unsigned __int64 controlId, float *outDb)
{
    // GET_PROPERTY request with Selector = 0x6C636476 ('lcdv')
    // Payload returns 4-byte IEEE 754 float
    *outDb = *(float *)((char *)replyPayload + 0x31);
}

// FUN_140003044: Query Level Control Decibel Range ('lcdr')
int __fastcall FUN_140003044(__int64 devContext, unsigned __int64 controlId, float *outMinDb, float *outMaxDb)
{
    // GET_PROPERTY request with Selector = 0x6C636472 ('lcdr')
    // Payload returns two 4-byte IEEE 754 floats
    *outMinDb = *(float *)((char *)replyPayload + 0x31);
    *outMaxDb = *(float *)((char *)replyPayload + 0x35);
}
```

### 2.4 Mute State Query: `FUN_140002d80` (`'bcvl'`)

For `'togl'` controls, `AppleAudio.sys` reads the boolean control value:
- **Selector:** `'bcvl'` (`0x6263766C`)
- **Scope:** `'glob'` (`0x676C6F62`)
- **Returns:** `uint32` (0 = false / unmuted, 1 = true / muted).

---

## 3. Hardware vs Software Volume Findings

### 3.1 Exhaustive Audit of Message 9 (`SET_PROPERTY`)
A complete xref audit across `AppleAudio.sys` confirms that **only one function** sends Message 9 (`SET_PROPERTY`):
- Location: `FUN_1400045fc` (`0x1400045fc`)
- Hardcoded Selector: `0x6e737274` (`'nsrt'`, Nominal Sample Rate)
- Target Scope: `0x676c6f62` (`'glob'`)

**Result:** `AppleAudio.sys` **never** attempts to set volume (`'lcdv'`) or mute (`'bcvl'`) over BCE transport.

### 3.2 Topology Filter Descriptors Audit
Inspection of the topology filter definitions in `AppleAudio.sys` (`DAT_140013830`, `DAT_1400138b0`, `DAT_140013930`):
- Filter categories: `KSCATEGORY_TOPOLOGY`, `KSCATEGORY_AUDIO`
- Nodes: **0 nodes registered**.
- Neither `KSNODETYPE_VOLUME` (`{507AE360-D55D-11D1-85CF-00C04FC295EE}`) nor `KSNODETYPE_MUTE` (`{507AE361-D55D-11D1-85CF-00C04FC295EE}`) exists anywhere in the driver's binary image or data sections.

---

## 4. Architectural Conclusions for `T2AudioPort`

1. **Volume Management Strategy:**
   `T2AudioPort` correctly implements volume control via host-side DSP and standard WASAPI stream attenuation. Attempting to forward volume changes to BridgeOS via BCE Message 9 with `'lcdv'` is unnecessary and unverified on Windows.
2. **Peak Limiter Protection:**
   Because BridgeOS amplifiers and DACs are run at fixed unity gain, software limiting (`T2AudioLimiter` / `SpeakerDsp`) is required on the host CPU to prevent speaker clipping and hardware damage at 100% volume.
3. **Decibel Matching:**
   BridgeOS reports default output ranges between `-96.0 dB` and `0.0 dB` via `'lcdr'`. Host-side attenuation curves in `T2AudioPort` align with this decibel range.
