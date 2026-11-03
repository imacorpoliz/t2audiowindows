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

// Headset microphone ("Codec Input", Parameters\HeadsetMicInput; AppleAudio's
// WaveMicIn): input stream 0x45, 48 kHz, mono signed 32-bit, flags 0xC,
// device input latency 47 frames, safety offset 111 (ExtendedProbe,
// 2026-10-28). Same ring length as the built-in microphones.
#define T2AUDIO_HEADSET_MIC_BYTES_PER_FRAME 4u
#define T2AUDIO_HEADSET_MIC_RING_FRAMES     T2AUDIO_MIC_RING_FRAMES
#define T2AUDIO_HEADSET_MIC_RING_BYTES      (T2AUDIO_HEADSET_MIC_RING_FRAMES * T2AUDIO_HEADSET_MIC_BYTES_PER_FRAME)

// How far the capture read position trails the T2 write position, and how
// far it may wander before it is re-aimed (frames at 48 kHz). 512 frames
// (~10.7 ms) keeps clear of the T2's write burst; the window absorbs the
// 2 ms copy tick and timestamp jitter.
// MacBookPro16,1 3-microphone array physical geometry (ROADMAP 9.3)
#define T2AUDIO_MIC_ARRAY_COUNT       3u
#define T2AUDIO_MIC_ARRAY_FREQ_LO     50u     // 50 Hz
#define T2AUDIO_MIC_ARRAY_FREQ_HI     20000u  // 20 kHz
#define T2AUDIO_MIC_ARRAY_WORK_ANGLE  15708   // pi/2 radians * 10000 (90 deg)
#define T2AUDIO_MIC0_X_MM             -40     // Left mic: -40mm
#define T2AUDIO_MIC1_X_MM             0       // Center mic: 0mm
#define T2AUDIO_MIC2_X_MM             40      // Right mic: +40mm

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

// One headset microphone frame: the slot already holds signed 32-bit PCM
// (T2AudioBceIsMonoS32Input), so it passes unchanged; there is no DSP on
// this path.
static __inline int
T2AudioHeadsetMicSample(unsigned int Raw)
{
    return (int)Raw;
}

// What a jack pin reports to Windows. Until a jack read has succeeded once
// (Seen), or without jack tracking, it reads "connected" so the endpoint
// stays selectable; after that it follows the jack.
static __inline int
T2AudioJackReportConnected(unsigned int JackEvents, int Seen, long Plugged)
{
    if (!JackEvents || !Seen) {
        return 1;
    }
    return Plugged != 0;
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

// ---------------------------------------------------------------------------
// Built-in microphone noise gate (Parameters\MicNoiseSuppression=1, default 0).
// A smooth, integer-only downward expander applied to the mono output sample
// produced by T2AudioMicProcessFrame. With Enable=0 the sample is returned
// bit-exact and the gate state is left untouched, so the default build is a
// strict bypass.
//
// Detector: a peak envelope (instant rise, exponential fall with a ~21 ms
// time constant) instead of the instantaneous sample, so zero crossings of a
// continuous signal do not register as silence. The envelope is compared
// against two thresholds with hysteresis: OPEN (-40 dBFS) opens the gate,
// and the gate starts closing only after the envelope has stayed at or below
// CLOSE (-50 dBFS) for HOLD samples (100 ms), which bridges short pauses
// between words.
//
// Gain: Q16 (0 = closed .. 65536 = open), ramped linearly by ATTACK_STEP per
// sample while opening (~2 ms) and RELEASE_STEP while closing (~150 ms). A
// bounded slew cannot click, and the gain reaches exactly 0 / 65536.
// Everything is O(1) per sample with no division, safe for DISPATCH_LEVEL.
#define T2AUDIO_MIC_GATE_GAIN_FULL       65536u
#define T2AUDIO_MIC_GATE_ATTACK_STEP     683u      // ~2 ms to full at 48 kHz
#define T2AUDIO_MIC_GATE_RELEASE_STEP    9u        // ~150 ms to close at 48 kHz
#define T2AUDIO_MIC_GATE_OPEN_THRESHOLD  21474836u // -40 dBFS of 2^31
#define T2AUDIO_MIC_GATE_CLOSE_THRESHOLD 6791886u  // -50 dBFS of 2^31
#define T2AUDIO_MIC_GATE_ENV_SHIFT       10u       // envelope fall: tau = 1024 samples (~21 ms)
#define T2AUDIO_MIC_GATE_HOLD_SAMPLES    4800u     // 100 ms at 48 kHz

typedef struct _T2AUDIO_MIC_GATE_STATE {
    unsigned int Gain;   // Q16 0..65536: current gate gain
    int Open;            // current target: 1 = opening, 0 = closing
    unsigned int Env;    // peak envelope of |sample|, 0..2^31
    unsigned int Hold;   // samples left before a quiet envelope closes the gate
} T2AUDIO_MIC_GATE_STATE;

static __inline void
T2AudioMicGateReset(T2AUDIO_MIC_GATE_STATE *G)
{
    G->Gain = 0;
    G->Open = 0;
    G->Env = 0;
    G->Hold = 0;
}

// One mono sample through the noise gate. Sample is the MSB-aligned 32-bit
// PCM value produced by T2AudioMicProcessFrame; Enable=0 is a bit-exact pass.
static __inline int
T2AudioMicNoiseGate(T2AUDIO_MIC_GATE_STATE *G, int Sample, unsigned int Enable)
{
    unsigned int mag;
    long long out;

    if (!Enable || G == NULL) {
        return Sample;
    }
    // |INT_MIN| = 2^31 still fits in an unsigned int.
    mag = (Sample < 0) ? (0u - (unsigned int)Sample) : (unsigned int)Sample;

    // Peak envelope: instant attack, exponential release.
    if (mag >= G->Env) {
        G->Env = mag;
    } else {
        G->Env -= G->Env >> T2AUDIO_MIC_GATE_ENV_SHIFT;
        if (G->Env < mag) {
            G->Env = mag;
        }
    }

    // Hysteresis + hold on the envelope.
    if (G->Env >= T2AUDIO_MIC_GATE_OPEN_THRESHOLD) {
        G->Open = 1;
        G->Hold = T2AUDIO_MIC_GATE_HOLD_SAMPLES;
    } else if (G->Env > T2AUDIO_MIC_GATE_CLOSE_THRESHOLD) {
        if (G->Open) {
            G->Hold = T2AUDIO_MIC_GATE_HOLD_SAMPLES;
        }
    } else if (G->Hold > 0) {
        G->Hold--;
    } else {
        G->Open = 0;
    }

    if (G->Open) {
        unsigned int g = G->Gain + T2AUDIO_MIC_GATE_ATTACK_STEP;

        G->Gain = (g > T2AUDIO_MIC_GATE_GAIN_FULL) ? T2AUDIO_MIC_GATE_GAIN_FULL : g;
    } else {
        G->Gain = (G->Gain > T2AUDIO_MIC_GATE_RELEASE_STEP)
                    ? (G->Gain - T2AUDIO_MIC_GATE_RELEASE_STEP) : 0u;
    }
    if (G->Gain == T2AUDIO_MIC_GATE_GAIN_FULL) {
        return Sample;
    }
    out = (long long)Sample * (long long)G->Gain;
    return (int)T2AudioMicSar(out, 16);
}

#endif
