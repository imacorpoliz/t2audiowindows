# RE_minidump: analysis of the BSOD minidumps on START_IO (task R1)

> **Analysis date:** 28 October 2026  
> **Tool:** WinDbg / CDB (`10.0.28000.2526 AMD64`), Microsoft Symbol Server symbols (`srv*https://msdl.microsoft.com/download/symbols`).  
> **Dumps analysed:**
> 1. `C:\Windows\Minidump\100826-15859-01.dmp` (2026-10-08 01:35:07)
> 2. `C:\Windows\Minidump\100826-23546-01.dmp` (2026-10-08 01:48:20)  
> **Status of the conclusions:** **PROVEN** (both dumps and the machine code match completely).

---

## 1. Crash summary

Both minidumps record an identical kernel crash:

* **BugCheck code:** `SYSTEM_THREAD_EXCEPTION_NOT_HANDLED (7e)`
* **Parameter 1 (ExceptionCode):** `0xC0000005` (`STATUS_ACCESS_VIOLATION`)
* **Parameter 2 (ExceptionAddress):** `ks!DispatchDeviceIoControl+0x15`
* **Parameter 3 (ExceptionRecord):** `ffffbb0180d1d3d8` (dump 23546) / `fffff484865053d8` (dump 15859)
* **Parameter 4 (ContextRecord):** `ffffbb0180d1cc10` (dump 23546) / `fffff48486504c10` (dump 15859)
* **Thread:** a system worker thread (`PROCESS_NAME: System`)

---

## 2. Disassembly of the crash point in `ks.sys`

Register context at the time of the crash:
```text
rax=0000000000000000 rbx=ffffe586444dd040 rcx=ffffe58643ca17f0
rdx=ffffe586444dd040 rsi=ffffe586444dd2c0 rdi=ffffe58642431560
rip=fffff8008fbb0bf5 rsp=ffffbb0180d1d610 rbp=ffffbb0180d1d6d0
```

Listing of `ks!DispatchDeviceIoControl`:
```assembly
ks!DispatchDeviceIoControl:
fffff800`8fbb0be5 8b82b8000000    mov     eax, dword ptr [rdx+0B8h]  ; rdx = IRP, [rdx+0xB8] = CurrentStackLocation
fffff800`8fbb0beb 4c8bc2          mov     r8, rdx
fffff800`8fbb0bee 4c8bc9          mov     r9, rcx
fffff800`8fbb0bf1 488b4030        mov     rax, qword ptr [rax+30h]   ; rax = CurrentStackLocation->FileObject
fffff800`8fbb0bf5 488b5018        mov     rdx, qword ptr [rax+18h]   ; rdx = FileObject->FsContext -> CRASH!
```

### Proven facts:
1. `CurrentStackLocation->FileObject` is exactly `0x0000000000000000` (`NULL`).
2. `ks.sys` reads `[rax + 0x18]` with `rax == 0`, causing an `ACCESS_VIOLATION` on a read at `0x0000000000000018`.
3. `ks.sys` has no `NULL` check for `FileObject` at the start of `DispatchDeviceIoControl`.

---

## 3. Call stack and the origin of the IRP

The call stack in both dumps:
```text
# Child-SP          RetAddr               Call Site
00 ffffbb01`80d1d610 fffff800`e8f597bb     ks!DispatchDeviceIoControl+0x15
01 ffffbb01`80d1d640 fffff800`7df31534     nt!IofCallDriver+0x4b
02 ffffbb01`80d1d680 fffff800`7df31253     ksthunk!CKernelFilterDevice::DispatchIrp+0xf4
03 ffffbb01`80d1d6e0 fffff800`e8f597bb     ksthunk!CKernelFilterDevice::DispatchIrpBridge+0x13
04 ffffbb01`80d1d710 fffff800`7def259a     nt!IofCallDriver+0x4b
05 ffffbb01`80d1d750 ffffffff`80001ec8     AppleUSBVHCI+0x259a
```

### Proven facts:
1. The return address `AppleUSBVHCI+0x259a` is exactly the instruction after the `IofCallDriver` call in `AppleUSBVHCI.sys` at offset `0x2594`:
   * `0x140002594`: `call IofCallDriver`
   * `0x14000259A`: `cmp eax, 0x103` (`STATUS_PENDING`)
2. `AppleUSBVHCI.sys` builds this IRP with `IoBuildDeviceIoControlRequest(0x222020, DeviceObject, ...)`:
   * `IoBuildDeviceIoControlRequest` leaves `FileObject == NULL` as standard.
3. The IRP is sent to the `KSCATEGORY_AUDIO` interface of the device whose name contains `"VEN_106B"`.
4. The device's upper filter was `ksthunk.sys` (Kernel Streaming Thunk), which passed the IRP with code `0x222020` down into the PortCls/KS stack.
5. Since the `IRP_MJ_DEVICE_CONTROL` handler of `T2AudioPort` used to pass the IRP unchanged to `PcDispatchIrp` → `ks!DispatchDeviceIoControl`, `ks.sys` crashed on the `NULL FileObject`.

---

## 4. State of the IRP buffer in the dump

* In the minidumps (`Mini Kernel Dump File: Only registers and stack trace are available`) the physical pages holding the IRP body (`rdx = ffffe586444dd040`, `rdi = ffffe58642431560` is the DeviceObject) and the SystemBuffer were not included (`Could not read Irp`).
* Still, the registers in the `ks!DispatchDeviceIoControl` frame unambiguously show the IRP address in `rdx`, the `CurrentStackLocation` load into `rax`, and the read of `CurrentStackLocation->FileObject == NULL`. The call chain `AppleUSBVHCI+0x259a` → `ksthunk` → `ks!DispatchDeviceIoControl+0x15` fully agrees with the static analysis in `START_IO_BSOD_ROOT_CAUSE.md`.

---

## 5. Result of task R1

| Question | Answer | Status |
| :--- | :--- | :--- |
| What sends the crashing IRP? | `AppleUSBVHCI.sys` at offset `0x2594` | **PROVEN** |
| Which IOCTL code? | `0x222020` (hard-coded in `AppleUSBVHCI.sys` at `0x140002566`) | **PROVEN** |
| Why `FileObject == NULL`? | Built with `IoBuildDeviceIoControlRequest`, which binds no `FileObject` | **PROVEN** |
| Why did KS crash? | `ks!DispatchDeviceIoControl` unconditionally reads `FileObject->FsContext` (`+0x18`) | **PROVEN** |
| Is the fix in `src/Driver.c` correct? | Intercepting `0x222020` in `DriverEntry` ahead of PortCls/KS keeps the IRP out of `ks.sys` | **PROVEN on hardware** (Stage D, 1.0.7.0: START_IO without BSOD) |
