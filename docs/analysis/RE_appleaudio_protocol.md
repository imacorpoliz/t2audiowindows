# RE: the AppleAudio ↔ T2 protocol and the role of AppleUSBVHCI (R4, R5, part of R7, R9)

Static analysis, 2026-10-28, `objdump -d -M intel` + import analysis. The binaries are the same as in [`START_IO_BSOD_ROOT_CAUSE.md`](START_IO_BSOD_ROOT_CAUSE.md). VAs with ImageBase `0x140000000`. Labels: **proven** (instructions), **probable**, **hypothesis**.

## 1. AppleUSBVHCI runs the handshake with bridgeOS itself — **proven**

| VA | What |
|---|---|
| `0x140002690` | `NOTIFICATION_ALIVE`: tag 1, type 3, device_id 0, `msg 0x64` (100), payload `proto_ver = 1`, `msg_ver = 3`, length `0x1D`. Waits on event `0x27448` for 5 s; the reply must have `msg 0x64` |
| `0x140002864` | `SET_REMOTE_ACCESS`: tag 2, type 1, device_id 0, `msg 0x20` (32), payload `mode` = the second argument, length `0x1D`. The reply must have `msg 0x21` and `status 0` |
| `0x1400020F2` → `0x140002101` | at initialisation: ALIVE, then `SET_REMOTE_ACCESS(r14)`, where `r14d = 1` (`0x140001F68`) |
| `0x140016EA0` → `0x140016EB1` | after `KeDelayExecutionThread(100 ms)` (probably on return from sleep): ALIVE, then `SET_REMOTE_ACCESS(1)` |
| `0x140016F8F`–`0x140016FAA` | power state change handler (states 5–6): `SET_REMOTE_ACCESS(0)` |

Conclusions:
* On Windows remote access is turned on by **AppleUSBVHCI with `mode = 1`**, not 2 as in kaiT2en on Linux.
* **AppleAudio.sys never sends `SET_REMOTE_ACCESS` or `NOTIFICATION_ALIVE`** (see §2).
* Consequence for T2AudioPort (fixed in the code): the driver no longer sends `SET_REMOTE_ACCESS(2)` at stage 4 nor `OFF` on device stop/removal. An `OFF` from the audio driver would leave the T2 off until the next VHCI initialisation, and AppleAudio silent after a rollback. `T2AudioStartIo` no longer requires remote access.

## 2. Everything AppleAudio sends to the T2 (R5) — **proven**

All 30 calls of the send function `0x140004A4C` (`IOCTL 0x222018`). The message type (`+4`) is always 1 (COMMAND); the tag is the context counter (`mov eax,[rcx]; inc; mov [rcx],eax`, then `% 0x7F + 1` inside `0x140004A4C`).

| Function | msg | Length | Purpose |
|---|---|---|---|
| `0x1400027D0` | 101 `GET_DEVICE_LIST` | 0x15 | device list |
| `0x1400029D4` | 24 `GET_INPUT_STREAM_LIST` | 0x15 | input streams |
| `0x140002B40` | 26 `GET_OUTPUT_STREAM_LIST` | 0x15 | output streams |
| 21 functions `0x140002C94`…`0x140004054` | 7 `GET_PROPERTY` | 0x31 | see the table below; reply: `msg == 8`, `status == 0`, value from `+0x31` |
| `0x14000251C` | **28** (not in kaiT2en) | 0x15 | list of controls, see §8 |
| `0x1400042C4` | 11 `PROPERTY_LISTENER` | 0x29 | subscription to changes (jack) |
| `0x1400044E4` | 18 `SET_INPUT_STREAM_ADDRESS_RANGES` | — | registering the recording buffer (microphone) |
| `0x1400045FC` | 9 `SET_PROPERTY` | 0x42 | **only `'nsrt'`** (nominal rate): the value is a double from `cvtsi2sd` (`0x140004672`); reply `msg 10` |
| `0x1400048B0` | 0 `START_IO` | 0x15 | reply `msg 1` |
| `0x140004970` | 2 `STOP_IO` | 0x15 | reply `msg 3` |

`GET_PROPERTY` properties (element = 0 everywhere; the addresses are constants in `.rdata`, e.g. `0x14000EC98`/`0x14000ECA0`):

| scope | selectors |
|---|---|
| `'glob'` | `'uid '`, `'lnam'`, `'clas'`, `'bcls'`, `'bcvl'`, `'scci'`, `'lcdr'`, `'lcdv'`, `'nsr#'`, `'nsrt'`, `'ring'`, `'pfta'`, `'sact'`, `'term'`, `'ltnc'` |
| `'outp'` | `'jack'`, `'ltnc'`, `'saft'` |
| `'inpt'` | `'jack'`, `'ltnc'`, `'saft'` |

Notably: AppleAudio reads `'pfta'` (the list of **available** physical formats) rather than `'pft '`, plus `'nsrt'`/`'nsr#'` (the rate and the number of available rates) and `'ring'` (**probably** the ring size). It does not switch formats: the only `SET_PROPERTY` is the rate.

Comparison with T2AudioPort:

| Step | AppleAudio | T2AudioPort |
|---|---|---|
| ALIVE / SET_REMOTE_ACCESS | does not send (the VHCI does) | **no longer sends** (fixed) |
| GET_DEVICE_LIST, `'uid '` | yes | yes |
| GET_OUTPUT_STREAM_LIST | yes | yes |
| format | `'pfta'` + `SET_PROPERTY 'nsrt'` | `'pft '` (the current format), does not set the rate |
| latency | `'ltnc'`, `'saft'` | yes |
| START_IO / STOP_IO | yes | yes |

Open: whether `SET_PROPERTY 'nsrt' = 48000` is needed before the first `START_IO`. If the T2 is already at 48 kHz (as `'pft '` shows in NOTES.md), **probably** not. (Sound works on hardware without it.)

## 3. Sample format in the ring (part of R7) — **proven**

`0x14000B04E`–`0x14000B078` (one variant of the copy loop):

```
movsx  r8d, WORD PTR [rdi] / [rdi+2]   ; 16-bit L / R sample from the Windows buffer
mov    rax, [rsi+0x158]                ; the stream's WAVEFORMATEX
movzx  edx, WORD PTR [rax+0xe]         ; wBitsPerSample (16)
mov    rax, [rsi+0xa30]
mov    ecx, [rax+0x71d8]               ; device channel bit depth (24)
sub    ecx, edx                        ; 24 - 16 = 8
shl    r8d, cl                         ; sample << 8
mov    [r15], r8d                      ; 32-bit write into the ring
```

24 significant bits in the **low** bytes of a 32-bit container (S24_LE): the same layout our driver gets by shifting a 32-bit Windows sample `>> 8`.

Along the way: before writing, AppleAudio processes the samples on the CPU: doubles, division by full scale, clamping (`minsd`/`comisd`), calls to `0x14000779C`, `0x140007454`, `0x14000A19C`. The filter set is chosen by the global `0x140014688` (values 6 and 10, **probably** the Mac model). The channel disable mask is `0x14001468C`. **Probably** this is the per-speaker equalisation that CoreAudio DSP does on macOS: on Windows it lives in AppleAudio, not in the T2 firmware. Our driver did not do it at the time, so the tweeters would get the full range; first probes at low volume only. (Since then the in-driver DSP does the equivalent from the kaiT2en profile.)

Not analysed: the stereo → 6 speaker channel layout and the filter coefficients themselves. The loop shown writes 2 channels (`ebx` 0..1); the 6-channel variant has to be found among the neighbouring branches `0x14000A715`–`0x14000B2C0`.

## 4. `PROPERTY_CHANGED` and the jack (R4) — **proven**, layout **probable**

AppleAudio's `0x222020` handler (`0x1400012C7`–`0x140001332`):
* `scope = *(u32*)(buf+0x21)`: `'outp'` → flag `[g+0x38] = !flag`, then a call `[g+0x08]([g+0x10])` through CFG; `'inpt'` → `[g+0x39]`, `[g+0x18]([g+0x20])`. `g = [0x140014690]`.
* The property value in the message is **not read**: the jack state is simply toggled on each notification. AppleAudio gets the actual value with a separate `GET_PROPERTY 'jack'` (`0x140003224` for `'inpt'`, `0x14000330C` for `'outp'`).
* Layout (per kaiT2en `t2audio_msg_read_property_changed`): `+0x15 obj (u64)`, `+0x1D element`, `+0x21 scope`, `+0x25 selector`. The scope offset `+0x21` matches, so the rest is **probably** right too.

## 5. Choice of the `0x222020` receiver (R9) — **proven**

`0x140002461`–`0x140002477`: `IoGetDeviceInterfaces(&KSCATEGORY_AUDIO, NULL, 0, &list)`. Flags = 0: only **enabled** interfaces. The function `0x1400023F0` runs again **for every message** (no caching) in a worker thread: it is queued by `0x140002274` (`lea rax, 0x1400023F0` at `0x1400022DD`). So our handler is called at `PASSIVE_LEVEL`, and only one of the drivers (ours or AppleAudio) is installed at a time, so there is no ambiguity in choosing the receiver.

## 6. Classification of incoming messages in AppleUSBVHCI — **proven**

`0x140001D72`–`0x140001DFB`:

| type | msg | VHCI action |
|---|---|---|
| 1 (COMMAND) | 4 `UPDATE_TIMESTAMP` | delivered through `0x222020`, then the VHCI replies `UPDATE_TIMESTAMP_RESPONSE` (34) itself |
| 1 | others | dropped |
| 2 (RESPONSE) | any | `KeSetEvent(0x27448)`: wakes the waiting `0x222018` |
| 3 (NOTIFICATION) | 100 `ALIVE` | also `KeSetEvent(0x27448)` (as if it were a reply!) |
| 3 | others (`PROPERTY_CHANGED` etc.) | delivered through `0x222020` |

That is why T2AudioPort now checks that the reply tag matches the request tag: an ALIVE arriving during our request is not taken for the reply.

## 7. Power (R8) — **proven**

`PcRegisterAdapterPowerManagement` (`0x14001663F`) registers an object with vtable `0x14000EBD8`: `PowerChangeState` = `0x140001520`, `QueryPowerChangeState` = `0x140001610`, `QueryDeviceCapabilities` = `0x1400015D0` (returns 0). Both methods first forward the call to child objects (`[node+0x228]`, slot `+0x18`), then:

| Event | VA | Action |
|---|---|---|
| `QueryPowerChangeState(D3)` | `0x140001667` → `0x1400041EC` | "re-initialisation needed" flag `[g+0x3A] = 1`; **STOP_IO** to every device with active I/O (`[dev+0xE998]`), timestamp counter `[dev+0xE99C] = 0` |
| `PowerChangeState(D1…D3)` | `0x140001570` → `0x140004250` | the same STOP_IO (if not stopped yet), without the flag |
| `PowerChangeState(D0)` with the flag | `0x14000158E` → `0x140004BA0` | `KeDelayExecutionThread(1 s)`, then `0x140004398`: for every **input** stream it rewrites the ranges of its buffers into the T2 buffer table (`0x1400046F8`: signature `0x19870423`, version ≥ 2, entries by UID with stride `0xBDEC`) and sends `SET_INPUT_STREAM_ADDRESS_RANGES` (msg 18); the flag is cleared |

What is **absent**: writing the sleep marker `0x19770525` (the constant is not in the binary), `SET_REMOTE_ACCESS` (handled by the VHCI, §1), re-reading the output ring description. The speaker ring apparently stays the same after sleep; START_IO comes again from `SetState(RUN)` when the audio engine restarts the stream.

**Done** for T2AudioPort (`src/Power.c`, not verified on hardware): a static `IAdapterPowerManagement` is registered in `T2AudioStartDevice`.
* STOP_IO in `QueryPowerChangeState(D3)` (while the VHCI is still alive) and in `PowerChangeState(D1…D3)`.
* In D1–D3 the copy does not write into BAR0 (`PoweredDown`).
* On D0 START_IO is not repeated; as in AppleAudio, it comes from the next `SetState(RUN)`.
* The `HardwareIoActive` flag (interlocked) guarantees one STOP_IO per START_IO for any order of "stream leaves RUN / power".

Open: if the audio engine keeps the stream in RUN throughout sleep, there will be silence after wake until the stream is restarted.

Along the way: `0x1400046F8` shows that for **input** (microphone) AppleAudio allocates the buffers itself and writes their addresses into the shared table. Output (speakers) uses the ring provided by the T2 (BAR), as we do.

## 8. msg 28: the list of controls — **proven** (purpose **probable**)

`0x14000251C`: COMMAND msg 28 (`0x1C`) to `device_id`, length `0x15`. The reply must have `msg 29` (`0x1D`) and `status 0`. Then:

* the `u64` at `+0x15` is the number of objects (stored in `[dev+0xE178]`);
* from `+0x1D`, an array of `u64` object ids.

For each object two `GET_PROPERTY` calls are made (`0x140002E6C`, `0x140002C94`), and the class obtained is compared with `'levl'` (level/volume), `'slct'` (selector) and `'togl'` (toggle). Each class has its own set of reads (`0x140003138`/`0x140003044`, `0x140002F58`, `0x140002D80`), then the subclass `'jack'` is checked. This is the counterpart of `GET_OUTPUT_STREAM_LIST`, but for controls (volume, mute, output selection, headphone detector).

Speaker output does **not need** it: kaiT2en plays without msg 28, and volume on Windows can be done in software (in the audio engine). It will be useful later for hardware volume and the headphone detector.
