#ifndef T2AUDIO_MIC_LOGIC_H
#define T2AUDIO_MIC_LOGIC_H

// Pure, dependency-free helpers for the built-in microphones ("Digital Mic",
// ROADMAP section 5): the input format, kaiT2en's 16_1 microphone DSP and
// the capture clock. Integer-only and free of kernel APIs, so
// tests/T2AudioMicTest.c runs the same code on the host.
//
// Hardware (ExtendedProbe, 2026-10-28): input stream 0x22, 48 kHz, 3
// channels, 24 bits in 32-bit slots (12 bytes a frame), flags 0x4 (signed
// integer, low-aligned). Unlike the speakers the ring is host memory the
// driver allocates and registers (SET_INPUT_STREAM_ADDRESS_RANGES, msg 18),
// 16640 frames long as in kaiT2en (t2audio_init_bs_stream_host).
//
// Microphone DSP (kaiT2en dsp/profiles/16_1/mic.json, GPL-3.0-or-later):
//   mono = 0.5 * mic0 + 0.5 * mic1   (mixer "bf"; mic2 is not used)
//   hp   = biquad high-pass ~20 Hz   ("hp", 48 kHz coefficients)
//   out  = 89.125 * hp               ("out", +39 dB)

#define T2AUDIO_MIC_CHANNELS          3u
#define T2AUDIO_MIC_BYTES_PER_FRAME   (T2AUDIO_MIC_CHANNELS * 4u)
#define T2AUDIO_MIC_RING_FRAMES       16640u
#define T2AUDIO_MIC_RING_BYTES        (T2AUDIO_MIC_RING_FRAMES * T2AUDIO_MIC_BYTES_PER_FRAME)

// How far the capture read position trails the T2 write position, and how
// far it may wander before it is re-aimed (frames at 48 kHz). 512 frames
// (~10.7 ms) keeps clear of the T2's write burst; the window absorbs the
// 2 ms copy tick and timestamp jitter.
#define T2AUDIO_MIC_CAPTURE_LAG       512
#define T2AUDIO_MIC_CAPTURE_TOLERANCE 384

// mic.json "hp" at 48 kHz, Q28 { b0, b1, b2, a1, a2 }.
static const int g_T2AudioMicHp48k[5] = {
    266455085, -532910170, 266455085, -532895559, 264489324
};
// mic.json "out": 89.12509381337455 in Q16.
#define T2AUDIO_MIC_OUT_GAIN_Q16      5840902ll

// Internal precision: samples are processed as 24-bit values << 6.
#define T2AUDIO_MIC_SHIFT             6

typedef struct _T2AUDIO_MIC_STATE {
    long long X1, X2, Y1, Y2;
    long long Err;     // error feedback of the >> 28 (keeps the 20 Hz pole clean)
} T2AUDIO_MIC_STATE;

static __inline void
T2AudioMicReset(T2AUDIO_MIC_STATE *S)
{
    S->X1 = S->X2 = S->Y1 = S->Y2 = S->Err = 0;
}

// One 24-bit sample from its 32-bit slot (low-aligned, sign in bit 23).
static __inline int
T2AudioMicSample24(unsigned int Raw)
{
    unsigned int v = Raw & 0x00FFFFFFu;

    return (v & 0x00800000u) ? (int)v - 0x01000000 : (int)v;
}

static __inline long long
T2AudioMicSar(long long v, unsigned int s)
{
    // Arithmetic shift right that does not depend on the compiler's
    // behaviour for negative numbers.
    return v >= 0 ? (v >> s) : -((-v - 1) >> s) - 1;
}

// One frame of the microphone DSP. Frame points at 3 raw slots. Dsp = 0
// passes mic 0 through unprocessed (diagnostics). Returns a 32-bit
// MSB-aligned PCM sample (what Windows expects for 32-bit PCM).
static __inline int
T2AudioMicProcessFrame(T2AUDIO_MIC_STATE *S, const unsigned int *Frame, unsigned int Dsp)
{
    long long x, acc, y, out;

    if (!Dsp) {
        return (int)((unsigned int)T2AudioMicSample24(Frame[0]) << 8);
    }
    // Beam: average of mic 0 and mic 1 (24-bit range), then extra precision.
    x = ((long long)T2AudioMicSample24(Frame[0]) + (long long)T2AudioMicSample24(Frame[1]));
    x = x * (1ll << (T2AUDIO_MIC_SHIFT - 1));
    acc = (long long)g_T2AudioMicHp48k[0] * x +
          (long long)g_T2AudioMicHp48k[1] * S->X1 +
          (long long)g_T2AudioMicHp48k[2] * S->X2 -
          (long long)g_T2AudioMicHp48k[3] * S->Y1 -
          (long long)g_T2AudioMicHp48k[4] * S->Y2 +
          S->Err;
    y = T2AudioMicSar(acc, 28);
    S->Err = acc - y * (1ll << 28);
    S->X2 = S->X1;
    S->X1 = x;
    S->Y2 = S->Y1;
    S->Y1 = y;
    // +39 dB, back to 24 bits, saturate.
    out = T2AudioMicSar(y * T2AUDIO_MIC_OUT_GAIN_Q16, 16 + T2AUDIO_MIC_SHIFT);
    if (out > 0x7FFFFF) {
        out = 0x7FFFFF;
    } else if (out < -0x800000) {
        out = -0x800000;
    }
    return (int)((unsigned int)(int)out << 8);
}

// Capture clock. Host frame H (the capture cursor reported to Windows) reads
// T2 frame H + *Offset, i.e. ring slot (H + *Offset) % RingFrames. WriteFrame
// is the T2's current write position (from UPDATE_TIMESTAMP, interpolated).
// When the reader is not between LAG +- TOLERANCE frames behind the writer
// (first anchor, drift, a stall), *Offset is re-aimed to exactly LAG behind
// and 1 is returned. Offsets are signed: T2 frames restart at 0 on START_IO.
static __inline int
T2AudioMicCaptureAim(unsigned long long HostFrame, long long *Offset,
                     unsigned long long WriteFrame, int Valid)
{
    long long lag;

    if (Offset == 0) {
        return 0;
    }
    lag = (long long)WriteFrame - ((long long)HostFrame + *Offset);
    if (Valid && lag >= T2AUDIO_MIC_CAPTURE_LAG - T2AUDIO_MIC_CAPTURE_TOLERANCE &&
        lag <= T2AUDIO_MIC_CAPTURE_LAG + T2AUDIO_MIC_CAPTURE_TOLERANCE) {
        return 0;
    }
    *Offset = (long long)WriteFrame - T2AUDIO_MIC_CAPTURE_LAG - (long long)HostFrame;
    return 1;
}

// Ring slot of a T2 frame (negative frames, before the T2 started, map to 0
// and are silence for the caller).
static __inline unsigned long
T2AudioMicRingSlot(long long T2Frame, unsigned long RingFrames)
{
    if (T2Frame < 0 || RingFrames == 0) {
        return 0;
    }
    return (unsigned long)((unsigned long long)T2Frame % RingFrames);
}

#endif
