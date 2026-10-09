# T2AudioPort Driver - Current State (Authoritative)

**Last Updated**: 2026-10-09
**Current status**: Experimental BSOD reproducer, not a working audio driver. The signed `packaging/` build is 1.0.3.15 and repeatedly bugchecks when BCE `START_IO` is enabled; the manual test agent rejects `-BceIo`. The test machine runs AppleAudio (Started/OK). The long status line and stage-by-stage notes below include historical intermediate results; read the dated updates first.
**Status**: Diagnostic mode. Driver now **inert by default**: it refuses to start unless a VOLATILE test session exists, and never writes device memory unless separately enabled. All hardware activation is manual via `tools/T2Audio-TestAgent.ps1`. Audio I/O disabled by design. Crash-fix increments 1-3 applied and host-tested; increment 2b (bounded BCE send that abandons a stuck IRP in place) added after the staged hardware test hung at stage 3, then corrected after review to use a `KMUTEX` (not a `FAST_MUTEX`) and to drop the race-prone `IoCancelIrp`. A second review pass then bounded the KMUTEX wait itself, fixed the WaveRT position-register `Accuracy` units and the `SetState` post-`STOP_IO`-failure state, and made the test agent drop the session before a bounded disable; a third pass stopped `FreeAudioBuffer` from disturbing a live stream on a foreign MDL and made every PnP state change in the agent bounded/truthful. Latest build `src\bin\Debug\T2AudioMiniport.sys`, 47,104 bytes, SHA256 `F723AC9DE9D35AE4BFB12A52F957E04D847CC57E17F514667096D1BEA8936315` (unsigned). Re-signed package `packaging\T2AudioMiniport.sys`, SHA256 `B918EBEC7858F2634B47CB6DF1D898B82CC8BEABDE461BEE8FF65EDBC6281641`, INF `DriverVer` bumped to 1.0.3.13 (published `oem159.inf`); the installed `C:\Windows\System32\drivers\T2AudioMiniport.sys` matches it. Earlier, package `9CCB5728...` / `1.0.3.12` / `oem158.inf` **passed stage 3 on hardware** (capture `docs/logs/capture_agent_20261007_221001/`) but **bugchecked at stage 4** (`0x7E`, `0xC0000005`, 22:14:39; capture `docs/logs/capture_agent_20261007_221322/`). Increment 4 then added a third gate (`EnableBceIo`, off by default), PASSIVE-level guards + IRQL logging around the BCE send, and reliable small-dump capture. Stage 4 **without** I/O then passed on hardware (capture `docs/logs/capture_agent_20261007_223141/`): speaker wired + transport held open, full render probe clean, no command sent, and `SetState` confirmed at `irql=0` (PASSIVE) - so the earlier crash localizes to the `START_IO` send. The device was left disabled and the volatile session removed after every run.
**Branch**: `diagnostics`
**Previous Commit**: `480b6ce`

### 2026-10-09: publish the reproducible BSOD build

- The signed package `packaging/T2AudioMiniport.sys` (`1.0.3.15`, SHA256 `7DDB8237DE367669B98B603782F0DEF39EABFBEB3F79D33532C8D1F11BD32039`) is the build that bugchecked on `-Stage 4 -BceIo` on 2026-10-08. The earlier source-only `START_IO` block has been reverted **to make the published source match the reproducer**; the manual test agent still refuses `-BceIo` even with `-Force`. Neither the package nor this source revision fixes the BSOD. AppleAudio remains bound/Started on the test machine. `README.md` describes the failure and recovery steps. No new hardware test was performed for publication.

### 2026-10-08: repeat crash on signed 1.0.3.15; hardware START_IO disabled

- Signed `1.0.3.15` (`7DDB8237DE367669B98B603782F0DEF39EABFBEB3F79D33532C8D1F11BD32039`) was installed for the authorized `-Stage 4 -BceIo` retest. One preliminary run stopped before probing because PnP said `enable-device` was "already enabled"; agent cleanup then disabled the device. A subsequent run activated it and bugchecked at 01:48:20. Evidence: `C:\Windows\Minidump\100826-23546-01.dmp` and `docs/logs/capture_agent_20261008_014807/stdout.txt`. DbgView logged successful BAR mapping, BCE discovery, SET_REMOTE_ACCESS and WaveRT setup; the tail was not flushed. `kernel_raw.csv` is empty. `C:\Windows\MEMORY.DMP` is older and partially corrupt; it is not evidence for this test.
- New dump is **identical** to the prior crash: `0x7E`, access violation reading `NULL+0x18` at `ks!DispatchDeviceIoControl+0x15`, with `AppleUSBVHCI+0x259a -> ksthunk -> ks` on a System worker thread. The outgoing IRP `FileObject` change did **not** fix the fault; do not claim it did. Disassembly proves KS sees `FileObject == NULL` on the forwarded IRP. The small dump does not contain the IRP contents, so the exact AppleUSBVHCI forwarding/notification mechanism is not yet proven. A further unrecorded reset around 01:52 has no new bugcheck event or dump, so its cause is unconfirmed.
- After that test the device was restored to **AppleAudio** (`oem16.inf`), enabled, `Started`, `CM_PROB_NONE`; volatile `TestSession` absent and the T2 package no longer published in the machine's driver store. The temporary source-only `START_IO` block was subsequently reverted for the 2026-10-09 reproducible-BSOD publication (see above). `tools/T2Audio-TestAgent.ps1` still refuses `-BceIo`, regardless of `-Force`; no further risky hardware run should occur on this path. The earlier outgoing FileObject assignment remains for ordinary requests, but does not resolve the separate forwarded IRP. Next: investigate AppleUSBVHCI's forwarded IRP construction and the audio protocol notification sequence offline before attempting playback again.

### 2026-10-08: stage-4 BCE crash diagnosis and source fix (hardware retest pending)

- The signed build (installed hash `E1362CB202E98119096107F089CA2CA128E5C8E537455A417FAE5AD82D7698D6`) crashed during `-Stage 4 -BceIo`. The current crash evidence is `C:\Windows\Minidump\100826-15859-01.dmp` (01:35:07); the older `C:\Windows\MEMORY.DMP` is **not** this crash. DbgView wrote its messages to `docs/logs/capture_agent_20261008_013454/stdout.txt`, while `kernel_raw.csv` was empty. The messages show successful BCE discovery of Speaker `0x39` and `SET_REMOTE_ACCESS(ON)`; the tail was lost before the crash.
- `!analyze -v` reports `0x7E`, `0xC0000005`, at `ks!DispatchDeviceIoControl+0x15` reading address `0x18`. The call stack passes through `AppleUSBVHCI`, `ksthunk`, then `ks`; disassembly shows KS dereferencing `IoGetCurrentIrpStackLocation(Irp)->FileObject` without a NULL guard (`_IO_STACK_LOCATION.FileObject` at `+0x30`, `_FILE_OBJECT.FsContext` at `+0x18`). `IoBuildDeviceIoControlRequest` does not attach our open transport file object to the IRP.
- Source fix in `src/BceTransport.c`: refuse sends unless both the device and file objects exist, then assign the referenced transport `FileObject` to `IoGetNextIrpStackLocation(irp)->FileObject` before `IoCallDriver`. The existing transport lock retains the file-object reference until completion; timed-out sends poison the transport and retain it. Debug x64 builds successfully (unsigned SHA256 `B5EB4B7838C66FCD6959C368418B2DAFDAFD94F94FD40605A58403E86D2D84F2`); both host test executables pass. This eliminates the demonstrated NULL-file-object path in source; **hardware behavior has not been reverified**. Artifacts reported during START_IO may also be due to starting hardware while `EnableMmioCopy=0` (hardware buffer has no new PCM); this remains a hypothesis, not a validated playback fix.
- After the crash, the old test package `oem157.inf` was removed and the device returned to `AppleAudio` (`oem16.inf`, `CM_PROB_NONE`). For the user-authorized retest, the fixed binary was copied into `packaging/`, signed with the trusted test certificate, the catalog regenerated and signed, and the INF bumped to `1.0.3.15`. Installed and packaged SHA256 both equal `7DDB8237DE367669B98B603782F0DEF39EABFBEB3F79D33532C8D1F11BD32039`; `pnputil /add-driver /install` bound the device as `oem157.inf`, but reports `CM_PROB_NEED_RESTART` (Code 14). A reboot is required **before** manual stage-4 + BCE-I/O testing. No MMIO copy is planned. After testing, delete the test package and verify the device returns to `AppleAudio`. The test agent was corrected to abort the probe on nonzero PnP enable/restart result or a device that is not Started.

> This file (`docs/CURRENT_STATE.md`) is the single authoritative status document.
> The former root `CURRENT_STATE.md` is superseded and now only points here.

---

## TEST SAFETY MODEL - READ FIRST (2026-10-07)

The driver no longer activates on its own. Two independent gates control it:

1. **Start gate (`EnableTestMode`).** `T2AudioStartDevice` reads
   `EnableTestMode` from the **volatile** key
   `HKLM\SYSTEM\CurrentControlSet\Services\T2AudioMiniport\TestSession`. If it is
   not `1` the driver returns `STATUS_UNSUCCESSFUL` **before** mapping any BAR,
   opening the BCE transport, or registering an endpoint. Because the key is
   volatile, the kernel discards it on every reboot (including after a bugcheck),
   so a normal boot always finds no session and refuses to start.
2. **Device-memory write gate (`EnableMmioCopy`).** Even in a session, the copy
   DPC only writes into BAR1 when `EnableMmioCopy=1`. It defaults to `0`, so the
   driver can start and be probed without ever writing device memory.
3. **Hardware-I/O gate (`EnableBceIo`).** Even in a session, `SetState` only
   issues BCE `START_IO`/`STOP_IO` when `EnableBceIo=1` (agent `-BceIo`). It
   defaults to `0`, so a stage-4 run can wire the speaker and hold the transport
   open without sending any command. The BCE send is additionally refused above
   `PASSIVE_LEVEL`.

The Safe Mode guard (`SharedUserData->SafeBootMode`) is still checked first.

**How to run a test (manual only):**
- `.\tools\T2Audio-TestAgent.ps1 -Mode Status`  - read-only state report.
- `.\tools\T2Audio-TestAgent.ps1 -Mode Test`    - create session, enable device,
  capture + probe, then **always** disable the device and drop the session.
- `.\tools\T2Audio-TestAgent.ps1 -Mode Test -Stage 4` - wire speaker, send no I/O.
- `.\tools\T2Audio-TestAgent.ps1 -Mode Test -Stage 4 -BceIo` - + BCE `START_IO`/`STOP_IO`.
- `.\tools\T2Audio-TestAgent.ps1 -Mode Test -Mmio` - same, but allows BAR1 writes.
- `.\tools\T2Audio-TestAgent.ps1 -Mode Recover` - run after a crash/reboot:
  drops any session and disables the device.

The agent is **not** a scheduled task. `tools\Auto-Capture-AfterBoot.ps1` is now
a no-op stub and the `T2Audio-AutoCapture-AfterBoot` task must not exist.

`tools\Install-T2AudioDriver.ps1` now **stages only** by default
(`pnputil /add-driver`, no `/install`); it no longer disables AppleAudio or
reboots. Binding requires the explicit `-Bind` switch, and even then the driver
stays inert until the agent creates a session.

Uncommitted work (this session): `src/Driver.c` (volatile test-session gate),
`src/T2AudioMiniport.h` (`TestModeEnabled`/`MmioCopyEnabled`),
`src/WaveRTStream.c` (DPC gated on `MmioCopyEnabled`), `src/Device.c` +
`src/T2AudioBufferLogic.h` (metadata-table bounds check),
`tests/T2AudioBufferLogicTest.c`, `tools/T2Audio-TestAgent.ps1`,
`tools/Auto-Capture-AfterBoot.ps1`, `tools/Install-T2AudioDriver.ps1`.

---

## CRASH-FIX INCREMENT 1 (2026-10-07)

First code increment of the crash-fix plan (see "Crash-fix plan" below). It
targets the WaveRT stream lifecycle and resource teardown — the paths that run
whenever an endpoint is opened, played, paused, stopped, or torn down. It does
**not** yet change the hardware PCM path (BCE addressing, START_IO, MMIO copy);
those stay behind the existing gates.

Changes:

1. **Unmatched `STOP_IO` fixed.** `T2AudioStreamSetState` previously set
   `HardwareStarted = TRUE` even when `ForceSystemBuffer` skipped `START_IO`,
   so a later PAUSE/STOP sent a `STOP_IO` that had no matching `START_IO`. The
   flag is now `HardwareIoStarted`, set only after a successful `T2AudioStartIo`,
   and the RUN/STOP decisions go through the pure helpers
   `T2AudioDecideHardwareIo` / `T2AudioDecideStopHardwareIo`.
2. **RUN edge handling.** Hardware I/O and the copy timer are started only on the
   STOP/PAUSE -> RUN transition (a redundant RUN is a no-op), and the play-cursor
   anchor is set on that edge. The timer is (re)started only when there is a
   system buffer and it is not already active (`T2AudioShouldStartCopyTimer`).
3. **`FreeAudioBuffer` contract.** It now ignores a NULL or foreign MDL, matching
   the documented behaviour. `T2AudioStreamReleaseBuffer(NULL)` (force release)
   is reserved for internal teardown and is no longer reachable from
   `FreeAudioBuffer`.
4. **Teardown is coordinated and idempotent.** `T2AudioUnmapResources` clears
   `HardwareReady` first (gating the copy DPC and I/O), then closes the BCE
   transport, frees the speaker MDL, and clears the mapping pointers **before**
   `MmUnmapIoSpace`. The copy DPC now also checks `HardwareReady`. A second call
   is a no-op.
5. **No leak across restart.** `T2AudioStartDevice` calls `T2AudioUnmapResources`
   before reusing the device context, so a re-start after stop/remove no longer
   leaks the previous BAR mapping and BCE file-object reference.

Verification: Debug x64 rebuild succeeds (warnings unchanged/pre-existing);
`tests/T2AudioBufferLogicTest.c` extended with the three new helpers — 45
assertions, ALL PASSED. The kernel paths still require the test agent + hardware
to exercise.

Artifact: `src\bin\Debug\T2AudioMiniport.sys`, 44,544 bytes, SHA256
`565F4BDE42869DBC1C7D111B38E8C0E4173D27514CA10236E05223F49364932B`.

### CRASH-FIX INCREMENT 2 (2026-10-07) - BCE transport

Second increment, plan item 4 (BCE transport hardening):

6. **Transport serialization.** A mutex (initialized from `DriverEntry` via
   `T2AudioInitializeBceTransport`) now guards `T2AudioOpenBceTransport`,
   `T2AudioCloseBceTransport`, and the whole of `T2AudioSendBceMessage`
   (including the wait for IRP completion). `T2AudioCloseBceTransport` can no
   longer dereference the file object while a request is in flight against the
   device it keeps alive. A missing lock (not initialized) fails closed with
   `STATUS_DEVICE_NOT_READY`. **Corrected in 2b:** this was originally a
   `FAST_MUTEX`, which raises IRQL to APC_LEVEL - illegal for the
   `IoGetDeviceObjectPointer` call in `T2AudioOpenBceTransport`, which requires
   PASSIVE_LEVEL. It is now a `KMUTEX` (`KeWaitForSingleObject`/`KeReleaseMutex`),
   which is correct because every BCE caller runs at PASSIVE_LEVEL.
7. **Full command-reply validation.** `T2AudioStartIo`/`T2AudioStopIo` no longer
   check only the reply length and status; they now validate the whole reply
   (tag `"Audt"`, RESPONSE type, echoed device id, message id, zero protocol
   status) through the pure helper `T2AudioBceParseCommandResponse`. A malformed
   or mismatched reply is `STATUS_DEVICE_PROTOCOL_ERROR`.

Verification: Debug x64 rebuild succeeds; `tests/BceProtocolLogicTest.c` extended
with 8 `T2AudioBceParseCommandResponse` assertions — 48 assertions, ALL PASSED.
No timeout was added here: a naive `KeWaitForSingleObject` timeout would leave the
IRP in flight against caller-owned buffers, so the bounded-cancel design was
deferred. **The stage-3 hardware test then confirmed this is a real hang** (see
increment 2b below), and the bounded-cancel design was implemented there.

Known remaining lifecycle gap (not fixed here): there is no `IRP_MN_REMOVE_DEVICE`
handler, so a device remove that PortCls does not route through a re-start still
leaves the BAR mapping in place until the next start. This is a bounded leak, not
a crash; the disable/enable cycle the test agent performs is covered by the
increment-1 start-time cleanup. Adding a proper PnP/power remove path is a
follow-up (it must not override PortCls's own PnP dispatch).

### CRASH-FIX INCREMENT 3 (2026-10-07) - staged isolation

Third increment, plan item 1 (a truly isolated RAM-only mode and separate
diagnostic stages). The single `EnableTestMode` switch is replaced by a
cumulative **`DiagStage`** (read from the volatile `TestSession` key) so a fault
can be attributed to exactly one step. `T2AudioStartDevice` now behaves as:

- **Stage 1 - RAM only** (default). No `T2AudioMapResources`, so no BAR is ever
  mapped, `HardwareReady` stays `FALSE`, no BCE is opened, and `SpeakerDeviceId`
  stays `0`. The endpoint registers and plays into a system-memory buffer only;
  the DPC advances the position counter but cannot touch a BAR.
- **Stage 2 - BAR metadata.** Maps the BARs and locates the speaker buffer.
- **Stage 3 - BCE discovery.** Opens the transport and resolves the speaker, but
  leaves the audio path unwired (transport closed again).
- **Stage 4 - hardware I/O.** Wires the speaker so `START_IO`/`STOP_IO` can run;
  the DPC still does not write BAR1.
- **Stage 5 - MMIO copy.** The copy DPC writes PCM into BAR1. Requires **both**
  stage 5 and `EnableMmioCopy=1`.

Two correctness fixes fall out of this: `T2AudioStartDevice` no longer sets
`HardwareReady = TRUE` unconditionally (it now reflects the actual mapping), and
`MmioCopyEnabled` is gated on stage 5.

Verification: Debug x64 rebuild succeeds; both host suites pass (45 + 48
assertions). `tools/T2Audio-TestAgent.ps1` gained `-Stage 1..5` (default 1); its
`-Mmio` switch implies stage 5. The agent's finally-block still always disables
the device and drops the session.

### STAGED HARDWARE TEST 1 - stage 3 hang (2026-10-07)

The first staged run on hardware (capture `docs/logs/capture_agent_20261007_201316/`)
proved the cumulative stages work:

- **Stage 1 (RAM only)**: clean, no crash. Endpoint registered and a WASAPI probe
  played into the host system buffer (GetMixFormat 6ch/48k/32, shared init,
  48000 frames, Start/Stop `hr=0`); driver allocated a system MDL of 61,440 bytes,
  `SetState` 1->2->3, the copy timer advanced the position register, then
  PAUSE/ACQUIRE/STOP and `FreeAudioBuffer`.
- **Stage 2 (BAR metadata)**: clean, no crash. BARs mapped, config signature
  `0x19870423`, speaker buffer located at `0x12c000` / `0x61800`.
- **Stage 3 (BCE discovery)**: **HUNG.** The log ends at "BCE transport opened";
  `T2AudioSendBceMessage` blocked forever, `StartDevice` never returned, and the
  agent's `pnputil /restart-device` never completed. No bugcheck, no reboot, no new
  dump - a pure deadlock in the driver.

Root cause: the old `T2AudioSendBceMessage` waited on the completion event with an
**unbounded** `KeWaitForSingleObject`, so a lower `AppleUSBVHCI` IRP that is never
completed blocks the caller (and therefore all PnP for the device) forever. This is
exactly the "cancel-safe timeout" the increment-2 note deferred.

### CRASH-FIX INCREMENT 2b (2026-10-07) - bounded BCE send (abandon-in-place)

Fixes the stage-3 hang. `T2AudioSendBceMessage` no longer waits forever:

8. **Bounded wait.** The completion event is waited on with a 2 s timeout
   (`T2AUDIO_BCE_SEND_TIMEOUT_MS`). There is no cancellation attempt.
9. **Abandon-in-place, not cancel.** The event, `IO_STATUS_BLOCK`, and reply
   landing buffer are pool-allocated (`T2AUDIO_BCE_SEND_CONTEXT`, tag `'AbT2'`)
   instead of stack locals. On timeout the IRP is **abandoned in place** and the
   block is leaked (never freed) so a late completion can never touch recycled
   memory. The transport is then **poisoned** (`g_BceTransportPoisoned`): further
   sends fail fast with `STATUS_IO_TIMEOUT` and `T2AudioCloseBceTransport` does
   **not** drop the file-object reference, keeping the target device alive for the
   abandoned IRP. This is bounded to a single abandoned IRP.
10. **Reply copied through the pool buffer.** On success the reply is copied from
    the pool landing buffer into the caller's buffer, clamped to its size as before.

**Correction after review (2026-10-07).** The first 2b draft was wrong in three
ways and was reworked:

- It used a `FAST_MUTEX`, which raises IRQL to APC_LEVEL and so is illegal around
  `IoGetDeviceObjectPointer` (PASSIVE_LEVEL only). It is now a `KMUTEX`.
- It called `IoCancelIrp` after the timeout and then waited a 1 s grace period.
  But an IRP built by `IoBuildDeviceIoControlRequest` is freed by the I/O manager
  the instant it completes, so cancelling after a timeout races with that free and
  can dereference a recycled IRP (use-after-free). Cancellation was removed
  entirely; the IRP is simply leaked. `T2AUDIO_BCE_CANCEL_GRACE_MS` was deleted.
- The timeout cannot bound a lower dispatch routine that blocks *inside*
  `IoCallDriver` (nothing can); it only bounds the post-`IoCallDriver` wait. That
  is a lower-driver property we cannot fix from here.

Net effect: a non-completing BCE IRP now costs at most ~2 s in `StartDevice`, after
which the probe fails cleanly (`STATUS_IO_TIMEOUT`) and the device continues to
start instead of deadlocking PnP.

Separately, `T2AudioUnmapResources` now calls `KeFlushQueuedDpcs()` after clearing
`HardwareReady` and before clearing/unmapping the BARs. This drains any in-flight
copy DPC on all processors as a backstop; in the normal PnP flow no stream exists
while it runs (PortCls tears streams down first), so it only guards that invariant.

Verification: Debug x64 rebuild succeeds; both host suites still pass (45 + 48).
The timeout/abandon path itself is only reachable against the real lower driver, so
it must be re-verified on hardware with a freshly signed package.

### Review follow-up (2026-10-07) - bounded lock + WaveRT contract + agent cleanup

A second review pass ("check everything is OK") found four remaining issues, now
fixed:

11. **Bounded lock wait.** The transport `KMUTEX` was acquired with an infinite
    wait. If a send ever wedged while holding it, every later caller - including
    `StartDevice` and teardown - would block forever. Acquisition now waits at
    most `T2AUDIO_BCE_LOCK_TIMEOUT_MS` (send timeout + 1 s) and returns
    `STATUS_TIMEOUT`; `Open`/`Send` fail fast with `STATUS_IO_TIMEOUT` and `Close`
    logs and returns without touching the file object.
12. **WaveRT position-register `Accuracy` units.** `Accuracy` is a *byte* count,
    not a time. It was set to `COPY_PERIOD_MS * 10000` (100 ns units). It is now
    the worst-case byte error of one refresh period
    (`SampleRate * COPY_PERIOD_MS / 1000 * BytesPerFrame`, min one frame), and the
    clock-only `Numerator`/`Denominator` are left zero for a position register.
13. **`SetState` after a failed `STOP_IO`.** On a `STOP_IO` failure the timer had
    been stopped but the stream was left in `RUN`, and the error was returned
    mid-transition. It now logs, clears `HardwareIoStarted` (no retry against a
    poisoned transport) and still completes the transition.
14. **Test-agent cleanup order.** `tools/T2Audio-TestAgent.ps1` used to disable
    the device (a call that can hang on a stuck driver) *before* deleting the
    volatile `TestSession`. It now deletes the session first, then disables via a
    new `Set-DeviceStateBounded` (background job, 45 s hard timeout), so a hung
    PnP can never leave the session behind for the next boot to auto-start.
15. **`FreeAudioBuffer` no longer disturbs a live stream.** `T2AudioStreamReleaseBuffer`
    stopped the copy timer before checking whether the MDL was actually ours, so a
    foreign/unknown `FreeAudioBuffer` call could stop a running stream. It now
    returns before touching any state unless the MDL matches (or teardown forces
    release).
16. **Bounded PnP state changes + truthful cleanup.** The agent's `enable`,
    `restart` and `Recover` `disable` all go through `Set-DeviceStateBounded`;
    `enable`/`restart` timeouts now abort the test instead of being ignored.
    `Remove-TestSession` returns success/failure and the final message no longer
    claims the safe state when the session removal or disable failed.

Documented, non-fixable limits (unchanged): the 2 s send timeout starts *after*
`IoCallDriver` returns, so it cannot bound a lower driver that blocks *inside*
`IoCallDriver`; the bounded lock wait only keeps the *other* callers (PnP,
teardown) moving, it does not unblock the thread already stuck in the call.
The abandoned IRP is thread-bound (`IoBuildDeviceIoControlRequest`): the I/O
manager frees it on completion or when the thread exits, and the leaked context
is never referenced again, so both paths are safe at the cost of one leaked
context. There is still no PortCls remove/stop callback to tear a live stream
down on surprise-removal; `KeFlushQueuedDpcs` only guarantees that DPCs queued
*before* the call have run, so the no-live-timer invariant during
`T2AudioUnmapResources` rests on PortCls tearing streams down first (true on the
current StartDevice/error paths, where no stream exists yet).

Verification: Debug x64 rebuild succeeds; both host suites pass (45 + 48).

### STAGED HARDWARE TEST 2 - stage 3 passes (2026-10-07)

After re-signing the package from the corrected build (47,536 bytes, SHA256
`9CCB5728...`, INF `1.0.3.12`, published `oem158.inf`, installed copy hash-matched
by the agent), stage 3 was re-run on hardware (capture
`docs/logs/capture_agent_20261007_221001/`). The previous hang is gone:

- **No deadlock.** The BCE probe returned all 5 devices (`Digital Mic` 0x21,
  `Codec Output` 0x25, `Speaker` 0x39, `Codec Input` 0x3F, `Bridge Loopback`
  0x47) and the speaker id in ~60 ms, then closed the transport and continued to
  `StartDevice SUCCESS`. The `pnputil` enable/restart/disable all completed.
- **Mapping unchanged.** BAR0 buffer, config BAR signature `0x19870423` v3,
  `NumDevices=3`, speaker buffer `0x12c000` / `0x61800` as before.
- **Full render probe clean.** A 6ch/48k/32 WASAPI shared stream initialised
  (48,000 frames), `SetState` 1->2->3 with the copy timer running (1,007 ticks,
  position register advanced), then PAUSE/ACQUIRE/STOP and `FreeAudioBuffer`.
  Stage 3 leaves `EnableMmioCopy=0`, so no device memory was written.
- **Clean teardown.** The volatile session was removed and the device disabled;
  the machine was left with no `TestSession`, `T2AudioMiniport` stopped, device
  `CM_PROB_DISABLED`, and no new bugcheck/minidump.

### STAGED HARDWARE TEST 3 - stage 4 bugchecks (2026-10-07)

The corrected package (`9CCB5728...`, INF `1.0.3.12`, `oem158.inf`) was then run
at **stage 4** (capture `docs/logs/capture_agent_20261007_221322/`). Discovery
and `StartDevice` passed, but the machine bugchecked:

- **BSOD `0x7E` (`0xC0000005`) at 22:14:39** (WER event 1001; Kernel-Power 41 at
  22:14:20). The only capture file with content, `stdout.txt`, ends **mid-line**
  at `NewStream req size=104 major=73647561 sub=00000001 spec=05589F81
  wtag=65534 ch=6 rate=48000 bits=` (17:13:34.768 UTC, ~46 ms before the crash).
  DbgView is asynchronous and lost the tail, so whether `START_IO` was reached
  is **not confirmed**.
- **Dump unusable.** `C:\WINDOWS\MEMORY.DMP` was written only ~70%
  (1,368,409,827 bytes, `LastWriteTime` 22:14:27); `cdb` could not load it and
  WER reported it could not produce a minidump from the incomplete full dump.
  No minidump was created.
- **Safe state after reboot.** Device `CM_PROB_DISABLED`, `T2AudioMiniport`
  stopped (`Start=3`, Manual), no `TestSession`, still bound to `oem158.inf`.

**Difference stage 3 -> stage 4.** Stage 4 was the first run that (a) sets
`SpeakerDeviceId = BceSpeakerDeviceId`, (b) leaves the BCE transport open, and
(c) lets `SetState(RUN)` reach `T2AudioStartIo` -> BCE `START_IO`. Stage 3 did
none of these. The send path itself was therefore the prime suspect.

### Hardening after stage 4 (increment 4)

Three changes, none of which enable any new device access:

1. **Third opt-in gate `EnableBceIo`.** `T2AudioStreamSetState` now issues
   `START_IO`/`STOP_IO` only when the session sets `EnableBceIo=1` (agent
   `-BceIo`). At stage 4 with the gate off, the speaker is wired and the
   transport is held open but **no command is sent**, so the stream/transport
   setup can be proven safe before any I/O.
2. **IRQL logging + PASSIVE guard.** `SetState`, `START_IO`, `STOP_IO` log
   `KeGetCurrentIrql()`, and `T2AudioSendBceMessage` (plus both I/O helpers)
   refuse to run above `PASSIVE_LEVEL` (`STATUS_INVALID_DEVICE_STATE`) instead
   of taking a blocking Executive wait from a raised IRQL. This converts a
   possible illegal-wait bugcheck into a logged, clean failure.
3. **Reliable dump capture.** `CrashControl\CrashDumpEnabled` switched from `2`
   (kernel, which produced an unusable partial dump) to `3` (small memory dump)
   so the next crash reliably yields a `C:\Windows\Minidump\*.dmp` with the
   faulting stack.

**New package.** Debug x64 rebuild clean; both host suites pass (45 + 48).
Re-signed as `1.0.3.13`, published `oem159.inf`, installed copy hash-matched
(`B918EBEC...`). Device remains `CM_PROB_DISABLED`; binding alone does not start
the driver.

**Next staged test (requires separate consent).** Run stage 4 **without**
`-BceIo` first (wired, no I/O). Only if that is clean, run stage 4 **with**
`-BceIo`. Do not escalate to stage 5 until stage 4 with I/O is clean.

### STAGED HARDWARE TEST 4 - stage 4 without I/O passes (2026-10-07)

Package `1.0.3.13` / `oem159.inf` / `B918EBEC...` run at **stage 4 with
`EnableBceIo=0`** (capture `docs/logs/capture_agent_20261007_223141/`, 163
`T2Audio:` lines). Clean, no bugcheck:

- `test session active stage=4 mmioCopy=0 bceIo=0`, then `audio path ENABLED for
  speaker 0x39 (bceIo=0, transport held open)` - the speaker is wired and the
  BCE transport is held open across the whole stream lifecycle.
- Full 6ch/48k/32 render probe: `NewStream status=0`, `SetState` 1->2->3, copy
  timer 1,007 ticks, position register advanced to 45,696, then PAUSE/ACQUIRE/
  STOP. **No `START_IO`/`STOP_IO` lines** - the gate blocked them, as intended.
- **IRQL finding:** every `SetState` logged `irql=0` (PASSIVE_LEVEL), including
  the RUN edge. So the stage-4 crash was **not** a raised-IRQL blocking wait;
  `SetState` reaches `T2AudioStartIo` at PASSIVE. With the transport-open and
  speaker-wired setup now proven safe, the crash localizes to the BCE
  `START_IO` **send** itself (the only behavior still absent here).
- Clean teardown: volatile session removed, device `CM_PROB_DISABLED`, service
  stopped, no new minidump.

**Remaining (requires separate consent).** Run stage 4 **with** `-BceIo` to
exercise the guarded `START_IO`/`STOP_IO` send. If it still bugchecks, the new
small-dump setting should finally yield a usable minidump.

### Crash-fix plan (remaining increments)

The plan agreed with the user, in order: (1) a truly isolated software/RAM-only
mode and separate diagnostic stages (increment 3, done); (2) device/stream
lifecycle and teardown (increments 1-2 cover most); (3) WaveRT contract
hardening (increment 1 covers the state/buffer parts; the position-register and
MDL-lifetime questions remain); (4) BCE transport serialization/lifetime
(increments 2 and 2b, done - serialization plus the bounded, abandon-in-place send); (5) confirm PCM addressing
and rework the copy DPC; (6) staged verification with a usable kernel dump.
Hardware activation stays manual and requires consent.

---

## MACHINE STATE - DRIVER CANNOT LOAD (2026-10-07)

The driver kept starting (and crashing) on normal boot even with the in-driver
gates, because the gate code only exists in the new source build - the *bound*
driver was still an older package. The machine was therefore fixed at the PnP
and boot level, not only in code:

- **Device disabled.** `PCI\VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01\4&3AC8FC3&0&03D8`
  is now `CM_PROB_DISABLED` (code 22); its `ConfigFlags` is `0x1`
  (`CONFIGFLAG_DISABLED`), which persists across reboots. PnP will not start any
  driver for it.
- **Driver package removed.** `oem157.inf` (`t2audiominiport.inf`, T2AudioPort
  1.0.3.11) was exported then deleted from the driver store
  (`pnputil /delete-driver oem157.inf /uninstall`). No T2AudioPort package
  remains, so the device cannot rebind to it. AppleAudio (`oem16.inf`) is the
  only matching driver left.
- **Boot menu.** Windows Boot Manager now shows a 5-second menu with two entries:
  `Windows 11` (default, normal boot) and `Windows 11 - Safe Mode`
  (`safeboot Minimal`, GUID `{8a0b9978-c25e-11f1-b0d9-76fe467a64e1}`).
  `{bootmgr}.path` is a stale `\EFI\refind\refind_x64.efi` (the real file lives
  under `\EFI\refind\refind\`); it is left untouched because the system boots
  fine and bootmgfw ignores it.
- **Backups** (restorable): `local_backups\safety_<ts>\bcd_backup.bcd`
  (`bcdedit /import`), `bcd_enum.txt`, `exported_oem157\` (the removed package).

The test agent now refuses to enable the device unless the installed
`T2AudioMiniport.sys` matches `-ExpectedSysHash` (or `-AllowUnverified`), so an
old/bad build can no longer be activated by accident. To test a build: stage and
bind it (`Install-T2AudioDriver.ps1 -Bind`), then run
`T2Audio-TestAgent.ps1 -Mode Test -ExpectedSysHash <sha256>`.

**Update (2026-10-07, after stage-3 pass).** A new package (`oem158.inf`,
`t2audiominiport.inf`, T2AudioPort `1.0.3.12`, the corrected build) is now in the
driver store and bound to the device, which is still `CM_PROB_DISABLED`. The old
`oem157.inf` remains removed. Binding alone does not start the driver - the
volatile `TestSession` gate still controls activation.

**Update (2026-10-07, after the stage-4 bugcheck).** A newer package
(`oem159.inf`, `t2audiominiport.inf`, T2AudioPort `1.0.3.13`, sys
`B918EBEC...`) is now in the driver store and bound to the device, still
`CM_PROB_DISABLED`. `oem157.inf`/`oem158.inf` remain staged but unbootable while
the device is disabled. This build adds the `EnableBceIo` gate and the
PASSIVE-level guards described above.

---

---

## Executive Summary

`T2AudioStartDevice` completes with `STATUS_SUCCESS`. It registers a **Topology**
subdevice, then a **WaveRT** subdevice, and finally wires their bridge pins with
`PcRegisterPhysicalConnection` (render-endpoint structure). Audio **playback** remains
disabled by design (diagnostic mode), so the endpoint is not yet usable.

The diagnostic mode is enforced at multiple layers (see "Diagnostic-mode boundaries"
below), so no audio path, BCE transport, or hardware command can activate.

The **WaveRT buffer contract** is now implemented. In diagnostic mode
(`SpeakerDeviceId == 0`) the stream is served a host-side system-memory cyclic buffer
through `IPortWaveRTStream::AllocatePagesForMdl`; the zero-copy MMIO path
(`T2AudioCreateSpeakerMdl`, `MmWriteCombined`) activates only once a BCE speaker device
id exists. In diagnostic mode the hardware audio buffer is never handed to PortCls
(BAR1 is still mapped at init). Both paths are **compiled and statically checked but
not yet exercised at runtime**; the pure size/validation/release-decision helpers are
covered by a host-side unit test (`tests/T2AudioBufferLogicTest.c`, 20 assertions).

The **BCE protocol parsing** is now hardened and the speaker-discovery logic is
implemented. Replies are validated as a whole before any field is read: the response
type byte must be RESPONSE, the protocol status must be zero, the message id must match
the request, and the advertised count/data size must fit within the bytes actually
returned (`BceProtocolLogic.h:T2AudioBceParseDeviceListResponse` /
`T2AudioBceParsePropertyResponse`). The reply length reported by the transport is
clamped to the supplied buffer so a larger `Information` value cannot cause an
out-of-bounds read. The BCE IOCTL constant is fixed to the verified `0x222018`
(`CTL_CODE(FILE_DEVICE_UNKNOWN, 0x0806, ...)`, previously mis-coded as `0x22A018`) and is
guarded by a `C_ASSERT`. `T2AudioFindSpeakerDeviceId` walks the device list and matches
the speaker by UID — `GET_PROPERTY(GLOBAL, UID, element 0)` per device, exact
case-sensitive compare against `"Speaker"` — instead of guessing `deviceList[0]`, and
verifies that the reply echoes the requested device/scope/selector.

A **diagnostic-only discovery probe** (`T2AudioProbeBceDevices`) is called at the end of
`T2AudioStartDevice`. It opens the BCE transport, enumerates devices, logs each UID, and
records the speaker id in a **separate** field (`Context->BceSpeakerDeviceId`); it then
closes the transport. It never touches `Context->SpeakerDeviceId`, so the audio path
(MMIO buffer, `StartIo`/`StopIo`) stays disabled. `Driver.c`/`Device.c` continue to
hard-code `SpeakerDeviceId = 0`. The pure byte-access/length/UID/parser helpers live in
`src/BceProtocolLogic.h` and are covered by a host-side unit test
(`tests/BceProtocolLogicTest.c`, 38 assertions).

The probe and the discovery logic were **validated on real hardware on 2026-10-07**
(Debug build installed as `oem157.inf`, device Status `Started`; capture
`docs/logs/capture_20261007_165851/`). The BCE transport opened over
`\Device\AppleUSBVHCI`, enumerated 5 devices, and read each UID:
`0x21 "Digital Mic"`, `0x25 "Codec Output"`, **`0x39 "Speaker"`**, `0x43 "Codec Input"`,
`0x47 "Bridge Loopback"`. The speaker id resolved to **`0x39`**, which matches the
hard-coded resource-table path (`Device[1] Name='Speaker'`, buffer `0x12c000`, size
`0x61800`) — the two independent discovery methods agree. The audio path still stays
disabled (`SpeakerDeviceId == 0`).

**Build reproducibility caveat:** The MSBuild link step is **not deterministic** —
two consecutive rebuilds of identical source produce different SHA256 hashes (the PE
`TimeDateStamp` and the CodeView PDB GUID/age differ; 21 bytes in the Release image).
Therefore a hash mismatch between a local build and an installed/package binary is
**not by itself** evidence of different source.

---

## Component Status

| Component | Status | Notes |
|-----------|--------|-------|
| DriverEntry | Working | `PcInitializeAdapterDriver` succeeds |
| AddDevice | Working | `PcAddAdapterDevice` succeeds |
| StartDevice | Working | Returns `STATUS_SUCCESS` |
| MapResources | Working | 3 memory resources; Resource[2] selected as config |
| FindSpeakerBuffer | Working | Speaker buffer `0x12c000`, size `0x61800` |
| Topology Port/Miniport | Registered | `PcRegisterSubdevice(..., L"Topology", ...)` |
| WaveRT Port/Miniport | Registered | `PcRegisterSubdevice(..., L"Wave", ...)` |
| Audio Endpoint | Structure registered | Bridge pins physically connected; playback still blocked, not yet verified on hardware |
| BCE Transport | Validated on hardware (read-only) | Reply parsing hardened; IOCTL `0x222018`; probe opened transport, read 5 UIDs, resolved Speaker `0x39`; `SpeakerDeviceId` still 0, audio path stays off |
| Audio I/O (StartIo/StopIo) | Gated, retest pending | Runs only when `SpeakerDeviceId != 0` **and** `EnableBceIo=1`; refuses above PASSIVE_LEVEL. Stage-4 no-I/O run is clean (setup proven safe, `SetState` at PASSIVE); the guarded `START_IO` send is the next test |
| Audio Playback | Not implemented | Out of scope |

---

## Current Driver State on the Test Machine

On **2026-10-07** the signed Debug build was installed for the BCE hardware test
(no reboot — `pnputil /add-driver /install` + `/scan-devices` rebound the device in
place):

| Property | Value |
|----------|-------|
| Device | `PCI\VEN_106B&DEV_1803...` -> "Apple T2 Audio Device (6-channel Native Driver)", Class MEDIA, Status `Started`, Problem `CM_PROB_NONE` |
| Driver | `T2AudioMiniport.sys` (signed Debug, 40,368 bytes) |
| Package | `packaging/T2AudioMiniport.inf` (DriverVer `10/07/2026,1.0.1.0`) published as `oem157.inf` |
| Capture | `docs/logs/capture_20261007_165851/` (54 `T2Audio:` lines) |

**Current state (2026-10-07, after the stage-4 bugcheck):** package
`oem159.inf` / T2AudioPort `1.0.3.13` (sys `B918EBEC...`) is staged and bound to
the device, which is **`CM_PROB_DISABLED`** (code 22); service `T2AudioMiniport`
is `Start=3` (Manual) and **Stopped**; no `TestSession` exists. `oem157.inf` and
`oem158.inf` remain staged but cannot start while the device is disabled.

The original Apple driver had been restored on 2026-10-06 with
`tools/Restore-AppleAudioDriver.ps1` (Brigadier, Boot Camp `061-62383`), where
`AppleAudio.sys` (112,512 bytes) was published as `oem16.inf`. `oem16.inf` is still
present in the driver store; re-bind the Apple driver with
`tools/Restore-AppleAudioDriver.ps1` (or `pnputil /update-driver`) to revert.

The earlier custom-driver capture (Part 1 evidence) remains in
`docs/logs/capture_20261006_023822/` (43 `T2Audio:` lines); sanitized excerpt at
`docs/logs/EXCERPT_20261006_023822.md`.

---

## Diagnostic-mode boundaries (Part 4, static verification)

The following gates keep the driver in diagnostic mode. All verified by static reading
of the current source:

1. `T2AudioMapResources` sets `Context->SpeakerDeviceId = 0` unconditionally
   (`Device.c:220`) — the `BufferStruct` contract has no device-id field.
2. `T2AudioStartDevice` also sets `context->SpeakerDeviceId = 0` (`Driver.c:190`).
3. `T2AudioFindSpeakerDeviceId` is now implemented (`BceTransport.c`) — it enumerates the
   BCE device list and matches the speaker by UID (`GET_PROPERTY(GLOBAL, UID)`, exact
   `"Speaker"` compare). `T2AudioProbeBceDevices` calls it from `T2AudioStartDevice` for
   diagnostics, but only records the result in `Context->BceSpeakerDeviceId` and closes
   the transport. `Driver.c:190` and `Device.c:220` still hard-code `SpeakerDeviceId = 0`,
   so no device id is ever derived for the audio path. Reply parsing rejects a mismatched
   message id, a non-zero protocol status, a non-response type, and a count/data size that
   exceeds the returned bytes (`BceProtocolLogic.h`).
4. `T2AudioCreateSpeakerMdl` zeroes outputs and returns `STATUS_NOT_SUPPORTED` when
   `SpeakerDeviceId == 0` (`Phase4.c:37-43`).
5. `T2AudioStartIo` (`Phase4.c:119`) and `T2AudioStopIo` (`Phase4.c:168`) return
   `STATUS_NOT_SUPPORTED` under the same condition.
6. `T2AudioStreamAllocateAudioBuffer` no longer refuses the stream. In diagnostic
   mode (`SpeakerDeviceId == 0`) it allocates an ordinary system-memory cyclic buffer
   via `IPortWaveRTStream::AllocatePagesForMdl` (`MmCached`), verifying the allocated
   byte count and rounding up to a whole frame so `ActualSize >= RequestedSize`. The
   zero-copy MMIO path (`T2AudioCreateSpeakerMdl`, `MmWriteCombined`) is only taken
   once a BCE speaker device id exists and only if the fixed hardware buffer can
   satisfy the request. In diagnostic mode the hardware buffer is never handed to
   PortCls (BAR1 is still mapped at init). Buffer release is centralized in
   `T2AudioStreamReleaseBuffer`: `T2AudioStreamFreeAudioBuffer` releases only the MDL
   it handed out (a NULL, foreign, or already-released MDL is ignored), and
   `T2AudioStreamRelease` releases any buffer still held at teardown as a defensive
   backstop (safe because the WaveRT port stream outlives the miniport stream).
7. `T2AudioStreamSetState` validates `KSSTATE_STOP..KSSTATE_RUN`. It tracks whether
   hardware I/O actually started (`HardwareStarted`): RUN starts I/O only if not
   already running (and requires a buffer, and refuses hardware I/O when a system
   buffer is paired with a non-zero device id); PAUSE/ACQUIRE/STOP stop I/O only if it
   was started, so a failed/blocked RUN is never paired with a spurious STOP
   (`WaveRTStream.c:75`). The underlying `StartIo`/`StopIo` are still blocked (items 4,
   5).
8. `T2AudioMiniportNewStream` rejects capture and any pin other than the render
   sink (`WaveRTMiniport.c:276`).

The WaveRT and Topology filters **are** now wired together via
`PcRegisterPhysicalConnection` (`Driver.c:198`), so the physical connection is no
longer a diagnostic gate. The WaveRT buffer is a host-side system buffer (item 6).
Playback stays off because the audio-I/O gates (items 4, 5, 7) still block
`StartIo`/`StopIo`, so no audio reaches the hardware. No BCE transport and no audio
path are active.

---

## Pin scheme

The WaveRT / Topology pin, node, and connection layout is documented in
`docs/WAVERT_TOPOLOGY_PINS.md`. It now describes the **implemented** structure: a
WaveRT render sink pin (PCM) plus a WaveRT bridge pin, a Topology bridge pin plus a
speaker pin (no nodes), and the physical connection between the two bridge pins.

---

## Packaging consolidation (Part 3)

- `packaging/` is the single canonical package directory.
- Duplicate binaries removed from `tools/` (`T2AudioMiniport.sys`, `.inf`, `t2audiominiport.cat`)
  and the unused `src/Driver_WaveRTFirst_BACKUP.c` (was tracked, not in the project).
- `tools/Install-T2AudioDriver.ps1`, `tools/Rollback-T2AudioDriver.ps1`,
  `tools/PreInstall-Check.ps1` now resolve paths relative to `$PSScriptRoot`
  (`..\packaging`, `..\Backup`).
- `packaging/t2audio.cdf` uses relative paths.
- `tools/INSTALL_SCRIPTS_README.md` updated accordingly.

---

## C++ conversion (Part 2)

`Driver.c` and `Device.c` are compiled as C++ (`CompileAsCpp` per file, Debug|x64 and
Release|x64). `DriverEntry`/`T2AudioAddDevice` are wrapped in `extern "C"`; vtable calls
use direct virtual dispatch; `IResourceList` methods are called directly. C4133 is
eliminated in both configurations. Remaining warnings are non-blocking (C4152 vtable
function/data pointer conversion, C4189 unused locals, C4996 deprecated pool API,
C4100 unused params, C4115 from WDK headers).

---

## Build

- Toolchain: MSBuild 18.10.1, WDK `10.0.28000.0`, VS 18 Community.
- Command (from `src\`):
  `MSBuild.exe T2AudioMiniport.vcxproj /p:Configuration=Release /p:Platform=x64 /t:Rebuild`
- Output: `src\bin\Release\T2AudioMiniport.sys` (~22,528 bytes).
- Debug output: `src\bin\Debug\T2AudioMiniport.sys` (~38,912 bytes).
- Host unit test (pure buffer helpers):
  `cl /nologo /W4 /Fe:tests\T2AudioBufferLogicTest.exe tests\T2AudioBufferLogicTest.c`
  then run `tests\T2AudioBufferLogicTest.exe` (all assertions pass).
- Host unit test (pure BCE protocol helpers):
  `cl /nologo /W4 /Fe:tests\BceProtocolLogicTest.exe tests\BceProtocolLogicTest.c`
  then run `tests\BceProtocolLogicTest.exe` (all assertions pass).
- Only Debug builds emit `KdPrint` output (`DBG` is not defined in Release).
- Builds are non-reproducible (see Executive Summary); do not compare hashes across rebuilds.

---

## Known Limitations

1. Endpoint structure is registered (WaveRT and Topology filters wired) but not yet
   verified on hardware; playback remains disabled in diagnostic mode.
2. WaveRT buffer contract is implemented (size/overflow checks, byte-count
   verification, ownership-safe free, teardown cleanup, state tracking) and its pure
   helpers are unit-tested on the host, but the kernel paths (allocator, MMIO, I/O)
   have not been exercised at runtime — only builds and static checks have run. The
   zero-copy MMIO path is unverified until BCE transport works.
3. BCE transport is not wired into the audio path; the speaker device id is discovered
   only diagnostically (`Context->BceSpeakerDeviceId`) and `SpeakerDeviceId` stays 0.
   The discovery probe and reply parsers are host-tested (38 assertions) and were
   exercised against real BCE replies on hardware (2026-10-07): the transport opened,
   five devices were enumerated, and Speaker resolved to `0x39`, agreeing with the
   resource-table path. Wiring `SpeakerDeviceId` into the audio path is the next step.
4. Audio I/O blocked in diagnostic mode.
5. Single fixed format: 48 kHz / 6 channel / 32-bit container (24-byte frame).
6. No volume control, power management, or hotplug handling.
7. `C4152` vtable warnings not resolved (unrelated to C4133, which is fixed).

---

## Git

- Remote: `origin` — `https://github.com/imacorpoliz/t2audiowindows`
- Branch: `diagnostics`
- Do not force-push. Never commit secrets, PFX/PVK, full kernel logs, or Apple binaries.
  Full kernel logs stay local; only sanitized excerpts go to Git.

---

## Device

- Model: MacBookPro16,1 (2019), Windows 11
- Instance: `PCI\VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01\4&3AC8FC3&0&03D8`
- Boot config: `testsigning=Yes`, `nointegritychecks=Yes`, `loadoptions=DISABLE_INTEGRITY_CHECKS`
