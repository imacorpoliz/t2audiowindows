# Reverse-engineering plan (for Gemini)

> The goal is to close the remaining questions about how `AppleAudio.sys` and `AppleUSBVHCI.sys` work with the T2. The answers decide sound through `START_IO`, the hardware clock, the jack and sleep.
> Read [`analysis/START_IO_BSOD_ROOT_CAUSE.md`](analysis/START_IO_BSOD_ROOT_CAUSE.md) first: it has the root cause of the BSOD and the addresses to start from.
> What is still open for full functionality is summarised in [`ROADMAP.md`](ROADMAP.md) (headphones, microphones, hardware volume, sleep, other models).

## 0. Inputs

| What | Where |
|---|---|
| `AppleUSBVHCI.sys` 6.1.7500.5 | private repository `imacorpoliz/mbp`: `AppleUSBVHCI-extracted/` (SHA256 `0438292090fc…55c1`) |
| `AppleAudio.sys` + `appleaudio.inf` | `imacorpoliz/mbp`: `MBP-Audio-driver/` (SHA256 `cd8b81fabc9b…a150`) |
| Earlier disassembly excerpts | `imacorpoliz/mbp`: `evidence/traces/` (36 files) |
| Earlier reports | this repository: `docs/analysis/*.md` |
| BSOD minidumps | on the test machine: `C:\Windows\Minidump\100826-15859-01.dmp`, `100826-23546-01.dmp` |
| Linux reference | [kaiT2en/KaiT2en-Fedora](https://github.com/kaiT2en/KaiT2en-Fedora) `modules/t2bce_audio` (`protocol_bce.c`, `audio.c`, `pcm.c`, `timing.h`) |

**The main tool is REA (Reverse Engineer Anything)**, the same framework used for the earlier reports (`analysis/AppleAudio_Reverse_Engineering_Report.md`), together with Ghidra 12.x. Run every task R1–R9 through REA: load the binary, go to the given addresses, get the decompilation, the call graph and the cross-references. Keep REA/Ghidra projects and exports only in the private `imacorpoliz/mbp` (e.g. `rea/<task>/`). Auxiliary tools: WinDbg with Microsoft symbols (`.symfix; .reload`), only for the minidumps (R1); `objdump -d -M intel` for quick address checks. Addresses below are VAs with ImageBase `0x140000000` (RVA = VA − `0x140000000`).

## 1. Rules

0. **No audio playback** unless the owner allows it. Reverse engineering is offline work with binaries and dumps and needs no sound. Do not run tones, players, `tools/Test-WasapiTone.ps1` or speaker checks. If a task needs a hardware run, it must be silent (see `GEMINI.md`, rule 0).
1. **Apple binaries, whole decompiled listings and Ghidra projects go only into the private `imacorpoliz/mbp`.** The public `t2audiowindows` gets only reports: addresses, short (≤ 15 lines) instruction excerpts, conclusions.
2. Mark every conclusion as **proven** (instructions/dump), **probable** (indirect) or **hypothesis**. Give addresses.
3. One report per task: `docs/analysis/RE_<topic>.md`, in English. Put a summary of changes into `analysis/START_IO_BSOD_ROOT_CAUSE.md` §5 (open questions) and into `CURRENT_STATE.md`.
4. Change the driver code after reverse engineering in a separate PR: a pure parser in `src/BceProtocolLogic.h` + a test, then the kernel code. Enable nothing on hardware without the stages of `GEMINI.md`.

---

## 2. Tasks (by priority)

> **Status (2026-10-28):** R1–R3: PR imacorpoliz/t2audiowindows#1 (Gemini, with review remarks). R4, R5, R8, R9, msg 28: [`analysis/RE_appleaudio_protocol.md`](analysis/RE_appleaudio_protocol.md). R6 (clock) and R7 (speaker layout): [`analysis/RE_speaker_layout_and_clock.md`](analysis/RE_speaker_layout_and_clock.md), already in the code (stereo endpoint, tweeter HPF, correction from `UPDATE_TIMESTAMP`). Left (does not block sound): whether `SET_PROPERTY 'nsrt'` is needed before START_IO; the properties `'pfta'`/`'ring'`/`'sact'`/`'term'`/`'lcdr'`/`'lcdv'`; channel numbers in AppleAudio's copy loop (static confirmation of the layout); msg 28 (`controls`) value ranges for hardware volume; the input-stream (microphone) path; the headphone stream registration. `IAdapterPowerManagement` is already in the code (`src/Power.c`).

### R1. Confirm the BSOD cause from the minidumps (first)

Hypothesis: AppleUSBVHCI delivers a bridgeOS message through `IOCTL 0x222020` into our stack, and KS dereferences a `NULL` FileObject.

```
windbg -z C:\Windows\Minidump\100826-23546-01.dmp
.symfix; .reload
!analyze -v
kv                                  ; find the AppleUSBVHCI+0x259a frame
.frame /r <frame number of ks!DispatchDeviceIoControl>
; the IRP is the second argument (rdx) of ks!DispatchDeviceIoControl
!irp <IRP address>                  ; expected: IRP_MJ_DEVICE_CONTROL, IoControlCode 0x222020, FileObject 0
!devstack <DeviceObject from the IRP>   ; expected: ksthunk over T2AudioMiniport over PCI
db <SystemBuffer> L40               ; expected: msg (offset 0x0D) == 4 (UPDATE_TIMESTAMP)
```

The minidump may lack the pages with the IRP or the buffer. Record what you could see and what you could not.

**Done when:** `RE_minidump.md` gives the IoControlCode, `FileObject`, the device stack and (if available) `msg` from the buffer, or states explicitly that the dump does not contain them.

### R2. Semantics of the `tag` field (first 4 bytes of the message)

We used to send `"Audt"`; AppleAudio sends a counter 1..127 (`FUN_140004a4c`: `*tag = *tag % 0x7F + 1`), kaiT2en `'S'` + 3 digits.

1. In AppleUSBVHCI find the `IOCTL 0x222018` handler: search for `cmp …, 0x222018` / `0x222018` and the dispatch table from `AppleUSBVHCI_IOCTL_COMPLETE_ANALYSIS.md`.
2. Trace how the first 4 bytes of the input buffer are used: an index into the pending-request table? a key for matching the reply? Is there a 1..127 (≤ `0x7F`) limit?
3. Find **all callers** of the delivery function `0x1400023F0` (`IoGetDeviceInterfaces` → `0x222020`). Under which conditions does a message go there: only notifications/commands from bridgeOS (type 1/3), or also replies not matched to a pending request?
4. What does AppleUSBVHCI return in the `0x222018` output buffer, and with what `Information`?

**Result:** whether we must send 1..127 instead of `"Audt"`, and what the old value risks. (Done: the driver now sends 1..127.)

### R3. Is a reply to `UPDATE_TIMESTAMP` needed

kaiT2en replies with `UPDATE_TIMESTAMP_RESPONSE` (msg 34) with the same tag. AppleAudio (`0x14000435C`) only stores the timestamp and sends nothing.

1. In AppleUSBVHCI near and after the call to `0x1400023F0`: does it build the reply itself (constant `0x22` = 34 in the msg field at offset 13)?
2. In AppleAudio: is msg = 34 written anywhere / passed to `FUN_140004a4c`?
3. If nobody sends a reply, record that. Then check on hardware whether the timestamps keep arriving (the `VhciTimestampCount` counter in the log).

(Done: AppleUSBVHCI replies itself, `RE_update_timestamp.md`; timestamps keep arriving on hardware.)

### R4. `PROPERTY_CHANGED` (msg 13) format and the jack

AppleAudio `0x1400012CC`–`0x140001332`: compares `*(u32*)(buf+0x21)` with `'outp'`/`'inpt'`, flags `[g+0x38]`/`[g+0x39]`, calls `[0x14000E250]` (identify the import, probably `KeSetEvent`).

1. Match the offsets with `t2audio_msg_read_property_changed` in kaiT2en (devid, obj, scope/selector/element).
2. What is signalled, and who waits for the event (headphones → endpoint switch)?

**Result:** the message layout for `src/BceProtocolLogic.h` and a description of the jack logic.

### R5. AppleAudio's initialisation sequence before the first `START_IO`

1. List **all** calls of `FUN_140004a4c` (the `0x222018` send) in execution order from `DriverEntry`/StartDevice/stream creation: msg code, device_id, payload.
2. In particular: is there `NOTIFICATION_ALIVE` (100), `SET_REMOTE_ACCESS` (32, with which mode), `GET_INPUT/OUTPUT_STREAM_LIST`, `SET_PROPERTY` (9), `PROPERTY_LISTENER` (11), `SET_INPUT_STREAM_ADDRESS_RANGES` (18)?
3. Compare with our sequence (`DRIVER_GUIDE.md` §5.3) and kaiT2en (`t2audio_init_cmd`, `t2audio_init_dev`).

**Result:** a table "AppleAudio / kaiT2en / us" and a list of what we lack.

### R6. Clock and position in AppleAudio

AppleAudio stores the timestamp in the device context (`+0xE958`, counter `+0xE954`).

1. Find the reads of `+0xE958`/`+0xE954`: how is the WaveRT position (`GetPosition` / position register) computed from them?
2. Match with kaiT2en `t2audio_handle_timestamp` / `t2audio_position_frames` (sampleTime, hostTime, latency).

**Result:** a clock model to replace the QPC extrapolation in `T2AudioStreamAdvanceLocked`. (Done: lead model 1000 ± 500, in the code and proven on hardware.)

### R7. Sample format and copying in AppleAudio

We shift 32-bit samples right by 8 bits (S24_LE, per `'pft '`). Confirm from AppleAudio:

1. Find the loop that writes PCM into the BAR0 ring (or registers its own buffer in `BufferStruct`: `FUN_1400046f8`, `FUN_140001fa4`).
2. Is there `sar …, 8` / masking / channel reordering? Which format does AppleAudio declare to Windows (stereo PCM16), and how does it spread it over the 6 T2 channels?

**Result:** confirmation or refutation of `>> 8` and the **channel order**, i.e. which channel is which speaker. (The `>> 8` and the layout are consistent with hardware listening; the static confirmation is still open.)

### R8. Power

1. The `IRP_MJ_POWER` / `IAdapterPowerManagement` handlers in AppleAudio: what happens on the transition to D3 and back? Is the sleep marker `0x19770525` written into GPR `+0x10`, `SET_REMOTE_ACCESS(OFF/ON)`, are the buffers re-registered?
2. Compare with kaiT2en `t2audio_quiesce`, `t2audio_suspend`, `t2audio_reset_buffers`.

### R9. Choice of the `0x222020` receiver and coexistence

1. The arguments of `IoGetDeviceInterfaces` at `0x140002477` (flags: enabled interfaces only?).
2. Does AppleUSBVHCI enumerate the interfaces on **every** message or cache the pointer? (Matters for the `UPDATE_TIMESTAMP` rate.)
3. What happens if both our interface and AppleAudio's exist in the system?

---

## 3. After reverse engineering: hardware check (only with the owner's approval)

(Historical: done in Stage D.) The R1 fix is in the code (`T2AudioDeviceControl` in `src/Driver.c`). The agent `tools/T2Audio-TestAgent.ps1` accepts `-BceIo` only together with `-Mmio` and the owner flag `-OwnerApprovedBceIo`. A run is allowed only when all of the following hold:

1. R1 confirms the hypothesis (or the dump at least does not contradict it).
2. R2 and R3 found no additional mandatory step (tag change, timestamp reply). If they did, it is implemented and covered by tests.
3. Stages 1–3 and stage 5 without `-BceIo` passed per `GEMINI.md`.
4. **The project owner explicitly allowed** the `-Mmio -BceIo -OwnerApprovedBceIo` run (`-Mmio` is mandatory: without it the ring is not zeroed, and after START_IO the T2 would play the buffer's leftovers).

The run is **silent**: Mute and volume 0, no tone or players. Checking the BSOD cause needs no sound; it is enough that `START_IO` succeeds and timestamps arrive.

Expected in the log at the first `START_IO`:

```
T2Audio: START_IO sent to device 0x39
T2Audio: VHCI UPDATE_TIMESTAMP #1 dev=0x39 t2=… seed=… sample=…
T2Audio: T2 clock: re-aimed lead by … frames (#1, …)   # a few times at the start is acceptable
```

Expected: timestamps about every 347 ms (one ring revolution, 16 640 frames), `sample` growing by the same step each time (record it: that is `timestampPeriod`),

and **no** BSOD. After the run: `-Mode Recover`, check AppleAudio, report in `docs/logs/EXCERPT_*.md` and `CURRENT_STATE.md`.
