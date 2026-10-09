#ifndef T2AUDIO_BUFFER_LOGIC_H
#define T2AUDIO_BUFFER_LOGIC_H

// Pure, dependency-free helpers for the WaveRT buffer contract. These are kept
// free of kernel APIs (only `unsigned long`, which matches ULONG) so the same
// code can be compiled and exercised by a host-side test
// (tests/T2AudioBufferLogicTest.c).

// Round RequestedSize up to a whole number of BytesPerFrame. Returns 1 on
// success and writes *AlignedSize (>= RequestedSize), or 0 on invalid input or
// arithmetic overflow.
static __inline int
T2AudioAlignSizeUpToFrame(unsigned long RequestedSize,
                          unsigned long BytesPerFrame,
                          unsigned long *AlignedSize)
{
    unsigned long remainder;
    unsigned long pad;

    if (AlignedSize == 0 || BytesPerFrame == 0 || RequestedSize == 0) {
        return 0;
    }

    remainder = RequestedSize % BytesPerFrame;
    if (remainder == 0) {
        *AlignedSize = RequestedSize;
        return 1;
    }

    pad = BytesPerFrame - remainder;
    if (RequestedSize > 0xFFFFFFFFul - pad) {
        return 0;
    }

    *AlignedSize = RequestedSize + pad;
    return 1;
}

// Returns 1 if a fixed hardware buffer of ActualSize bytes can satisfy a client
// request for RequestedSize bytes and is frame-aligned; otherwise 0.
static __inline int
T2AudioHardwareBufferSatisfies(unsigned long RequestedSize,
                               unsigned long ActualSize,
                               unsigned long BytesPerFrame)
{
    if (BytesPerFrame == 0) {
        return 0;
    }
    return (ActualSize >= RequestedSize) &&
           ((ActualSize % BytesPerFrame) == 0);
}

// Returns 1 if a metadata table consisting of a HeaderSize-byte header followed
// by NumDevices entries of DeviceStride bytes fits entirely inside an
// AvailableSize-byte region; 0 otherwise (including NumDevices > MaxDevices).
// Used to validate the T2 buffer metadata table before walking it, so a bogus
// device count can never make the driver read past the mapped BAR. Pure helper
// so the bounds check is host-testable.
static __inline int
T2AudioDeviceTableFits(unsigned long long NumDevices,
                       unsigned long long MaxDevices,
                       unsigned long long HeaderSize,
                       unsigned long long DeviceStride,
                       unsigned long long AvailableSize)
{
    unsigned long long need;

    if (NumDevices > MaxDevices) {
        return 0;
    }
    need = HeaderSize + NumDevices * DeviceStride;
    return need <= AvailableSize;
}

// Returns 1 if a stream entering RUN must issue START_IO to the hardware,
// otherwise 0. Hardware I/O is started only in the real device path: a wired
// speaker device id, no ForceSystemBuffer override, and not already running. A
// forced system-buffer (pure software) stream must never touch the device, and a
// stream that is already running must not send START_IO twice.
static __inline unsigned long
T2AudioDecideHardwareIo(unsigned long ForceSystemBuffer,
                        unsigned long SpeakerWired,
                        unsigned long HardwareIoStarted)
{
    if (ForceSystemBuffer || !SpeakerWired || HardwareIoStarted) {
        return 0;
    }
    return 1;
}

// Returns 1 if leaving RUN must issue STOP_IO, otherwise 0. A stream only stops
// hardware I/O that it actually started, so a forced/software stream never
// sends a STOP that has no matching START.
static __inline unsigned long
T2AudioDecideStopHardwareIo(unsigned long HardwareIoStarted)
{
    return HardwareIoStarted ? 1u : 0u;
}

// Returns 1 if the copy timer must be (re)started when a stream enters RUN:
// there is a system buffer to mirror/advance and the timer is not already
// active. The DPC advances the play cursor in every mode, so this is true even
// when device-memory writes are disabled.
static __inline unsigned long
T2AudioShouldStartCopyTimer(unsigned long HasSystemBuffer,
                            unsigned long CopySize,
                            unsigned long TimerActive)
{
    return (HasSystemBuffer && CopySize != 0 && !TimerActive) ? 1u : 0u;
}

// How a currently held audio buffer should be released.
#define T2AUDIO_RELEASE_NONE     0u
#define T2AUDIO_RELEASE_SYSTEM   1u
#define T2AUDIO_RELEASE_HARDWARE 2u

// Decide the release action for a held buffer.
//   held            - a buffer is currently held
//   hardware        - the held buffer is the hardware (MMIO) buffer
//   incomingMatches - the MDL handed to FreeAudioBuffer equals the held MDL
//                     (pass 1 when releasing unconditionally, e.g. at teardown)
// A foreign, unknown, or already-released MDL maps to T2AUDIO_RELEASE_NONE so
// it is never routed to an allocator.
static __inline unsigned long
T2AudioDecideBufferRelease(unsigned long held,
                           unsigned long hardware,
                           unsigned long incomingMatches)
{
    if (!held || !incomingMatches) {
        return T2AUDIO_RELEASE_NONE;
    }
    return hardware ? T2AUDIO_RELEASE_HARDWARE : T2AUDIO_RELEASE_SYSTEM;
}

#endif
