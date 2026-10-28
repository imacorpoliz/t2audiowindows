# WORK_LOG — AppleAudio.sys / T2 Audio Static Analysis

**Project**: MBP 16,1 (2019), Apple T2, Windows 11: native 6-channel audio access.
**Limits (apply to all work)**: static analysis only; do not install/change the driver; do not modify MMIO; do not run stream control commands (START_IO/STOP_IO etc.).

> Archived (early October 2026). Still useful today: §3.4 and §8 describe AppleAudio's 6-channel processing branch (per-channel DSP, disable mask, pool staging, copy into the ring), which is the static counterpart of the speaker layout question in `docs/ROADMAP.md`.

---

## 1. Restoring the state (this audit)

**Methods**: `git status` / `git log` (result: the directory is NOT a git repository, so no git trail), file listing with timestamps, reading every report, checking the binary's SHA256, checking the tools.

### 1.1 Git
- `git status` → `fatal: not a git repository` (working directory `C:\Users\othysa\Desktop\mbp`, run on 2026-10-05).
- Consequence: git diff/status **cannot** be recovered. Changes were tracked only through file timestamps.

### 1.2 Binary verification (done during the audit)
```
Get-FileHash MBP-Audio-driver\AppleAudio.sys -Algorithm SHA256
→ CD8B81FABC9BE6127A082DA22C4CC046A4D46E0225EB6BAF6D27E0DC9862A150
→ 122752 bytes
```
Matches the value recorded in NOTES.md:12–15 and FINAL_REANALYSIS_REPORT.md:13. This is the right AppleAudio.sys 6.1.8100.4.

### 1.3 Tools (checked)
- `py -c "import pefile, capstone"` → OK (2026-10-05). The scripts `disasm_func.py`, `disasm2.py`, `scan_global_fields.py`, `Verify_RVA.ps1` work.
- `MBP-Audio-driver.zip`, `AppleUSBVHCI.zip`: archives of the original binaries; the directory `AppleUSBVHCI-extracted` is their extraction.

---

## 2. Work chronology by file timestamps

### Phase 0: probes on the target hardware (HARDWARE-TESTED, before 09:00 on 04.10)
Sources: `MBP-Audio-Probe-v2\MBP-Audio-report-20261004-085054.txt`, `MBP-T2-Native-Probe\...\MBP-T2-native-report-20261004-090031.txt` (the files exist; their content is summarised in NOTES.md:32–73).

- KS probe: WaveSpeaker publishes stereo PCM16 48 kHz; 6-channel formats are rejected (Win32 1169) — NOTES.md:33–38.
- T2 native probe: IOCTL 0x222018 through `\\.\AppleUSBVHCI`; commands 101/26/7 work; device Speaker = 0x39, stream 0x3A, format 48000 Hz / 6 ch / 'lpcm' / bytes_per_frame=24 / bits=24 / interleaved — NOTES.md:41–73.

### Phase 0.5: StreamControlTest created (20:38)
- Created: `MBP-Stream-Control-Test\StreamControlTest.cs` (10440 B), `Program.cs`, `README.txt`, `.csproj`.
- **Not compiled and NOT run** (deliberately, per the safety limits; NOTES.md:245, 316–319).

### Phase 1: documentation and the first static analysis (19:59–21:40)
Created (in timestamp order):
- `T2_Audio_Protocol_Analysis.md` (19:59), `T2_Windows_Implementation_Guide.md` (20:01): knowledge carried over from kait2en.
- `CURRENT_STATUS.md` (20:40), `СВОДКА_РАБОТЫ.md` (20:41, "work summary"): statuses and plans.
- `AppleUSBVHCI_IOCTL_Analysis.md` (21:18), `AppleAudio_Message_Path.md` (21:26): static analysis of the AppleUSBVHCI ioctl handlers (global object @ RVA 0x27488, zero in the static image) and of AppleAudio's message-sending function (IOCTL 0x222018 @ RVA 0x4A60).
- `NOTES.md` (21:40): the main log: hashes, evidence classification, the T2Request/T2Reply/Property protocol (corrected structures: value_size @ abs 41, value @ abs 49 — NOTES.md:190–198).

### Phase 2: PE analysis and MmMapIoSpaceEx (22:04–23:11)
- `BUFFER_PATH_CORRECTIONS.md` (22:04): the first independent recount. It wrongly concluded that there are no MmMapIoSpaceEx calls (an RVA→file offset conversion error: the .text offset was applied to every section). Acknowledged as an error in FINAL_REANALYSIS_REPORT.md:428–432.
- Scripts created: `Verify_RVA.ps1` (22:21), `disasm_func.py` (22:24), `scan_global_fields.py` (22:48), `disasm2.py` (22:40): reproducible verification.
- `Linux_Windows_Structure_Mapping.md` (23:06), `FINAL_REANALYSIS_REPORT.md` (23:06): **correction**: the right section table (FINAL_REANALYSIS_REPORT.md:27–36), a full scan of the EXEC sections, **exactly 2 MmMapIoSpaceEx calls** found (RVA 0x17204, 0x1721C, PAGE section, function 0x16FF0, IAT 0xE058) — FINAL_REANALYSIS_REPORT.md:84–91, confirmed by capstone (lines 506–510).
- `BUFFER_PATH_ANALYSIS.md` (23:09), `VERIFICATION_SUMMARY.md` (23:11): summary: the mapped addresses are kept in the global object 0x14690 (+0x28 = BAR1, +0x30 = BAR2+0xC000), function 0x4700 reads the BufferStruct metadata (magic 0x19870423).

### Phase 3: BufferStruct and playback (23:24–23:48)
- `Function_46F8_Analysis.md` (23:24) and `AppleAudio_BufferRegistration_Report.md` (23:32): function 0x46F8–0x48AF registers input-stream buffers in BufferStruct: device lookup by name with `_strnicmp` (0x476F→0x4CC0), address/size writes (0x480A/0x4816), num_buffers @ +0x94, active @ +0x90.
- `AppleAudio_Playback_Path_Report.md` (23:48): analysis of AllocateOutputBuffer 0x1C240 (vtable slot +0xB8), GetOutputBufferFromBS 0x9D74, the start/stop handler 0xB470 (memset on stop through 0xCD80, proving that [stream+0xA0] is a real PCM pointer), a zero-copy conclusion.

### Phase 4: the current session (2026-10-05)
- Disassembled (conclusions in §4 below): 0x1C240–0x1C4BA, 0x9D74–0x9E8D, 0xCE80, 0xB300–0xB462, 0xA44C–0xA8A3, 0xC550–0xC8DC. The disassembly results **were not written to a file**; they are restored only in this log by re-running the commands (see §5, "not restored").
- `WORK_LOG.md` (this file) created.

---

## 3. Confirmed facts (with sources)

### 3.1 Hardware and protocol (HARDWARE-TESTED)
| Fact | Source |
|---|---|
| WaveSpeaker: stereo PCM16 48 kHz only; 6ch → error 1169 | NOTES.md:33–38, MBP-Audio-Probe-v2 report |
| IOCTL 0x222018, commands 101/26/7 work | NOTES.md:41–47, MBP-T2-Native-Probe report |
| Speaker 0x39 / stream 0x3A; 48 kHz, 6ch, 'lpcm', 24 B/frame, bits 24, interleaved | NOTES.md:57–73 |

### 3.2 Static: AppleAudio.sys memory and buffers
| Fact | RVA/instruction | Source |
|---|---|---|
| 2 MmMapIoSpaceEx calls | 0x17204, 0x1721C (PAGE, func 0x16FF0), IAT 0xE058 | FINAL_REANALYSIS_REPORT.md:84–91, 506–510; `disasm2.py dis 16FF0 172FE` |
| BAR1 map → [rbx+0x68] (cache 0x404), BAR2 → [rbx+0x70] (cache 0x204) | 0x1721C→0x17222; 0x17204→0x1720A | FINAL_REANALYSIS_REPORT.md:196–212 |
| Copy into the global 0x14690: +0x28=BAR1, +0x30=BAR2+0xC000 | 0x17231–0x17247 → func 0x45F0 | FINAL_REANALYSIS_REPORT.md:222–235 |
| BufferStruct: magic 0x19870423 @+4, version @+0, bs_offset @+8 | 0x4728/0x473B/0x4744 | Function_46F8_Analysis.md:24–36; 0x9D94 cmp magic (disassembly of 0x9D74) |
| Device stride 0xBDEC, devices @ bs+0x10, num_devices @ [rbx+0xC] | 0x475B, 0x474D | Function_46F8_Analysis.md:44–58 |
| Input buffer registration: address @ dev+idx*6*8+0x98, size @ +0xA0; num_buffers @ stream+0x94; active @ stream+0x90 | 0x480A/0x4816/0x4866/0x486D | AppleAudio_BufferRegistration_Report.md:127–148 |
| Output: 0x9D74 reads [dev + idx*0xBDEC + 0x5F0C]=address, +0x5F14=size (version≥2 branch); 0x5F0C = 0x10 + 0x5EF8(output_streams) + 0x04 | 0x9E78, 0x9E83 | **Disassembly of 2026-10-05** (`disasm2.py dis 9D74 9EE0`); matches Playback_Report.md:57–65 after correcting the addresses (see §3.4) |
| 0x9D74 version 1 branch (input): buffer entries stride 0x110, written to [rsp+0x50]/[rsp+0x58] | 0x9DBE–0x9E20 | Disassembly of 2026-10-05 |

### 3.3 Static: AllocateOutputBuffer 0x1C240–0x1C4BA (vtable slot +0xB8)
Checked by disassembly on 2026-10-05 (`py disasm2.py dis 1C240 1C4BA`). Corrects Playback_Report.md:35–48 (the instruction addresses there were shifted):

| What | RVA | Note |
|---|---|---|
| Validation: r8d≠0, r8d ≤ word [stream+0x150]+0xC, r8d % ebx == 0 | 0x1C269–0x1C297 | RDX=frame alignment, R8=size |
| Rounding ebp = size − (size % word[[rsi+0x150]+0xC]) | 0x1C29F–0x1C2A4 | ebp = the aligned size |
| Indirect call vtable+0x18 → result in R12 (buffer handle) | 0x1C2FB, R12@0x1C301 | through [ [rsi+0x40]→vtable ] |
| Second indirect call vtable+0x28 → result in RAX → **[RSI+0x90]** | 0x1C324, 0x1C332 | |
| **[RSI+0x88] = ebp** (aligned size) | 0x1C34B | |
| [RSI+0xA8] = ebx (frame alignment), [RSI+0x68] = period computation | 0x1C340, 0x1C354 | |
| R15 = RSI − 8 (used later as `rcx` in 0x9D74) | 0x1C37C | |
| If stream+0x84 != 0: [rsi+0x98] = [r14+0xCC0], [rsi+0x9C] = /…, [rsi+0xA0] = [r14+0xCC8] | 0x1C382–0x1C3AE | fast path through r14=[rsi+0xa28] |
| Otherwise: call 0x9D74(rcx=R15, rdx=[rsi+0xa28]+0x21, r8=[r14]+0x38 call→rax+0xC000, [rsp+0x20]=&off, [rsp+0x28]=&size) | 0x1C3E8–0x1C414 | |
| [RSI+0x98] = size (RBX), [RSI+0x9C] = size/result_of_0x9BB4 | 0x1C42C, 0x1C446 | |
| Third indirect call vtable+0x38 → RAX += [rsp+0x40] → **[RSI+0xA0] = output buffer address** | 0x1C44F–0x1C45E | RAX = bar1_base + buffer_offset from BufferStruct |
| R12 → **[R13] (output parameter)**; [rsp+0xa0]=ebp size; [rsp+0xa8]=0; [rsp+0xb0]=2 | 0x1C465–0x1C486 | |

**Conclusion (proven by code)**: `[RSI+0x90]` is the result of the second indirect call (a kernel mapping API, the buffer's virtual address); `[RSI+0xA0]` comes from the third call (vtable+0x38), which adds the BAR1 base + the BufferStruct offset (like the 0xB470 memset path). These are **two different sources**; they cannot be treated as one buffer without further tracing (see §6).

### 3.4 Static: the writer function 0xA44C–0xB462 — the PCM write path found
Checked by disassembly on 2026-10-05 (`py disasm2.py dis A44C AC00`, `dis B300 B463`). **This closes the main former "UNKNOWN" (the PCM write path of BUFFER_PATH_ANALYSIS.md:523–537) at the static analysis level:**

- 0xA44E: `mov r14d, edx` — **RDX = the number of bytes from the source [RSP+0x48]** (data source).
- 0xA48E–0xA4C7: if flag 0x12000==0 (offline/direct mode): a loop `0x1A9C(dest=rsi+0x168, src=rsi+0x98+offset, len)`, copying through the internal buffer stream+0x168.
- 0xA4E2–0xA503: checks of flag 0x14680 and 0x42C0(global 0x14690): is the BufferStruct mode active.
- 0xA510–0xA541: `[rsi+0xa30]` (the device descriptor in the mapped BufferStruct): check `[dev+0xE954] != 0`, format fields `[dev+0x71D0]/[dev+0x71D4]` (bytes_per_frame/…), the quotient must be 4 (0xA53F–0xA541).
- 0xA547–0xA57F: the format from `[rsi+0x158]`: `word [+0xE]` (container size, 0x10 → divisor 2, else 4 — 0xA558/0xA564/0xA56D), `word [+2]`: computing **frames = bytes / divisor** and bpf.
- 0xA5B9–0xA5D6: `0x4140(global 0x14690, dev, count)` → **RAX = destination pointer, [RSP+0x48]** (the staging buffer, the source for 0xB3E9: see below).
- 0xA5DC–0xA5F0: `[rsi+0x80] → [+0x8B00]`: mixing config; `[rsp+0x38]`.
- **The 6-channel branch**: 0xA5FE `cmp edi,2` (divisor 2 → 24-bit?) … 0xA600 `cmp ebx,6; jne 0xa8a3`: with a 6-channel format the processing is per channel (0xA607–0xA89E): a loop over 6 channels (ebx 0..5, channel bit checks against the bytes at [0x1468C] @0xA64D–0xA6B4), DSP subroutines (0x80F4, 0x94CC, 0x7B78, 0x6EB8, 0x7EA4, 0x8BBC, 0xBA64, 0xBBAC) and a 4-band biquad filter with constants (0xAA44–0xAAF4, mulsd with 0x3FF9D24DE87F66D0 and others). **This is where the per-channel and dynamic processing (volume/DSP) happens**.
- **The write loop into the mapped buffer**: 0xB398–0xB40D:
  - 0xB3A7: `call 0x9A24(rsi, offset, size)`: computes the position.
  - 0xB3AC–0xB3B8: `lea r8d,[r14+rax]`; **`cmp r8d, dword [rsi+0xA0]`**: bounds check against the buffer size (+0xA0 of the stream).
  - 0xB3C4/0xB3FE: **`add rcx, qword [rsi+0xA8]`**: RCX = base? + frame alignment; wrapped through 0x9A24.
  - 0xB3E1/0xB405: **`mov rdx, qword [rsp+0x48]`**: the data source.
  - 0xB3E9/0xB40D: **`call 0xCE80`**: memmove with ring handling.
  - 0xB412–0xB415: KfRaiseIrql/KfLowerIrql in a pair: the write happens at raised IRQL.
- **0xCE80 is memmove** (confirmed by code, `dis CE80 CF80`): `sub rdx, rcx; jb fail`, qword/byte copy (<0x4F), an aligned SSE path for large sizes. This is **a copy, not zero-copy on the T2 side** (see §5).
- 0xB438–0xB43E: completion, epilogue 0xB457–0xB462.

**The source [RSP+0x48]** = the result of `0x4140(global 0x14690, dev_descriptor, count)` @0xA5C0–0xA5CB: a reserved area (later shown in §8 to be a kernel pool staging buffer, not BAR memory).

### 3.5 Static: function 0xC550–0xC8DC — a format converter (→ the Windows stereo endpoint)
Checked by disassembly on 2026-10-05 (`dis C550 C8DC`):

- 0xC566: check of flag `[0x12004]`; 0xC573–0xC5AE: direct mode, copying `rsi+0x98 → rdi+0x250` through 0x1C3C.
- Otherwise (BufferStruct mode, 0xC5CB–0xC5F1): checks of 0x14680, [rcx+0xA0]≠0, 0x42C0(global).
- 0xC5F7–0xC60F: `[rdi+0xa30]` → `[dev+0xE954]`; if < 3 → `[rdi+0x2A8] = value` and exit. I.e. 0xC550 handles **capture/another mode** for small values.
- 0xC614–0xC6E0: format selection: `[rsi+0x158]: word +2` → EBX (sample bytes), `word +0xE` → AX, `cmp ax, 0x10` → choice of 2 or 4 bytes (0xC66E/0xC68E cmovne r13d, ecx=4); frames = esi / (r13 * ebx); call 0x9BD0: the number of available bytes.
- **The conversion loop** (0xC6EE–0xC8BE), per frame:
  - 16-bit branch (0xC714–0xC753): reads `word [rcx + rax]`, where **rax = qword [rdi+0xA8]** (0xC72A) + index; writes `word [r12 + r13]` into the local buffer rdi+0x250; position: `lea eax,[r14+2]; div dword [rdi+0xA0]`: **modulo the buffer size [rdi+0xA0]** (0xC743).
  - 24/32-bit branch (0xC759–0xC7FE): `r8 = r14 + qword [rdi+0xA8]` (0xC76E); reads dword [r8]; with `[0x8B00]→dword==4` and a special condition, division by 256 (sar 6, sar 8: 0xC7AE–0xC7DD): **conversion 24-in-32 → 16-bit**; at 0xC7B5 a call `0xCCBD(rdi+0xa30+0x21, "Codec N" @0xD700)` (a log); writes `word [r12+r13]`.
  - 32-bit branch (0xC846–0xC87B): reads `dword [rcx + rax]`, rax=qword [rdi+0xA8] (0xC853), `div dword [rdi+0xA0]` (0xC86A).
- As in 0xB3xx: here **[RDI+0xA8] is the read offset (read cursor) and [RDI+0xA0] the ring buffer size**. RDI is a stream object of the same type as RSI in 0xA44C (see §6, hypothesis).

**Conclusion**: 0xC550 converts 24-in-32 → 16-bit for **input** (capture/bridge), producing a Windows stereo 16-bit stream. Its link to the Windows stereo playback endpoint is a separate question (§6).

### 3.6 Corrections of contradictions in the working notes
Confirmed by disassembly on 2026-10-05, per the task:
1. **0x9D74: the output address/size reads are at RVA 0x9E78 and 0x9E83** (not 0x9DC8/0x9DD0 as in AppleAudio_Playback_Path_Report.md:59–60). At 0x9DC8/0x9DD0 there is `imul rsi, rax, 0x110` + `call 0xCCB7` (the input path, version 1). The Playback_Report.md needs correcting.
2. **0x98 = BS header 0x10 + input_streams 0x84 + buffers 0x04**: confirmed (0x5F0C = 0x10+0x5EF8+0x04 for output; 0x98 = 0x84+0x04+0x10 alignment for input per Function_46F8_Analysis.md:130–136). No "extra Windows layout" fields before buffers[] follow from this; the `stream_windows` extension hypothesis (AppleAudio_BufferRegistration_Report.md:152–219) is marked **not confirmed**: after recomputing the bases, the stream layout matches Linux (+0x04 buffers).
3. **Six independent channels = one interleaved stream**: confirmed by the structure: device 0x39 has a single stream 0x3A (NOTES.md:58), and 0x1C240 allocates one buffer per stream. Separate streams are not needed. The earlier "1 interleaved OR 6 separate?" (Playback_Report.md:158–168) is resolved in favour of interleaved.
4. The native Speaker format is already known (48 kHz/6ch/24-in-32 interleaved) — NOTES.md:57–73; Playback_Report.md:170–171 ("the next gap is the PCM format") is outdated. The new question: the link to the Windows stereo endpoint.

---

## 4. The proven chain (data path, playback)

```
Windows KS pin (stereo PCM16 → 24-in-32 buffer, stream+0x168 stage)   [0xA44C: RDX bytes, source 0xA4C0→0x1A9C]
   ↓ (BufferStruct mode: flags 0x14680, 0x42C0, dev+0xE954)
Format/channels: [stream+0x158] word+0xE/+2 → divisor 2|4; 6ch → per-channel DSP
   [0xA558–0xA57F; 0xA607–0xAAF4: biquads, 0x80F4/0x94CC/…]
   ↓
0x4140(global 0x14690, dev, count) → destination staging [RSP+0x48]   [0xA5C0–0xA5CB]
   ↓
KfRaiseIrql → 0x9A24(stream, offset, size) → position                  [0xB390–0xB3A7]
   ↓
Bounds: cmp r8d, [stream+0xA0]; wrap: mod [stream+0xA0]                 [0xB3B8, 0xB3D0]
   ↓
add rcx, [stream+0xA8]  (base + read offset)                            [0xB3C4, 0xB3FE]
   ↓
0xCE80 = memmove(src=[RSP+0x48], dst=ring, size): A COPY                [0xB3E9, 0xB40D; 0xCE80 confirmed]
   ↓ (KfLowerIrql 0xB412)
T2 ring buffer (BAR1 + BufferStruct offset) ← read by the T2 DMA
```

Destination-side allocation: AllocateOutputBuffer 0x1C240 → `[stream+0xA0]` = bar1_base + offset from BufferStruct (through the vtable+0x38 call @0x1C44F), `[stream+0x90]` = from the vtable+0x28 call @0x1C324 (a second source, link not proven), handle → R12 → [out].

**Zero-copy: NOT proven.** 0xCE80 is a confirmed memmove; the data are copied into the ring buffer at raised IRQL. The earlier claim of Playback_Report.md:129 ("the driver does NOT copy PCM, zero-copy") is **retracted**.

---

## 5. Hypotheses (NOT confirmed)

1. **RSI in 0xA44C / RDI in 0xC550 = the same stream object as RSI in 0x1C240** (then +0xA8/the 0x9A24 base at 0xB3C4 corresponds to +0xA0 in Allocate). Basis: the same offsets 0xA0/0xA8/0x98, typical checks `[stream+0x84]`, the `[rsi+0xa30]` BufferStruct device descriptor. **Not proven by the pointers' origin**: needs tracing of the vtable dispatch (who calls 0xA44C/0xC550/0x1C240).
2. **The source [RSP+0x48] in 0xA44C** = the result of 0x4140: a staging area (probably in mapped BAR1/BufferStruct). The internals of 0x4140 are not analysed. (Answered in §8: a kernel pool buffer.)
3. **0xC550 = a capture/converter** 24-in-32→16-bit; the meaning of the field `[dev+0xE954]` (<3 → exit) is unknown.
4. **How the Windows stereo endpoint relates to the native 6ch**: a probable chain: stereo PCM16 upmix → 24-in-32 6ch (0xA44C DSP) → T2 ring. The reverse (capture) is 0xC550. The upmix code before 0xA44C is not traced (KS pin → caller of 0xA44C).
5. **+0x90 and +0xA0 in 0x1C240: one buffer or two**: neither is proven.

## 6. Could not be restored / left as UNKNOWN

- **The full log of the 2026-10-05 session (phase 4)**: the disassembly output (0x1C240, 0x9D74, 0xCE80, 0xA44C, 0xC550) was not saved to files; in this log it is restored by re-running the same `py disasm2.py dis …` commands, and the results match the RVAs given here.
- **Git history**: there is no repository; which files changed/were created is known only from the LastWriteTime stamps (listed in §2).
- **Intermediate drafts/reports, if any**: no traces.
- **Who calls 0xA44C / 0xC550 / 0x1C240**: the vtable xref tracing was not done in this session (the command `py disasm2.py xref <addr>` is available).
- **0x4140, 0x9A24, 0x9BB4, 0x42C0, 0x80F4/0x94CC and other DSP functions**: not disassembled.

## 7. Recommended next steps (static, safe)

1. `py disasm2.py xref A44C` / `xref C550` / `xref 1C240`: find the vtable slots and prove that RSI/RDI are one type of stream object (closes hypothesis §5.1).
2. Analyse `0x4140` (the source of [RSP+0x48]) and `0x9A24` (read/write position).
3. Compare the owner object of fields +0x88/+0x90/+0xA0/+0xA8 between 0x1C240, 0xA44C, 0xC550.
4. Write the phase 4 disassembly into a separate file (e.g. `disasm_phase4.txt`) so it is not lost.

**Limits kept**: static analysis only; do not install the driver; do not change MMIO; do not run stream control commands (START_IO/STOP_IO; StreamControlTest is not run).

---

## 8. Phase 5: reconstructing stereo -> 6ch (start)

### Stage 1: the pool buffer and the direct caller (2026-10-05)

Commands and saved results: `disasm_phase5.txt`.

#### Confirmed

- `0x4140` takes `RDX` as the device/stream object and `R8D` as the requested count.
- `0x4157`: `ESI = [RDX+0x71D0]`.
- `0x417F`: `ExFreePoolWithTag` through IAT `0xE168`.
- `0x4185`: computes the size `[device+0x71D0] * requested_count`.
- `0x4195`: `ExAllocatePoolWithTag` through IAT `0xE0B8`.
- `0x419B`: the result is stored in `[device+0xE968]`.
- `0x41AF`: the capacity is stored in `[device+0xE960]`.
- `0x41C6`: the pool buffer is zeroed through `0xCD80`.
- `0x41CB-0x41D7`: returns `[device+0xE968]`.

Consequence: the source `[RSP+0x48]` in `0xA44C` after `0xA5C0-0xA5CB` is a pool/staging buffer from `0x4140`, not mapped BAR/T2 memory.

#### Origin of `this` in the direct path

- `0xBE36`: `mov edx, edi`; `0xBE39`: `call 0xC550`.
- `0xBEF3`: `mov rcx, rbx`; `0xBEF6`: `call 0xA44C`.
- This proves that `0xA44C` gets `RCX=RBX`, the caller's object, and that `0xC550` is called from the same function with the same saved `RBX` object.
- `py disasm2.py xref 1C240` returned `no xrefs to 0x1C240`; it is a virtual/interface method, and no direct call site was found.

#### Not proven yet

- That `RBX` at `0xBEF6` is the same as `RSI` in `0x1C240`.
- The correspondence of fields `+0x90/+0x98/+0xA0/+0xA8` between the two contexts.
- The specific DSP algorithm of the called subfunctions.

Next stage: save the full disassembly of the `0xA607-0xA8A3` branch, classify the six iterations and trace the calls/object vtable chains.

### Stage 2: the 6-channel branch and data movement (2026-10-05)

Commands and the full saved output: `disasm_phase5.txt`, Stage 2.

#### Confirmed format split

- `0xA55D`: `EDI = WORD [format+2]`: the number of input channels.
- `0xA558/0xA564`: `ECX` is chosen as `2` or `4`: the byte divisor/sample container.
- `0xA5F5`: checks `EDI == 2`.
- `0xA5FE`: `EBX == 6` selects the 6-channel branch.

`EDI` is not the sample size; `EBX` is not the input channel count.

#### The 6-channel branch table

The input address is computed at `0xA630`: `RDI = [RSI+0x98] + input_offset`. Channels `EBX=0..5` run in a loop. At `0xA63F-0xA646` the write address is `R15 = pool_base + 4*EBX`; the value is written at `0xA84F` (`DWORD [R15]`).

- Channel 0: `WORD [RDI]` (`0xA7E6`), disable bit 0 (`0xA64D-0xA655`), state `[RSI+0x2B0]/[RSI+0x350]`, call `0x94CC` (`0xA813`), output `pool+0`.
- Channel 1: `WORD [RDI]` (`0xA7D5`/`0xA7D8`), disable bit 1 (`0xA660-0xA668`), a visible clamp/scale path without a named call, output `pool+4`.
- Channel 2: `WORD [RDI]` (`0xA799`), disable bit 2 (`0xA673-0xA67B`), state `[RSI+0x530]/[RSI+0x5D0]`, calls chosen through `0xA744-0xA747` (`0x80F4`, `0x6EB8`, `0x7EA4`, `0x8BBC`, `0xBA64`, `0xBBAC`), output `pool+8`.
- Channel 3: `WORD [RDI+2]` (`0xA768`), disable bit 3 (`0xA686-0xA68E`), state `[RSI+0x670]/[RSI+0x710]`, the same selector-dependent family, output `pool+12`.
- Channel 4: `WORD [RDI+2]` (`0xA715`), disable bit 4 (`0xA699-0xA6A1`), state `[RSI+0x8F0]/[RSI+0x990]`, `0x80F4` (`0xA747`), output `pool+16`.
- Channel 5: `WORD [RDI+2]` (`0xA6F2-0xA710`), disable bit 5 (`0xA6AC-0xA6B4`), a visible clamp/scale path without a named call, output `pool+20`.

For every channel a set bit leads to `0xA82D`, and then zero is written through `0xA84F`. After computation the value is scaled with `shl r8d, CL` (`0xA830-0xA84C`), where `CL = [device+0x71D8] - [format+0xE]`.

This proves two source word-position classes (`[RDI]` and `[RDI+2]`), per-channel state and a disable mask. It **does not prove** that these words are stereo PCM L/R samples; the physical mapping of channels to speakers is not proven either. (Consistent with the later layout: channels 0–2 from the left word, 3–5 from the right word, with channel 2 and 5 on the tweeter filter family.)

#### The source -> pool -> ring chain

- Direct/offline source path: `0xA4B6` adds `[RSI+0x98]` to the offset, `0xA4C0` sets the destination `[RSI+0x168]`, `0xA4C7` calls `0x1A9C`.
- BufferStruct path: `0xA5B9` loads the global `0x14690`, `0xA5C3` loads the device `[RSI+0xA30]`, `0xA5C6` calls `0x4140`, `0xA5CB` stores the returned pool pointer in `[RSP+0x48]`.
- Ring destination/position: `0xB3A7` calls `0x9A24`; `0xB3B8` and `0xB3D0` check/limit against `[RSI+0xA0]`; `0xB3C4/0xB3FE` add `[RSI+0xA8]` to the destination base.
- Copy: `0xB3E1/0xB405` pass `[RSP+0x48]` as the source; `0xB3E9/0xB40D` call `0xCE80`. The `0xCE80` code does a qword/byte/SSE copy.

Consequence: `pool staging -> ring copy` is confirmed; zero-copy cannot be claimed.

#### Correcting old wording

- `+0x90` is no longer called `active_flag`: in `0x1C240` it is the result of the second indirect call (`0x1C324 -> 0x1C332`), and in the writer context it is a numeric field used as a divisor/size parameter (`0xA48E`, `0xB3B8`); its meaning depends on confirming the object.
- The claim about an extra Windows layout before `buffers[]` is not used: equal offsets alone do not prove a layout.
- `0x4140` is not called a mapped BAR/T2 buffer: it is a kernel pool allocation (`ExAllocatePoolWithTag`/`ExFreePoolWithTag`).
- No claim is made that EBX 0..5 corresponds to the physical speakers.

The next step remains: prove or refute that `RSI`/`RBX` are the same, through the constructor and the vtable/interface dispatch; a direct `xref 0x1C240` found nothing, consistent with a virtual call.

### Stage 3: the `+8` interface and the limits of the proof (2026-10-05)

Commands and results are saved in `disasm_phase5.txt`, Stage 3.

#### Proven

- At `0x19E28`, `0x1C570` is called with `RCX=RBX`, where `RBX` is the internal object being created/initialised.
- `0x19E3D-0x19E4D` returns `RBX+8` to the outside (`lea rdx,[rbx+8]`, conditional select, `mov [r12],rcx`).
 - **SUPERSEDED:** a direct read of the function/pointer at RVA `0xF100` alone does not prove the start of a vtable or its first slot; the object base and the table bounds are not established.
- `0x1C266` keeps the incoming interface pointer in `RSI`; `0x1C37C` computes `R15=RSI-8`; `0x1C411-0x1C414` pass R15 to `0x9D74`.

This proves the +8 adjustment for the object/interface of `0x1C240`.

#### Not proven

- `RBX` in the direct caller `0xBEF6 -> 0xA44C` is not statically linked to the allocation `RBX` of `0x19DE5`.
- So the candidate correspondences `A44C+0x90 -> Allocate+0x88`, `A44C+0x98 -> Allocate+0xA0`, `A44C+0xA0 -> Allocate+0xA8` are only a consistent hypothesis from the fields and the +8, not a proof of one object.
- `py disasm2.py xref 1C240` gave `no xrefs`, consistent with a virtual call, but it does not recover the calling object.

#### Correcting old claims

- The old declaration `+0x90 = active_flag` is excluded from new conclusions. In the Allocate context it is the result of the indirect call `0x1C324 -> [RSI+0x90]`; in the writer context `[RSI+0x90]` takes part in division/size computations (`0xA48E`, `0xB3B8`). It must not be called an active flag.
- The old claim about an extra Windows layout before `buffers[]` is not used: equal offsets are not proof of a layout.

#### Phase 5 result so far

Proven: `0x4140` gives a pool staging pointer; the six channels are processed by a separate loop; `0xA44C` passes the pool buffer to `0xCE80`, which copies the data into the ring destination; the `+8` interface for `0x1C240` is proven.

Not proven: the exact meaning of the two word sources as stereo PCM L/R, that the objects of `0xA44C` and `0x1C240` are the same, and the correspondence of indices 0..5 to the physical speakers.

## 9. Phase 6: KaiT2en portability inventory and Windows plan (2026-10-05)

### KaiT2en source

- A local checkout was found in `kait2en/`.
- There is no `.git`: the commit SHA and the checkout history cannot be checked; no SHA is invented.
- Read selectively: `modules/t2bce_audio/audio.h`, `audio.c`, `pcm.c`, `description.h`, `dsp/README.md`, `dsp/profiles/16_1/graph.json`, `LICENSING.md` and `LICENSE`.

### Inventory result

- `tools/inventory_kait2en_profile.py` read the graph without processing sound.
- Result: 108 nodes, 118 links, 2 inputs, 6 outputs.
- Nodes: 92 `bq_raw`, 4 mixers, copy/delay/convolver; 8 LV2: autogain (1), bankstown (1), stereo limiter (3), stereo multiband compressor (3).
- External dependencies: three XTC FIR WAVs for 44.1/48/96 kHz.
 - The capture-side graph takes 2 channels FL/FR; the playback side routes 6 channels to `alsa_output.hw_Audio_0` as `AUX0..AUX5`. This is the graph topology, not proof of the physical speaker order. **SUPERSEDED:** the old table in which channels 4 and 5 were swapped.

### Corrected offset notes

- With `I=O+8`: `O+0x90=I+0x88`, `O+0x98=I+0x90`, `O+0xA0=I+0x98`, `O+0xA8=I+0xA0`. The previous offset mapping is marked **SUPERSEDED**.
- The KaiT2en PCM code describes the negotiated sample format/rate/channels, one remote stream/substream, mapped buffer exposure, an ordered workqueue for I/O and timestamp-anchor interpolation. The START/STOP code was not run.

### Porting decision

- Recommended path B: a Windows audio miniport with an explicit owner of the T2 transport, the mapped/ring resources, the stream state and the timing; the DSP graph/data to be carried over on top of an independently verifiable 6ch endpoint.
- Path A is limited: the current KS endpoint is confirmed only as stereo, and the native six-channel query is metadata, not a PCM write API. A user-mode DSP in front of a stereo endpoint does not create 6 arbitrary independent outputs.
- AppleAudio's internal pointers are not treated as a stable API. Further reverse engineering is justified only to close the miniport contract/object ownership.

(This is the path the project took: T2AudioPort, with the KAIT2EN graph ported into the driver as `src/T2AudioDsp.h`.)

### Artefacts and checks

- Created `CURRENT_STATE.md`, `PORT_PLAN.md`, `tools/inventory_kait2en_profile.py`, `evidence_kait2en_inventory.txt`.
- `py -m py_compile tools\\inventory_kait2en_profile.py` finished with exit 0.
- The JSON `kait2en/dsp/profiles/16_1/graph.json` was parsed successfully.
- No hardware/driver/MMIO/stream-control operations were done in this phase.
