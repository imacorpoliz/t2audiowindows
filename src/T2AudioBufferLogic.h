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
