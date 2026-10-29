# Reverse Engineering: AppleAudio.sys Jack Detection & Switching

**Binary Analyzed:** `AppleAudio.sys`  
**SHA256:** `cd8b81fabc9be6127a082da22c4cc046a4d46e0225eb6baf6d27e0dc9862a150`  
**Size:** 122,752 bytes  
**Date:** 2026-10-29  
**Tooling:** REA Ghidra Decompiler / Disassembler  

---

## 1. Executive Summary

This report answers the five architectural questions posed in `GEMINI.md` (§ J2) regarding automatic headphone ↔ speaker switching in `AppleAudio.sys` on MacBookPro16,1.

Key findings:
1. **Jack Listener Registration (Msg 11, `ADD_PROPERTY_LISTENER`):** AppleAudio registers jack listeners during device initialization (`AddDevice` / probe phase), NOT dynamically after `START_IO`. It registers listeners for both `'outp'` (`0x6f757470`) and `'inpt'` (`0x696e7074`) with selector `'jack'` (`0x6a61636b`) for any BCE audio device possessing a control with selector `'jack'`.
2. **IOCTL 0x222020 Handler (`FUN_140001280`):** Handles incoming messages with `msg == 0xd` (`13` = `PROPERTY_CHANGED`) and `msg == 4` (`UPDATE_TIMESTAMP`). On receiving `PROPERTY_CHANGED` for `'jack'`, it **inverts** a stored boolean (`*(bool *)(g + 0x38) = (*(char *)(g + 0x38) == 0)` for `'outp'`), without inspecting the payload value. It then calls a registered callback `FUN_140006ac0` -> `FUN_140001368` which raises `KSEVENT_PINCAPS_JACKINFOCHANGE` via `IPortEvents::GenerateEventList`.
3. **Endpoint Architecture:** AppleAudio creates separate subdevices for Speaker and Codec Output (Headphones). It does not switch audio rings inside one endpoint; rather, Windows Audio Service (AudioSrv) receives the KS Jack event, re-evaluates endpoint connectivity via `KSPROPERTY_JACK_DESCRIPTION`, and automatically routes default playback to the newly connected endpoint.
4. **Jack Query (Msg 7, `GET_PROPERTY`):** Queries property selector `'jack'` (`0x6a61636b`) on scope `'outp'` (`0x6f757470`) and `'inpt'` (`0x696e7074`) using a 49-byte request. The result is read as a 32-bit unsigned integer at offset `0x31` (decimal 49) of the reply payload.

---

## 2. Detailed Findings

### J2.1 — PROPERTY_LISTENER (Msg 11, `0x1400042C4` / `FUN_1400042c4`)

#### Exact Request Structure (41 bytes, `0x29`):
| Offset | Field | Type | Value / Description |
|---|---|---|---|
| `0x00` | `Tag` | `uint32` | Rolling request tag (1..127) |
| `0x04` | `Type` | `uint8` | `0x01` (`BCE_MSG_TYPE_COMMAND`) |
| `0x05` | _pad_ | `uint8[3]` | Padding / zeroes |
| `0x08` | `DeviceId` | `uint64` | Target BCE device ID (e.g. `0x25` Codec Output) |
| `0x10` | `Message` | `uint32` | `0x0000000B` (`11` = `ADD_PROPERTY_LISTENER`) |
| `0x14` | `Status` | `uint32` | `0x00000000` |
| `0x18` | `ObjectId` | `uint64` | Target object ID (matches DeviceId) |
| `0x20` | `Element` | `uint32` | `0x00000000` |
| `0x24` | `Scope` | `uint32` | `0x6F757470` (`'outp'`) or `0x696E7074` (`'inpt'`) |
| `0x28` | `Selector` | `uint32` | `0x6A61636B` (`'jack'`) |

#### Timing and Target Devices:
- AppleAudio calls `FUN_1400042c4` during device initialization in `FUN_14000251c` (called via `thunk_FUN_1400027d0` from `FUN_140016500`).
- It iterates over devices discovered by message 22 (`GET_DEVICE_LIST`). For each device, it queries controls using message 28 (`GET_CONTROLS`).
- If a device contains a control with selector `'jack'`, it invokes `FUN_1400042c4` twice:
  1. Once with Scope = `'outp'` (`0x6f757470`).
  2. Once with Scope = `'inpt'` (`0x696e7074`).
- It is sent **at start**, never postponed until `START_IO`.

---

### J2.2 — IOCTL 0x222020 Handler (`FUN_140001280`, `0x1400012C7`–`0x140001332`)

The notification thread or completion routine reads messages returned by AppleUSBVHCI via IOCTL `0x222020`:

```c
// Decompiled excerpt from FUN_140001280:
if (msg_id == 4) {
    // 0x04 = UPDATE_TIMESTAMP
    FUN_1400045e0(g, buf);
} else if (msg_id == 0xd) {
    // 0x0D = 13 = PROPERTY_CHANGED
    uint32_t scope = *(uint32_t *)(buf + 0x21); // Scope at offset 0x21
    if (scope == 0x6f757470) { // 'outp'
        // Toggle output jack flag:
        *(bool *)(g + 0x38) = (*(char *)(g + 0x38) == 0);
        // Call output jack callback:
        if (*(code **)(g + 0x08) != NULL) {
            (*(code *)(g + 0x08))(*(undefined8 *)(g + 0x10));
        }
    } else if (scope == 0x696e7074) { // 'inpt'
        // Toggle input jack flag:
        *(bool *)(g + 0x39) = (*(char *)(g + 0x39) == 0);
        // Call input jack callback:
        if (*(code **)(g + 0x18) != NULL) {
            (*(code *)(g + 0x18))(*(undefined8 *)(g + 0x20));
        }
    }
}
```

#### What reads `[g + 0x38]` and `[g + 0x39]`?
- `FUN_1400042bc` simply reads and returns `*(bool *)(g + 0x38)`.
- `FUN_14001b36c` is the `KSPROPERTY_JACK_DESCRIPTION` property handler for the headphone pin. It calls `FUN_1400042bc`, and populates `KSJACK_DESCRIPTION.IsConnected`:
  ```c
  pJackDesc->IsConnected = FUN_1400042bc(device_ctx);
  ```

#### What do callbacks `[g + 0x08]` / `[g + 0x10]` do?
- Pointer `[g + 0x08]` is initialized to `FUN_140006ac0` with context pointer `[g + 0x10]`.
- `FUN_140006ac0` calls `FUN_140001368(ctx, 1)`.
- `FUN_140001368` resolves the `IPortEvents` interface on the PortCls wave/topology miniport and executes:
  ```c
  // Vtable offset +0x20 is IPortEvents::GenerateEventList
  (*(code *)(*(longlong *)pPortEvents + 0x20))(pPortEvents, NULL, 1, 0, 1);
  ```
  This signals `KSEVENT_PINCAPS_JACKINFOCHANGE` to Windows Audio Engine.
- **Crucial conclusion:** AppleAudio does **not** switch ring buffers or reconfigure hardware routing upon receiving `PROPERTY_CHANGED`. It strictly toggles the cached `IsConnected` flag and notifies PortCls. Windows Audio Service does the rest!

---

### J2.3 — GET_PROPERTY 'jack' (`FUN_14000330c` / `FUN_140003224`)

- **Function:** `FUN_14000330c` queries `'jack'` on scope `'outp'`. `FUN_140003224` queries `'jack'` on scope `'inpt'`.
- **Request Size:** 49 bytes (`0x31`):
  - Tag: rolling counter
  - Type: 1 (`COMMAND`)
  - DeviceId: device ID (e.g. `0x25`)
  - Message: `0x00000007` (`GET_PROPERTY`)
  - Status: 0
  - ObjectId: device ID
  - Element: 0
  - Scope: `0x6f757470` (`'outp'`)
  - Selector: `0x6a61636b` (`'jack'`)
  - DataSize: 0
- **Reply Buffer:** Allocated size `0x60d` bytes.
  - Checks: `msg == 8` (`PROPERTY_VALUE`) and `status == 0`.
  - Return Value: Read as 32-bit uint at byte offset `0x31` (`49` decimal):
    ```c
    *param_3 = *(undefined4 *)(reply_buf + 0x31);
    ```

---

### J2.4 — Render Endpoints Exposed on MacBookPro16,1

- Subdevice registration occurs in `FUN_14001612c` and `FUN_14001604c`.
- Devices are discovered from the T2 device table:
  - If a device matching "Codec Output" or "Codec" is found: creates `DAT_140013870` (`WaveSpeakerHeadphone` / `TopologySpeakerHeadphone`).
  - If a device matching "Speaker" is found: creates `DAT_1400137f0` / `DAT_140013770` (`WaveSpeaker` / `TopologySpeaker`).
- On MacBookPro16,1, both devices (`0x23` Speaker and `0x25` Codec Output) exist.
- Therefore, AppleAudio exposes **two separate render endpoints**:
  1. **Speakers** (associated with BCE device `0x23`).
  2. **Headphones / Codec Output** (associated with BCE device `0x25`), with its physical jack pin description hooked to `[g + 0x38]`.

---

### J2.5 — Other Messages Sent to 0x25 / 0x44 at Start

Analysis of all 30 BCE command dispatch call sites in `AppleAudio.sys` shows:
1. Message 22 (`GET_DEVICE_LIST`)
2. Message 24 (`GET_STREAMS`)
3. Message 26 (`GET_STREAM_FORMATS`)
4. Message 28 (`GET_CONTROLS`)
5. Control attribute queries: `'cmnm'`, `'clss'`, `'mdln'`, `'manf'`, `'levl'`, `'togl'`, `'slct'`
6. Message 7 (`GET_PROPERTY`) for `'jack'`, `'saft'`, `'ltnc'`, `'pft '`
7. Message 11 (`ADD_PROPERTY_LISTENER`) for `'jack'`
8. Message 9 (`SET_PROPERTY`) for `'nsrt'` (48000.0)
9. Message 18 (`SET_INPUT_STREAM_ADDRESS_RANGES` / buffer registration)
- **No secret vendor authorization, handshake, or proprietary enable packet exists.** Communication strictly follows the standard Apple BCE audio control protocol.

---

## 3. Root Cause of Automatic Switching Failure in T2AudioPort

1. **Why `JackProbeAll` / poll stayed 0:**
   In `T2AudioPort`, polling was probing various elements and scopes, but T2 only sends asynchronous notifications if `ADD_PROPERTY_LISTENER` (msg 11) is actively registered for the device during probe/initialization.
2. **Why AppleAudio toggles on notification:**
   Notice that in `AppleAudio.sys` (`FUN_140001280`), `[g + 0x38]` is **toggled** (`flag = !flag`) rather than read from the payload! This implies that T2's `PROPERTY_CHANGED` notification indicates an edge transition (plug or unplug event), and AppleAudio trusts the transition.
3. **Automatic Routing Mechanism in Windows:**
   Because AppleAudio exposes separate endpoints for Speakers and Headphones:
   - When Headphones are reported `IsConnected = FALSE`, AudioSrv routes playback to Speakers.
   - When `KSEVENT_PINCAPS_JACKINFOCHANGE` fires and `IsConnected` returns `TRUE`, AudioSrv automatically re-routes stream playback to Headphones.
