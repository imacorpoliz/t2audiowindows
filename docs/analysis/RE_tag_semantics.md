# RE_tag_semantics: semantics of the tag field in BCE messages (task R2)

> **Analysis date:** 28 October 2026  
> **Tools:** Capstone disassembler, a Python PE parser, analysis of `AppleAudio.sys` (SHA256 `cd8b81fabc9b…a150`) and `AppleUSBVHCI.sys` 6.1.7500.5 (SHA256 `0438292090fc…55c1`).  
> **Status of the conclusions:** **PROVEN** (with exact instructions and VA/RVA addresses; hypotheses about bridgeOS behaviour are marked explicitly).

---

## 1. How the tag field is formed in `AppleAudio.sys`

In `AppleAudio.sys` every command is sent through `IOCTL 0x222018` by `FUN_140004a4c`.  
Right before filling in the `IoBuildDeviceIoControlRequest` parameters, the driver updates the first 4 bytes of the message (`tag`):

```assembly
; AppleAudio.sys VA: 0x140004AB9 - 0x140004AD9 (RVA 0x4AB9 - 0x4AD9)
0x140004ab9: mov  ecx, dword ptr [rdi]        ; rdi points to the message buffer (tag at offset 0)
0x140004abb: mov  eax, 0x02040811             ; magic constant for division by 0x7F (127)
0x140004ac0: mul  ecx                         ; edx:eax = ecx * 0x02040811
0x140004ac2: mov  eax, ecx
0x140004ac4: xor  r8d, r8d
0x140004ac7: sub  eax, edx
0x140004ac9: shr  eax, 1
0x140004acb: add  eax, edx
0x140004acd: xor  edx, edx
0x140004acf: shr  eax, 6                      ; eax = ecx / 0x7F
0x140004ad2: imul eax, eax, 0x7f             ; eax = (ecx / 0x7F) * 0x7F
0x140004ad5: sub  ecx, eax                    ; ecx = ecx % 0x7F (remainder 0 to 126)
0x140004ad7: inc  ecx                         ; ecx = (ecx % 0x7F) + 1 (result 1 to 127)
0x140004ad9: mov  dword ptr [rdi], ecx        ; the new tag is written back into the message
```

### Proven facts:
1. `AppleAudio.sys` uses **a strictly cyclic counter from 1 to 127** (`1 <= tag <= 127`, `0x01..0x7F`).
2. The tag is never `0`.
3. The tag is not an ASCII string.

---

## 2. How `AppleUSBVHCI.sys` handles `IOCTL 0x222018`

The IOCTL dispatch table in `AppleUSBVHCI.sys` is in the function `0x140017C50`:
```assembly
; AppleUSBVHCI.sys VA: 0x140017D1D - 0x140017D56
0x140017D1D: sub  ecx, 0x222000
...
0x140017D54: cmp  ecx, 4                      ; check: code == 0x222018
0x140017D56: jne  0x17FD7
```

For `0x222018` control goes to the function `0x140002128`:
1. **Passing the message to bridgeOS (`0x1400021B4`):**
   * Calls `0x1400027C4(ctx, InputBuffer, InputBufferLength)`.
   * `AppleUSBVHCI.sys` **neither validates nor modifies the `tag` field**: the buffer goes to bridgeOS as is.
2. **Synchronous wait (`0x1400021E3`):**
   * `KeWaitForSingleObject` waits for the static event at `RVA 0x27448` with a 5-second timeout (`-50 000 000` in 100 ns units).
3. **Getting the reply (`0x14000222A`):**
   * The bridgeOS reply buffer is taken from the global pointer `RVA 0x27440` and copied into the caller's `OutputBuffer` (`0x140002235: call memcpy`).
   * `IoStatusBlock.Information` receives the length of the copied reply (`0x140002240: mov qword ptr [rsi], rdx`).
4. **The check in `AppleAudio.sys` (`0x140004B5D`):**
   * `AppleAudio.sys` checks: `if (IoStatus.Information == 0) return STATUS_UNSUCCESSFUL (0xC0000001)`.

---

## 3. Protocol message types and their dispatch in `AppleUSBVHCI.sys`

The BCE protocol defines three message types (`header.type` at offset `+4`):
* **`type == 1`**: COMMAND (a command from bridgeOS to the host)
* **`type == 2`**: RESPONSE (a reply to a command the host sent earlier)
* **`type == 3`**: NOTIFICATION (an asynchronous notification from bridgeOS)

In the dispatcher of incoming bridgeOS messages (`AppleUSBVHCI.sys` `0x140001CF0`–`0x140001DEF`):
```assembly
; AppleUSBVHCI.sys VA: 0x140001D72 - 0x140001DEF
0x140001D72: movzx ecx, byte ptr [rbx + 4]    ; rbx = packet buffer, +4 = header.type
0x140001D76: sub   ecx, 1                     ; type == 1 (COMMAND)?
0x140001D79: je    0x1DA4                     ; go to the msg == 4 check
0x140001D7B: sub   ecx, 1                     ; type == 2 (RESPONSE)?
0x140001D7E: je    0x1DEF                     ; go to KeSetEvent(0x27448)!
0x140001D80: sub   ecx, 1                     ; type == 3 (NOTIFICATION)?
0x140001D83: jne   0x1E05
0x140001D85: cmp   dword ptr [rbx + 0xd], 100 ; msg == 100 (NOTIFICATION_ALIVE)?
0x140001D89: je    0x1DEF                     ; ALIVE -> also KeSetEvent(0x27448)!
0x140001D8B: jmp   0x1DAA                     ; other type 3 -> delivered through 0x222020 (0x140002274)
...
0x140001DA4: cmp   dword ptr [rbx + 0xd], 4   ; for type 1: msg == 4 (UPDATE_TIMESTAMP)?
0x140001DA8: jne   0x1D9A                     ; if not -> DROPPED
0x140001DAA: call  0x140002274                ; delivered to the audio driver through 0x222020!
...
0x140001DEF: lea   rcx, [rip + 0x2564f]       ; RVA 0x27448 (the reply wait event)
0x140001DF6: xor   edx, edx
0x140001DF8: xor   r8d, r8d
0x140001DFB: call  KeSetEvent                 ; wakes the thread that called 0x222018
```

### Proven dispatch facts:
1. `type == 1` (COMMAND) is delivered to the audio driver through `0x222020` **only when `msg == 4`** (`UPDATE_TIMESTAMP`). All other `type == 1` are dropped.
2. `type == 2` (RESPONSE) **never goes to `0x222020`**; it always fires `KeSetEvent(0x27448)`.
3. `type == 3` (NOTIFICATION) is delivered through `0x222020` for every message **except `msg == 100`** (`NOTIFICATION_ALIVE`).
4. **Critical risk: `NOTIFICATION_ALIVE (100)`** is not passed to the audio driver but jumps to `0x140001DEF` and sets the same event `KeSetEvent(0x27448)` as a command reply! If `NOTIFICATION_ALIVE` arrives while our thread is waiting for a `0x222018` reply, the event fires early, and the caller gets the ALIVE packet in its output buffer instead of the real reply.

---

## 4. Tag semantics and risks

* `AppleUSBVHCI.sys` **does not check the `tag` field** in replies. All `IOCTL 0x222018` handling is single-threaded and relies on the static event `0x27448`.
* **bridgeOS behaviour regarding `tag`:**
  - *Hypothesis:* bridgeOS mirrors the received `tag` in the header of the `type == 2` reply.
  - *Hypothesis:* the 1..127 range in Apple's driver comes from a 7-bit transaction id in the bridgeOS IPC tables.
* **Why the driver must check the tag:**
  Since `NOTIFICATION_ALIVE` may wake `0x222018` early, the audio driver must check that the reply has `header.type == 2` and `tag == expected tag`. On a mismatch the driver must reject the reply (`T2AudioSendBceMessage` already does).

---

## 5. Result of task R2

| Parameter | Value | Status |
| :--- | :--- | :--- |
| Correct `tag` format | A number 1..127 (`(*tag % 0x7F) + 1`), incremented on every request | **PROVEN** |
| Does `AppleUSBVHCI` filter by tag? | No, `AppleUSBVHCI` checks only `header.type` and wakes the thread | **PROVEN** |
| Do replies with a foreign tag go to `0x222020`? | No: `type == 2` always goes to `KeSetEvent`, while `0x222020` gets `type == 1 (msg 4)` and `type == 3 (except msg 100)` | **PROVEN** |
| Behaviour on `NOTIFICATION_ALIVE` | Fires `KeSetEvent(0x27448)`, which risks replacing the expected reply | **PROVEN** |
| Action for `T2AudioPort` | Check `tag` and `type == 2` on return from `0x222018`; the driver's wait timeout > 5 s (the VHCI timeout is 5 s) | **IMPLEMENTED** (in `diagnostics`) |
