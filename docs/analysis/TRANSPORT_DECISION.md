# Windows 6ch Transport Decision

## Scope and evidence

This stage is static/offline only. No driver was installed, no MMIO was
written, and no START/STOP command or hardware playback was run. The native
probe report is a saved text report; it is not a capture of the complete wire
protocol.

Confirmed for the target:

- `Speaker` device `0x39`, one output stream `0x3A`.
- 48000 Hz, six channels, 24 valid bits, 24 bytes/frame, interleaved, signed
  flag `0x4`. The report does not explicitly state the container width.
- The saved report used `AppleUSBVHCI` IOCTL `0x222018` successfully for
  device/stream/property reads only.
- The public Windows KS probe advertises/accepts only stereo PCM16 for the
  speaker pin. Six-channel validation attempts returned Win32 1169.

## KaiT2en lifecycle and ownership

The local checkout has no `.git`. It is therefore identified by the manifest
and the separate upstream comparison, not by an assumed local commit.

### PCI, BAR and BCE owner

`modules/t2bce_audio/audio.c:t2audio_probe` enables the PCI function, requests
all PCI regions, sets bus mastering, obtains the BCE client, and maps BAR0 and
BAR4. BAR4 plus `0xC000` is the GPR area. `t2bce_audio` is the audio client,
but the shared BCE core owns the transport queues and resume coordination.

`audio.c:t2audio_init_bs` reads GPR version/signature/BufferStruct offset and
locates the shared BufferStruct in BAR0. This is resource ownership, not an
ALSA abstraction.

### Required command and notification sequence

- Prepare: `audio.c:t2audio_init_cmd` sends ALIVE, waits for the remote alive
  notification, then enumerates devices. `audio.c:t2audio_init_dev` reads UID,
  latency, input/output stream lists and physical format. `audio.c:t2audio_init_bs`
  matches BufferStruct devices by UID and enables remote access.
- Playback prepare: `pcm.c:t2audio_pcm_prepare` flushes prior I/O, resets
  timing and clears the mapped ring. `pcm.c:t2audio_pcm_hw_params` exposes the
  already-owned buffer to ALSA.
- Start: `pcm.c:t2audio_pcm_start_sync` publishes prefilled samples, marks the
  stream started, then sends START_IO and expects START_IO_RESPONSE.
- Position: the BCE input queue in `protocol_bce.c:t2audio_bce_in_queue_completion`
  receives asynchronous messages. `audio.c:t2audio_handle_command` dispatches
  UPDATE_TIMESTAMP; `pcm.c:t2audio_handle_timestamp` anchors sample time and
  `pcm.c:t2audio_pcm_pointer` interpolates position using host monotonic time.
- Stop: `pcm.c:t2audio_pcm_stop_sync` clears the started state and sends STOP_IO,
  expecting STOP_IO_RESPONSE. `pcm.c:t2audio_pcm_quiesce_subdevice` serializes
  this during teardown.
- Release: `audio.c:t2audio_free_dev` frees host capture DMA and metadata; T2
  output mapping descriptors are freed without freeing the T2-owned BAR memory.
  `audio.c:t2audio_remove` unmaps BARs, destroys queues/card, releases PCI
  regions and drops the BCE client.

Other protocol operations needed for setup are GET/SET_PROPERTY,
PROPERTY_LISTENER, GET_INPUT/OUTPUT_STREAM_LIST and SET_REMOTE_ACCESS. The
async notifications are ALIVE, PROPERTY_CHANGED, UPDATE_TIMESTAMP and BOOT;
the dispatch path is `protocol_bce.c:t2audio_deferred_msg_work` followed by
`audio.c:t2audio_handle_notification` or `audio.c:t2audio_handle_command`.

### Ring ownership and errors

For playback, `audio.c:t2audio_init_bs_stream` reads T2-provided output buffer
addresses from BufferStruct and maps them as BAR I/O memory. The T2 side
therefore supplies the playback ring; Windows/Linux host audio exposes that
mapping. Host position updates come from T2 timestamp notifications, not from
the host guessing callback cadence. Capture is different:
`audio.c:t2audio_init_bs_stream_host` allocates coherent host DMA, writes its
address into BufferStruct, issues SET_INPUT_STREAM_ADDRESS_RANGES and owns the
free path.

`pcm.c:t2audio_pcm_io_work` serializes requested START/STOP state and records
an unknown state on command failure. `protocol_bce.c:__t2audio_send_cmd_sync`
removes timed-out waiters so late replies cannot complete a reused waiter.

Suspend calls `audio.c:t2audio_quiesce`, stops I/O while BCE remains available,
flushes the workqueue and sends SET_REMOTE_ACCESS(OFF). Resume re-enables PCI;
stateful resume waits for `t2audio_resume_complete` before enabling access,
while no-state resume enables it directly, then resets stream timing. BOOT and
transport failure handling must prevent stale rings/notifications from being
accepted.

## AppleUSBVHCI IOCTL Analysis Results

### IOCTLs 0x222004, 0x222008, 0x22200C, 0x222010, 0x222014

**Complete static analysis performed** (see `AppleUSBVHCI_IOCTL_COMPLETE_ANALYSIS.md`):

All five IOCTLs dispatch through a runtime-initialized global object (RVA 0x27488)
with vtable at 0x23F78. Disassembly of all handlers and vtable functions confirms:

**Proven pattern**: descriptor management with a doubly-linked list at object[+0x20],
entry links at +0x78, pool allocation tagged 'ABCE', and MDL preparation/cleanup.
The 0x60-byte allocation is only a descriptor; its address/size fields can refer to
a larger range. MDL use does not prove user-mode origin, DMA purpose, or absence of
a resource mapping.

**Not established in the visible slice**:
- Direct PCI BAR mapping is not visible in these functions.
- Large buffer mapping cannot be excluded because the small object is a descriptor.
- Async completion/callback behavior cannot be excluded below CFG dispatch and deeper vtable calls.
- Absence of event APIs in upper handlers does not classify the operation.

**Critical finding**: PE Load Config identifies VA `0x14001E140` / RVA `0x1E140`
as `GuardCFDispatchFunctionPointer`. It is a CFG dispatch slot, not executable code.
Each indirect call through it dispatches to the target currently in `RAX`; provenance
is recorded in `vtable_func_0xE310_full.txt` and related evidence files.

**Conclusion**: The five IOCTLs visibly manage entries and a locked memory-range
descriptor. For `0x222008`, the address is `context+0x08`, the length is
`(DWORD)context+0x10`, and the manager dispatch at `0xE445` resolves to
`0xE4E0`; `E4E0` preserves the manager through operation and cleanup.
`0x22200C` is the visible result-collection path: it calls entry `+0x60`,
copies `+0x10/+0x28/+0x30`, and releases the MDL and descriptor. The entry-side
method bodies and writers of `+0x28/+0x30` remain unresolved for the checked
vtable/write reasons recorded in raw evidence. No named kaiT2en queue or audio
identifier is present, so audio relevance is not established.

### A. Keep AppleUSBVHCI, replace only audio

**Confirmed capabilities:** IOCTL `0x222018` is a serialized synchronous
message path used by AppleAudio for T2 control messages. Existing probe code
has confirmed GET_DEVICE_LIST (101), GET_OUTPUT_STREAM_LIST (26) and
GET_PROPERTY (7). The KaiT2en protocol identifies the control commands needed
for property discovery, remote access, START/STOP, stream lists and timestamp
acknowledgement.

**Remaining unknowns:** IOCTLs 0x222004-0x222014 have a proven descriptor/MDL path.
The manager method at `0xE445` is resolved to `0xE4E0`. Entry methods at
`E3F4`, `DE6B`, and `E581` remain unresolved because their written vtable value
is VA `0x140002970`, whose RVA `0x2970` is `.text` (`mov al,1; ret`), not a
pointer table. Their relation to buffer mapping and audio notifications is not
established.

AppleAudio.sys does not reference IOCTLs 0x222004-0x222014 in reviewed binary.
The static AppleAudio mapping/allocation paths show internal MDL/BAR-related
objects, but their ownership and connection to a WaveRT ring are not a supported
contract.

**Missing facts blocking Option A**:
1. How do async T2 notifications (ALIVE, UPDATE_TIMESTAMP, PROPERTY_CHANGED) reach AppleAudio.sys?
2. Who owns the playback BAR mapping (T2 BufferStruct at BAR0+offset)?
3. How does the WaveRT/PortCls cyclic buffer connect to the T2-owned ring?
4. How do timestamp events trigger WaveRT position updates?

**Affected devices:** AppleUSBVHCI, AppleAudio/WaveRT or PortCls audio
component, and the T2 audio endpoint. The PCI/BCE owner remains opaque.

**Status**: Option A still requires identifying the async notification and buffer
mapping interface. The five IOCTLs provide a concrete descriptor path, but it is
not established as the audio ring path. The Apple manager-list path has no static
match to kaiT2en's `com.apple.BridgeAudio.IntelToARM` /
`com.apple.BridgeAudio.ARMToIntel` or VHCI `VHC1*` queues. Possibilities include:
- IOCTL 0x222018 also handles async notifications (not proven in static analysis)
- Another unreviewed mechanism exists (WDF notification callbacks, shared memory, etc.)
- Windows implementation differs fundamentally from kaiT2en model

### B. Move the required BCE/PCI portion with audio

**Confirmed capabilities:** KaiT2en demonstrates the complete conceptual
ownership boundary: PCI regions and BARs in `t2audio_probe`, BCE command/event
queues in `protocol_bce.c:t2audio_bce_init`, T2-owned playback BufferStruct
mapping in `audio.c:t2audio_init_bs_stream`, host-owned capture allocation,
serialized I/O, and timestamp interpolation. The native report supplies the
Speaker format contract.

**Unknown:** Windows PCI resource ownership and the T2/BCE queue implementation
are not present. The exact Windows mapping policy, interrupt/event delivery,
power sequencing, and physical channel order still require validation. This
option also requires a Windows audio component exposing 24-in-32 six-channel
PCM, but it does not depend on undocumented AppleAudio object pointers.

**Affected devices:** the T2 PCI function/BAR resources, BCE transport queues,
AppleUSBVHCI interaction if it remains needed for T2 control, and the new
Windows audio endpoint. AppleAudio DSP internals are not required for the
transport proof.

**Minimum next experiment:** build an offline Windows resource/queue contract
model from the manifest and KaiT2en layouts, then validate resource ownership
and ring/position state transitions with synthetic buffers only. Hardware
transport and START/STOP remain out of scope for this stage.

## Decision

**Static analysis of the visible bodies is complete**. `0x222008` now has a complete
static chain through MDL preparation, entry method dispatch, descriptor population,
and manager method dispatch to `0xE4E0`. Only the entry-side methods and the
writers of queried `+0x28/+0x30` remain unresolved for the concrete vtable/write
reasons above. The operation is not identified with a kaiT2en queue and audio
relevance remains unestablished.

Option A remains blocked on three unresolved facts:
1. Async notification delivery mechanism (ALIVE, UPDATE_TIMESTAMP, PROPERTY_CHANGED)
2. PCM buffer mapping ownership (T2 BufferStruct in BAR0)
3. Timestamp event delivery to WaveRT/PortCls

KaiT2en remains the reference for the required command and notification semantics,
but this result does not choose a full BCE port solely because Option A was unknown.
Option A remains open pending static resolution of the deeper vtable targets and
the AppleAudio/AppleUSBVHCI resource contract.

**Option A should not be abandoned**: IOCTL 0x222018 provides proven synchronous
T2 control messaging. If async notifications and buffer mapping can be identified
through deeper AppleAudio.sys analysis or safe runtime observation (without START/STOP),
Option A remains viable. But IOCTLs 0x222004-0x222014 do not contribute to solving
the transport problem.

**Next steps for Option A validation** (if pursued):
- Deep static analysis of AppleAudio.sys buffer allocation and WaveRT integration
- Identify IOCTL 0x222018 async behavior (callback registration, completion context)
- Locate PCI/BAR ownership (AppleUSBVHCI, AppleAudio, or separate component)
- Safe runtime trace of notification delivery without issuing audio commands

**Next steps for Option B** (recommended):
- Windows BCE queue implementation design (ported from kaiT2en core)
- PCI resource ownership and BAR mapping contract
- Async notification dispatch via thread/workqueue
- WaveRT cyclic buffer integration with T2-owned ring

The missing fact is no longer "which IOCTLs" but "which component owns async
notifications and buffer mapping in the Windows stack."
