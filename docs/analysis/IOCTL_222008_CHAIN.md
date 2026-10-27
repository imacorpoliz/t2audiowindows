# IOCTL 0x222008 -> 0x22200C Static Chain

Static/offline evidence for AppleUSBVHCI.sys 6.1.7500.5.

## Entry and dispatch

`0xDC5C` receives the handler context in `RBX = RCX`.

```text
0xDC65  add rcx, 0x1c
0xDC69  call 0x8A54
0xDC6E  mov rcx, [RVA 0x27488]       ; manager object
0xDC78  mov r9,  [rbx+0x08]
0xDC7C  mov r8,  [rbx+0x00]
0xDC82  mov rdx, [rcx]                ; manager vtable
0xDC86  mov rax, [rdx+0x28]           ; RVA 0xE310
0xDC8A  mov [rsp+0x20], rdx           ; fifth argument = context[+0x10]
0xDC8F  mov rdx, r10                  ; wrapper result
0xDC92  call [RVA 0x1E140]            ; GuardCFDispatchFunctionPointer
```

The source of the fifth argument is `0xDC8A`, which loads `[RBX+0x10]`.
`E310` saves `R9` at `[entry_rsp+0x20]` on `0xE31A`, then performs five pushes
and `sub rsp,0x60`, a total displacement of `0x88`. Therefore:

```text
[current_rsp+0xA8] = [entry_rsp+0x20] = saved R9 = context+0x08
[current_rsp+0xB0] = [entry_rsp+0x28] = fifth argument = context+0x10
```

`0xE390`/`0xE39B` load these values and `0xE39D` passes `EDX=R12d` to
`IoAllocateMdl`. Thus the address is `context+0x08` and the byte count is
`(DWORD)context+0x10`; `R8` at `DC7C` is `context+0x00`.

## Complete 0xE310 body

Function boundary from `.pdata`: `[0xE310, 0xE4D5)`.

```text
0xE35A  edx = 0x60
0xE35F  ecx = 0x200
0xE364  r8d = 'ABCE'
0xE36A  call ntoskrnl.exe!ExAllocatePoolWithTag
0xE381  [descriptor+0x10] = RBX
0xE390  r12 = [RSP+0xB0]
0xE39B  rcx = [RSP+0xA8]
0xE3A3  call ntoskrnl.exe!IoAllocateMdl
0xE3C3  call ntoskrnl.exe!MmProbeAndLockPages
0xE3D0  call 0x7A68
0xE3EA  rax = [entry.vtable+0x50]
0xE3F4  call [RVA 0x1E140]            ; target is entry.vtable[+0x50]
0xE402  [descriptor+0x18] = RAX
0xE40E  [descriptor+0x38] = [RSP+0xA8]
0xE412  [descriptor+0x40] = R12
0xE416  [descriptor+0x58] = RBX       ; MDL
0xE430  rax = [manager.vtable+0x58]
0xE445  call [RVA 0x1E140]            ; manager.vtable[+0x58] = 0xE4E0
```

Failure branches are also explicit: `0xE3DD` returns `STATUS_UNSUCCESSFUL`
after the entry method returns NULL; `0xE453` frees the MDL; `0xE481` frees
the descriptor; `0xE4A6` calls manager.vtable[+0x58] for cleanup when the
entry-side result exists.

## 0x22200C consumer

```text
0xDE48  call 0xE618                  ; find entry in manager list
0xDE67  rax = [entry.vtable+0x60]
0xDE6B  call [RVA 0x1E140]            ; entry query target
0xDE82  output[+0x00] = entry[+0x10]
0xDE8E  output[+0x08] = entry[+0x28]
0xDE9F  output[+0x10] = entry[+0x30]
0xDEB2  call ntoskrnl.exe!MmUnlockPages
0xDEBB  call ntoskrnl.exe!IoFreeMdl
0xDECB  call ntoskrnl.exe!ExFreePoolWithTag
```

`E4E0` is part of the chain. It preserves manager in `R14` from `E4FD` through
`E445` and uses `[R14->vtable+0x58]` again at `E4A6` for cleanup. The call
arguments at `E445` are manager=`R13`, entry-side result=`R14`, descriptor=`RDI`,
length=`R12`, and zero callback/context=`R9`.

The producer/consumer relation is therefore:
`0x222008 -> 0xDC5C -> 0xE310 -> IoAllocateMdl(address=context+8,
length=(DWORD)context+0x10) -> MmProbeAndLockPages -> 0x7A68 ->
entry.vtable[+0x50] -> manager.vtable[+0x58]=0xE4E0`.
`0x22200C -> 0xDE30 -> E618 lookup -> entry.vtable[+0x60] -> copy
entry/result +0x10/+0x28/+0x30 -> unlock/free MDL and ABCE descriptor`.

The entry-side dispatch sites at `E3F4`, `DE6B`, and `E581` are not manager
slots. Their vtable value is written by `E8F8`, `8A54`, and `7A68` as
VA `0x140002970`; PE RVA `0x2970` is `.text` (`mov al,1; ret`), not a pointer
table. Their concrete targets therefore remain unknown for this checked reason.
The queried `+0x28/+0x30` fields are read at `DE8E/DE9F`; no writer is present
in the reviewed bodies, so their producer is also unknown. The visible result
collection function is `DE30`, with semantic work below `entry.vtable[+0x60]`.

The entry belongs statically to the AppleUSBVHCI manager list. No named kaiT2en
queue identifier or audio identifier is present in this path; audio relevance
is not established.
