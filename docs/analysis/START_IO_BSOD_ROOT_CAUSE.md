# START_IO BSOD Root Cause: IOCTL 0x222020 Handshake via AppleUSBVHCI

> **Date:** 2026-10-28  
> **Status:** Static analysis proven by disassembly of `AppleUSBVHCI.sys` 6.1.7500.5 and minidumps `100826-15859-01.dmp`, `100826-23546-01.dmp`. Runtime fix implemented in `Driver.c` / `BceTransport.c` and **proven on hardware** (Stage D, 1.0.7.0: START_IO without BSOD).  
> **Scope:** Resolves the `0x7E` bugcheck at `ks!DispatchDeviceIoControl+0x15` on `START_IO`.

---

## 1. Summary

In the tests of 2026-10-08 the `-Stage 4 -BceIo` run twice ended in BSOD `0x7E` (`STATUS_ACCESS_VIOLATION`) on the instruction:
```text
ks!DispatchDeviceIoControl+0x15:
mov rdx, qword ptr [rax+18h]    ; rax = CurrentStackLocation->FileObject == NULL -> read at 0x18
```
The call stack in the dump:
```text
ks!DispatchDeviceIoControl+0x15
nt!IofCallDriver+0x4b
ksthunk!CKernelFilterDevice::DispatchIrp+0xf4
ksthunk!CKernelFilterDevice::DispatchIrpBridge+0x13
nt!IofCallDriver+0x4b
AppleUSBVHCI+0x259a
```

The attempt to fix the crash by setting `FileObject` in the outgoing IRP of `T2AudioSendBceMessage` had no effect, because **the crashing IRP is not sent by our driver**. It is created by `AppleUSBVHCI.sys`.

---

## 2. The bridgeOS → host delivery mechanism through AppleUSBVHCI

Disassembly of `AppleUSBVHCI.sys` (version 6.1.7500.5, SHA256 `0438292090fc...55c1`):

1. **Receiving a packet from bridgeOS:** `AppleUSBVHCI` registers the BCE interrupt / message queue. On reception the incoming packet handler is called (`0x140001CF0`).
2. **Filtering message types (`0x140001D72`–`0x140001D9A`):**
   * `type == 1` (COMMAND): delivered to the audio driver through `0x140002274` **only when `msg == 4`** (`UPDATE_TIMESTAMP`), otherwise dropped.
   * `type == 2` (RESPONSE): not passed to the audio driver; it calls `KeSetEvent(0x27448)`, waking the synchronous `IOCTL 0x222018`.
   * `type == 3` (NOTIFICATION): delivered through `0x140002274` for every message **except `msg == 100`** (`NOTIFICATION_ALIVE`), which also wakes `KeSetEvent(0x27448)`.
3. **Finding the audio driver (`0x140002274`):**
   * `AppleUSBVHCI` calls `IoGetDeviceInterfaces(&KSCATEGORY_AUDIO, ...)` and looks for the first interface whose symbolic link contains the substring `"VEN_106B"`.
   * It opens the target `DEVICE_OBJECT` with `IoGetDeviceObjectPointer`.
4. **Building the IRP (`0x140002540`–`0x140002594`):**
   * Calls `IoBuildDeviceIoControlRequest(IoControlCode = 0x222020, TargetDeviceObject, InputBuffer = msg, ...)`.
   * **`IoBuildDeviceIoControlRequest` leaves `FileObject == NULL`.**
   * Sends the IRP with `IofCallDriver(TargetDeviceObject, Irp)`.
   * The return address is `AppleUSBVHCI+0x259a`.

---

## 3. Why the stock `AppleAudio.sys` does not crash

In `AppleAudio.sys` (RVA `0x1280`, `IRP_MJ_DEVICE_CONTROL`):
```assembly
0x140001280:
...
cmp eax, 0x222020
je  handle_vhci_message
...
; handle_vhci_message (0x1400012BF):
; msg == 4  -> 0x14000435C (stores UPDATE_TIMESTAMP)
; msg == 13 -> 0x1400044B8 (PROPERTY_CHANGED: jack status)
; completes the IRP with STATUS_SUCCESS without passing it to PortCls / KS
```
The stock driver **intercepts `0x222020` in its own `IRP_MJ_DEVICE_CONTROL` entry point** and completes it itself, never letting it into `ks.sys`.

---

## 4. Why `T2AudioPort` crashed

`T2AudioPort` initialised through PortCls (`PcInitializeAdapterDriver`). Every IRP, including `IRP_MJ_DEVICE_CONTROL`, went to `PcDispatchIrp` → `ks!DispatchDeviceIoControl`.  
Since `AppleUSBVHCI` built the IRP with `IoBuildDeviceIoControlRequest` and `FileObject == NULL`, `ks.sys` unconditionally dereferenced `NULL + 0x18`, causing BSOD `0x7E`.

---

## 5. Fix and open questions

1. **Intercepting `0x222020`:**
   `DriverEntry` (`src/Driver.c`) installs the hook `DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = T2AudioDeviceControl`.
   For code `0x222020` it calls `T2AudioHandleVhciMessage` (`src/BceTransport.c`) and completes the IRP with `STATUS_SUCCESS`.
   Allowed only for kernel-mode IRPs (`Irp->RequestorMode == KernelMode`).
2. **Status:** the root cause is confirmed by decompilation and kernel minidumps. The handler is **proven on real hardware** (Stage D, 2026-10-28, and every run since).
3. **Open:** handling `PROPERTY_CHANGED` (msg 13) for the headphone jack (see `docs/HEADPHONES.md`, step 2).
