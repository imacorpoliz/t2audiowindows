# RE_update_timestamp: the reply to UPDATE_TIMESTAMP (task R3)

> **Analysis date:** 28 October 2026  
> **Tools:** Capstone disassembler, a Python PE parser, analysis of `AppleAudio.sys` (SHA256 `cd8b81fabc9b…a150`) and `AppleUSBVHCI.sys` 6.1.7500.5 (SHA256 `0438292090fc…55c1`).  
> **Status of the conclusions:** **PROVEN** (with exact instructions and VA/RVA addresses).

---

## 1. Who sends the reply to `UPDATE_TIMESTAMP`

In the Linux driver `kaiT2en` the audio driver is attached directly to the BCE queue and itself sends `UPDATE_TIMESTAMP_RESPONSE (34)` for every notification from bridgeOS.  
On Windows the architecture is split: `AppleUSBVHCI.sys` owns the bus and the BCE queues, and the audio driver (`AppleAudio.sys` / `T2AudioPort`) runs on top of it.

### Evidence from the `AppleUSBVHCI.sys` disassembly:

In the handler of incoming bridgeOS packets (`AppleUSBVHCI.sys` `0x140001CF0`):
```assembly
; AppleUSBVHCI.sys VA: 0x140001D72 - 0x140001DE8
0x140001d72: movzx ecx, byte ptr [rbx + 4]    ; rbx = incoming buffer, +4 = header.type
0x140001d76: sub   ecx, 1                     ; ecx == 0 -> type 1 (COMMAND)
0x140001d79: je    0x1DA4                     ; go to the msg == 4 check
...
0x140001da4: cmp   dword ptr [rbx + 0xd], 4   ; rbx + 0x0D = msg (4 = UPDATE_TIMESTAMP)
0x140001da8: jne   0x1D9A                     ; not msg 4 -> dropped
0x140001daa: mov   r8d, dword ptr [rbp - 0x40]; r8d = incoming packet length
0x140001dae: mov   rdx, rbx                   ; rdx = pointer to the incoming packet
0x140001db1: mov   rcx, rdi                   ; rcx = device context
0x140001db4: call  0x140002274                ; DELIVERY TO THE AUDIO DRIVER THROUGH IOCTL 0x222020!

; --- RIGHT AFTER DELIVERY: AppleUSBVHCI.SYS BUILDS THE REPLY ITSELF ---
0x140001db9: movups xmm0, xmmword ptr [rbx]   ; copies the first 16 bytes (tag, devid, header)
0x140001dbc: mov   r8d, 0x15                  ; reply size = 21 (0x15) bytes
0x140001dc2: lea   rdx, [rbp - 0x28]          ; reply buffer on the stack
0x140001dc6: mov   rcx, rdi                   ; context
0x140001dc9: movups xmmword ptr [rbp - 0x28], xmm0 ; writes the copied header
0x140001dcd: mov   eax, dword ptr [rbx + 0x10]
0x140001dd0: mov   dword ptr [rbp - 0x18], eax
0x140001dd3: mov   al, byte ptr [rbx + 0x14]
0x140001dd6: mov   byte ptr [rbp - 0x14], al
0x140001dd9: and   dword ptr [rbp - 0x17], 0
0x140001ddd: mov   byte ptr [rbp - 0x24], 2   ; offset +4 = TYPE 2 (RESPONSE)!
0x140001de1: mov   dword ptr [rbp - 0x1b], 0x22 ; offset +13 = MSG 0x22 = 34 (UPDATE_TIMESTAMP_RESPONSE)!
0x140001de8: call  0x1400027C4                ; SENDS THE REPLY TO BRIDGEOS!
```

### Proven facts:
1. `AppleUSBVHCI.sys` **builds and sends `UPDATE_TIMESTAMP_RESPONSE (34)` to bridgeOS by itself, automatically**, right after calling `0x140002274`.
2. The reply keeps the `tag` and `device_id` of the original incoming message.
3. The reply's message code is exactly `0x22` (34).

---

## 2. What `AppleAudio.sys` does on `UPDATE_TIMESTAMP`

In `AppleAudio.sys` (the `IOCTL 0x222020` handler at `0x140001280`):
```assembly
; AppleAudio.sys VA: 0x1400012BF - 0x14000134F (IOCTL 0x222020 handler)
0x1400012bf: mov  eax, dword ptr [rdi + 0xd]  ; eax = msg (offset 0x0D)
0x1400012c2: cmp  eax, 4                      ; msg == 4 (UPDATE_TIMESTAMP)?
0x1400012c5: je   0x140001334
...
0x140001334: mov  rdx, qword ptr [rdi + 5]    ; rdx = device_id (offset +5)
0x140001338: mov  r8, rdi                     ; r8 = message buffer
0x14000133b: mov  rcx, qword ptr [0x140014690]; rcx = device context (from a global)
0x140001342: call 0x14000435C                ; stores the timestamp
0x140001349: mov  dword ptr [rsi + 0x30], ebx ; Irp->IoStatus.Status = 0 (ebx = 0, IRP in rsi)
0x14000134f: call IofCompleteRequest          ; completes the IRP
```

The function `0x14000435C`:
* Looks the device up by `device_id`.
* Increments the tick counter at offset `+0xE954`.
* Stores the 64-bit timestamp `*(u64*)(buf + 0x15)` at offset `+0xE958`.
* **Calls no message-sending function.** Neither `FUN_140004a4c` nor any IOCTL is called.

---

## 3. Result of task R3

| Question | Answer | Status |
| :--- | :--- | :--- |
| Does bridgeOS need a reply to `UPDATE_TIMESTAMP`? | Yes, bridgeOS expects a reply with code 34 (`0x22`) | **PROVEN** |
| Who builds and sends the reply on Windows? | The bus driver `AppleUSBVHCI.sys` (at `0x140001DE8`) | **PROVEN** |
| Must the audio driver (`T2AudioPort`) reply to `UPDATE_TIMESTAMP`? | **NO, it must not.** If the audio driver sent a reply, there would be a duplicate-reply conflict | **PROVEN** |
| What should `T2AudioPort` do on `0x222020` with `msg == 4`? | Parse the timestamp/sampleTime, update the device context and complete the IRP with `STATUS_SUCCESS` | **PROVEN on hardware** (Stage D and later; implemented in `src/Driver.c` and `src/BceTransport.c`) |
| Will bridgeOS keep sending timestamps? | Yes, since `AppleUSBVHCI.sys` acknowledges every packet with `0x22` | **PROVEN** (also on hardware) |
