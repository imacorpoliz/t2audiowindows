# KaiT2en Port Plan

## Recommendation

Choose path B: a Windows audio miniport that owns the six-channel endpoint,
T2 transport, shared BufferStruct/ring resources, and stream timing. Port the
KaiT2en DSP data and processing above that endpoint only after the six-channel
PCM path is independently testable. Do not make AppleAudio internal pointers
the application API.

## Component table

| KaiT2en component | Purpose | Directly reusable | Windows work |
|---|---|---|---|
| `t2bce_audio` BufferStruct definitions | device/stream/buffer metadata | layout clues and constants, after validation | WDF resource ownership, mapping, synchronization |
| BCE protocol/transport | T2 commands, properties, notifications | protocol documentation and message concepts | Apple-specific Windows transport and IRP/IOCTL implementation |
| ALSA PCM setup | exposes negotiated format/channels | format negotiation requirements | WaveRT/PortCls pin descriptors and six-channel formats |
| DMA/ring buffer handling | sample delivery | ring arithmetic concept | DMA/common-buffer policy, ISR/DPC or event model |
| workqueue I/O state machine | serialized START/STOP and teardown | state-machine intent | Windows stream state ownership and cancellation |
| timestamp/timing code | monotonic position interpolation | algorithmic structure | QPC/device clock conversion and glitch handling |
| `bq_raw` nodes | model EQ/crossovers | coefficients and rate selection | native DSP implementation or supported plugin engine |
| LV2 `autogain_stereo`/`bankstown`/compressor | dynamic processing and bass synthesis | parameter inventory only | compatible Windows DSP/plugin runtime and licenses |
| delay/convolver/FIR assets | XTC crosstalk cancellation | WAV assets and graph topology | real-time convolution implementation and asset packaging |
| mixers/route graph | stereo-to-six routing | graph links and channel-count contract | explicit channel map; physical order must be measured |

Licensing requires checking per-file SPDX headers. The checkout documents
kernel modules as GPL-2.0 and DSP/userspace as GPL-3.0-or-later with attribution;
third-party LV2 plugins and assets retain their own licenses. A Windows port
must preserve applicable notices and cannot assume LV2 code is embeddable.

## Delivery options

### A. Existing Apple interface

Evidence: KS currently accepts stereo PCM16 only; native query exposes six
channels as metadata; AppleAudio has a static six-channel branch and ring copy.
Blocker: no supported Windows six-channel PCM write contract has been shown.
User DSP before a stereo-only endpoint cannot create six independent physical
outputs because the endpoint contract discards/does not expose those channels.
Internal AppleAudio addresses are undocumented, object-dependent, and not a
stable API. This option is useful only for a narrowly validated stereo fallback.

### B. Windows miniport

Evidence: KaiT2en already demonstrates the required conceptual ownership:
BufferStruct registration, one interleaved remote stream, negotiated format,
mapped buffer access, serialized I/O commands, and timestamp anchors. The
native query supplies the target six-channel format.
Blockers: Windows T2 transport and resource ownership are not implemented;
AppleAudio's object provenance and complete write contract remain incomplete;
the Windows audio stack must expose 24-in-32 six-channel PCM; DSP execution,
plugin dependencies, and channel-order validation remain separate tasks.

## Next verifiable implementation

Build an offline Windows-side contract model and unit tests first: parse the
native format, represent six interleaved 24-in-32 channels, validate ring
wrap/copy and QPC timestamp interpolation, and keep a single owner for mapped
resources and stream state. Then implement a non-hardware miniport skeleton
with format descriptors only. Hardware transport and START/STOP remain out of
scope until that ownership and test model are reviewed.
