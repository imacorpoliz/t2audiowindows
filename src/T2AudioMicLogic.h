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
// Parameters\MicGainDb (0..48, default 39 = the kaiT2en value above).
#define T2AUDIO_MIC_GAIN_DB_DEFAULT   39u
#define T2AUDIO_MIC_GAIN_DB_MAX       48u
// 10^(dB/20) in Q16 for 0..48 dB (generated).
static const long long g_T2AudioMicGainQ16[49] = {
    65536, 73533, 82505, 92572, 103868, 116541, 130762, 146717,
    164619, 184706, 207243, 232531, 260904, 292739, 328458, 368536,
    413504, 463959, 520571, 584090, 655360, 735326, 825049, 925721,
    1038676, 1165413, 1307615, 1467168, 1646190, 1847055, 2072430, 2325305,
    2609035, 2927386, 3284581, 3685360, 4135042, 4639593, 5205710, 5840902,
    6553600, 7353260, 8250494, 9257206, 10386756, 11654132, 13076151, 14671683,
    16461899,
};

// Internal precision: samples are processed as 24-bit values << 6.
#define T2AUDIO_MIC_SHIFT             6

static __inline long long
T2AudioMicSar(long long v, unsigned int s)
{
    // Arithmetic shift right that does not depend on the compiler's
    // behaviour for negative numbers.
    return v >= 0 ? (v >> s) : -((-v - 1) >> s) - 1;
}

// ---------------------------------------------------------------------------
// Three-microphone noise canceller (Parameters\MicDsp=2): one microphone
// carries the voice, the other two together give the noise reference for
// what the speakers (and other off-axis sources) put into it. An adaptive
// noise canceller in integer arithmetic:
//
//   v    = voice microphone (MicVoiceMic, default 1 = the middle one)
//   r    = a - G * b   for the other two microphones a, b; G matches b's
//          level to a's (slow, from their long-term power), because the
//          capsules differ by several dB (hardware 2026-11-04). A talker in
//          front reaches a and b nearly alike, so r holds little voice;
//          sound from the speakers below / beside the keyboard reaches them
//          with different delays and stays. r holds nothing of v itself, so
//          the filter cannot learn to subtract the voice microphone's own
//          signal (the first, v-based design did on hardware).
//   out  = v(n - D) - sum_k W[k] r(n - k)
//
// The FIR filter (TAPS taps, the voice path delayed by D = TAPS/2 so it can
// model either arrival order) adapts by normalized LMS. While the talker
// dominates (r small against v) adaption pauses; a filter that makes the
// output louder than the voice for HOLD samples is reset. Each channel's DC
// is removed first. All state is per stream, O(TAPS) per sample, one 64-bit
// division a sample - fine at DISPATCH_LEVEL.
// ---------------------------------------------------------------------------
#define T2AUDIO_MIC_ANC_TAPS          64
#define T2AUDIO_MIC_ANC_DELAY         (T2AUDIO_MIC_ANC_TAPS / 2)
#define T2AUDIO_MIC_ANC_WEIGHT_MAX    (4ll << 24)          // W in Q24, |W| <= 4
#define T2AUDIO_MIC_ANC_MU_Q16        1311                 // step 0.02
#define T2AUDIO_MIC_ANC_GAIN_SHIFT    13                   // extra fraction bits of the step
#define T2AUDIO_MIC_ANC_EPS           (1ll << 14)          // norm floor (about -114 dBFS a tap)
#define T2AUDIO_MIC_ANC_POWER_SHIFT   10                   // power smoothing, ~21 ms
#define T2AUDIO_MIC_ANC_ADAPT_RATIO   64                   // adapt while ref > voice - 18 dB
#define T2AUDIO_MIC_ANC_FLOOR         (16ll << T2AUDIO_MIC_ANC_POWER_SHIFT) // power floor (~-114 dBFS raw)
#define T2AUDIO_MIC_ANC_DIVERGE_HOLD  4800                 // 100 ms louder than the voice -> reset
#define T2AUDIO_MIC_ANC_MATCH_SHIFT   14                   // level matching power smoothing, ~0.34 s
#define T2AUDIO_MIC_ANC_WARMUP        24000                // 0.5 s: match the levels before adapting
#define T2AUDIO_MIC_ANC_LEAK_PERIOD   64                   // weights decay by 1/2048 every 64 samples
#define T2AUDIO_MIC_ANC_LEAK_SHIFT    11                   // (time constant ~2.7 s): stale weights fade
#define T2AUDIO_MIC_ANC_MATCH_PERIOD  4096                 // samples between gain updates
#define T2AUDIO_MIC_ANC_MATCH_MIN     16384u               // G in Q16: 0.25 .. 4 (+-12 dB)
#define T2AUDIO_MIC_ANC_MATCH_MAX     262144u

typedef struct _T2AUDIO_MIC_ANC_STATE {
    int Ref[T2AUDIO_MIC_ANC_TAPS];         // noise reference, ring
    int Voice[T2AUDIO_MIC_ANC_DELAY];      // voice delay line, ring
    long long W[T2AUDIO_MIC_ANC_TAPS];     // filter weights, Q24
    long long Norm;                        // sum of Ref^2 over the ring
    // Smoothed powers as accumulators: Acc += x^2/16 - Acc/1024, so tiny
    // raw levels (the microphones sit near -80 dBFS before the +39 dB gain)
    // are not rounded away.
    long long PowVoice, PowRef, PowOut;
    // Level matching of the reference pair: slow powers of a and b, and G.
    long long PowA, PowB;
    unsigned int MatchGain;                // Q16
    unsigned int MatchTick;
    unsigned int Warmup;                   // samples left before adaption may start
    unsigned int LeakTick;
    int DcPrimed;                          // the DC blocker starts at the first sample
    unsigned int Pos;                      // ring position of the newest reference
    unsigned int VoicePos;
    unsigned int Diverging;                // samples the output was louder than the voice
    unsigned int Resets;
    // Run diagnostics (logged with the run summary): summed powers of the
    // voice, the reference and the output (x^2 >> 4), per-microphone powers,
    // samples, and how many of them adapted.
    unsigned long long SumVoice, SumRef, SumOut;
    unsigned long long SumMic[3];
    unsigned long long Samples, AdaptSamples;
    // DC blocker per microphone (Q8 running mean, ~4 Hz): the raw channels
    // carry DC offsets.
    long long Dc[3];
} T2AUDIO_MIC_ANC_STATE;

// ---------------------------------------------------------------------------
// Three-capsule matched beam (Parameters\MicDsp=3): the average of all three
// microphones after matching each capsule's sensitivity to mic 0. A talker
// in front adds up coherently and keeps the level of MicDsp=1, while each
// capsule's own self-noise is independent and drops (about 1.8 dB below the
// two-capsule average, more when the quiet capsule was dragging it down;
// hardware 2026-11-04: mic1 4.6 dB below mic0 / mic2). The gains follow the
// long-term (~1.4 s) power of each DC-blocked capsule, clamped to +-6 dB.
// ---------------------------------------------------------------------------
#define T2AUDIO_MIC_BEAM_POWER_SHIFT  16
#define T2AUDIO_MIC_BEAM_PERIOD       4096                 // samples between gain updates
#define T2AUDIO_MIC_BEAM_GAIN_MIN     32845u               // Q16, -6 dB
#define T2AUDIO_MIC_BEAM_GAIN_MAX     130762u              // Q16, +6 dB

typedef struct _T2AUDIO_MIC_BEAM_STATE {
    long long Dc[3];                       // DC blocker, Q8 running mean
    long long Pow[3];                      // long-term power accumulators
    unsigned int Gain[3];                  // Q16, Gain[0] is always 1
    unsigned int Tick;
    int Primed;
} T2AUDIO_MIC_BEAM_STATE;

typedef struct _T2AUDIO_MIC_STATE {
    long long X1, X2, Y1, Y2;
    long long Err;     // error feedback of the >> 28 (keeps the 20 Hz pole clean)
    T2AUDIO_MIC_ANC_STATE Anc;
    T2AUDIO_MIC_BEAM_STATE Beam;
    // Output gain in Q16 (0 = the default +39 dB) and the high-pass priming:
    // the filter starts from the first sample, so a DC offset is no step
    // (it used to give a click of up to half scale at every capture start).
    long long OutGainQ16;
    int HpPrimed;
} T2AUDIO_MIC_STATE;

static __inline void
T2AudioMicBeamReset(T2AUDIO_MIC_BEAM_STATE *B)
{
    unsigned int k;

    for (k = 0; k < 3; k++) {
        B->Dc[k] = 0;
        B->Pow[k] = 0;
        B->Gain[k] = 65536u;
    }
    B->Tick = 0;
    B->Primed = 0;
}

static __inline void
T2AudioMicAncReset(T2AUDIO_MIC_ANC_STATE *A)
{
    unsigned int k;

    for (k = 0; k < T2AUDIO_MIC_ANC_TAPS; k++) {
        A->Ref[k] = 0;
        A->W[k] = 0;
    }
    for (k = 0; k < T2AUDIO_MIC_ANC_DELAY; k++) {
        A->Voice[k] = 0;
    }
    A->Norm = 0;
    A->PowVoice = A->PowRef = A->PowOut = 0;
    A->PowA = A->PowB = 0;
    A->MatchGain = 65536u;
    A->MatchTick = 0;
    A->Warmup = T2AUDIO_MIC_ANC_WARMUP;
    A->LeakTick = 0;
    A->DcPrimed = 0;
    A->Pos = A->VoicePos = 0;
    A->Diverging = 0;
    A->Resets = 0;
    A->SumVoice = A->SumRef = A->SumOut = 0;
    A->Samples = A->AdaptSamples = 0;
    for (k = 0; k < 3; k++) {
        A->Dc[k] = 0;
        A->SumMic[k] = 0;
    }
}

static __inline void
T2AudioMicReset(T2AUDIO_MIC_STATE *S)
{
    S->X1 = S->X2 = S->Y1 = S->Y2 = S->Err = 0;
    T2AudioMicAncReset(&S->Anc);
    T2AudioMicBeamReset(&S->Beam);
    S->HpPrimed = 0;
    S->OutGainQ16 = 0;
}

static __inline int
T2AudioMicAncClamp(long long v, long long limit)
{
    if (v > limit - 1) {
        return (int)(limit - 1);
    }
    if (v < -limit) {
        return (int)-limit;
    }
    return (int)v;
}

// Integer square root of a non-negative 64-bit value (bit by bit).
static __inline unsigned long long
T2AudioMicIsqrt(unsigned long long v)
{
    unsigned long long r = 0, bit = 1ull << 62;

    while (bit > v) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

// G = sqrt(PowA / PowB) in Q16, clamped to +-12 dB; unchanged while either
// microphone is silent.
static __inline void
T2AudioMicAncUpdateMatch(T2AUDIO_MIC_ANC_STATE *A)
{
    unsigned long long pa = (unsigned long long)A->PowA;
    unsigned long long pb = (unsigned long long)A->PowB;
    unsigned long long ratio;
    unsigned long long g;

    if (pa < (16ull << T2AUDIO_MIC_ANC_MATCH_SHIFT) || pb < (16ull << T2AUDIO_MIC_ANC_MATCH_SHIFT)) {
        return;
    }
    while (pa > (1ull << 40) || pb > (1ull << 40)) {
        pa >>= 1;
        pb >>= 1;
    }
    ratio = (pa << 22) / pb;                    // PowA / PowB in Q22
    if (ratio < (1ull << 18)) {
        ratio = 1ull << 18;                     // 1/16
    } else if (ratio > (1ull << 26)) {
        ratio = 1ull << 26;                     // 16
    }
    g = T2AudioMicIsqrt(ratio << 10);           // sqrt(ratio) in Q16
    if (g < T2AUDIO_MIC_ANC_MATCH_MIN) {
        g = T2AUDIO_MIC_ANC_MATCH_MIN;
    } else if (g > T2AUDIO_MIC_ANC_MATCH_MAX) {
        g = T2AUDIO_MIC_ANC_MATCH_MAX;
    }
    A->MatchGain = (unsigned int)g;
}

// One frame of the canceller. Raw[] are the three 24-bit samples; Voice is
// the index of the voice microphone (0..2). Returns a 24-bit sample: the
// voice delayed by T2AUDIO_MIC_ANC_DELAY frames, minus the estimated noise.
static __inline int
T2AudioMicAncProcess(T2AUDIO_MIC_ANC_STATE *A, const int *Raw, unsigned int Voice)
{
    int Mic[3];
    int v, ma, mb, ref;
    int delayed;
    long long y = 0;
    long long e;
    unsigned int k, idx;

    if (Voice > 2) {
        Voice = 1;
    }
    // Remove each channel's DC first (the kaiT2en high-pass after the
    // canceller still shapes the low end as before).
    for (k = 0; k < 3; k++) {
        long long x = (long long)Raw[k];

        // Start at the first sample: no settling transient that would upset
        // the level match.
        if (!A->DcPrimed) {
            A->Dc[k] = x * 256;
        }
        A->Dc[k] += T2AudioMicSar(x * 256 - A->Dc[k], 11);
        Mic[k] = T2AudioMicAncClamp(x - T2AudioMicSar(A->Dc[k], 8), 1ll << 23);
        A->SumMic[k] += (unsigned long long)(((long long)Mic[k] * Mic[k]) >> 4);
    }
    A->DcPrimed = 1;
    v = Mic[Voice];
    ma = Mic[(Voice + 1) % 3];
    mb = Mic[(Voice + 2) % 3];

    // Level-match b to a, then the reference is their difference.
    A->PowA += (((long long)ma * ma) >> 4) - (A->PowA >> T2AUDIO_MIC_ANC_MATCH_SHIFT);
    A->PowB += (((long long)mb * mb) >> 4) - (A->PowB >> T2AUDIO_MIC_ANC_MATCH_SHIFT);
    if (++A->MatchTick >= T2AUDIO_MIC_ANC_MATCH_PERIOD) {
        A->MatchTick = 0;
        T2AudioMicAncUpdateMatch(A);
    }
    ref = T2AudioMicAncClamp((long long)ma - T2AudioMicSar((long long)mb * A->MatchGain, 16), 1ll << 24);

    // Push the reference (keeping the running norm) and the voice.
    A->Pos = (A->Pos + 1) % T2AUDIO_MIC_ANC_TAPS;
    {
        long long old = A->Ref[A->Pos];

        A->Norm += (long long)ref * ref - old * old;
        A->Ref[A->Pos] = ref;
    }
    if (A->Norm < 0) {
        A->Norm = 0;
    }
    delayed = A->Voice[A->VoicePos];
    A->Voice[A->VoicePos] = v;
    A->VoicePos = (A->VoicePos + 1) % T2AUDIO_MIC_ANC_DELAY;

    // Noise estimate: |W| <= 2^26, |Ref| <= 2^24, 64 terms: |y| < 2^57.
    idx = A->Pos;
    for (k = 0; k < T2AUDIO_MIC_ANC_TAPS; k++) {
        y += A->W[k] * A->Ref[idx];
        idx = (idx == 0) ? T2AUDIO_MIC_ANC_TAPS - 1 : idx - 1;
    }
    y = T2AudioMicSar(y, 24);
    e = (long long)delayed - y;

    // Smoothed powers of voice, reference and output.
    {
        long long pv = ((long long)delayed * delayed) >> 4;
        long long pr = ((long long)ref * ref) >> 4;
        long long ec = (e > (1ll << 26)) ? (1ll << 26) : ((e < -(1ll << 26)) ? -(1ll << 26) : e);
        long long po = (ec * ec) >> 4;

        A->PowVoice += pv - (A->PowVoice >> T2AUDIO_MIC_ANC_POWER_SHIFT);
        A->PowRef += pr - (A->PowRef >> T2AUDIO_MIC_ANC_POWER_SHIFT);
        A->PowOut += po - (A->PowOut >> T2AUDIO_MIC_ANC_POWER_SHIFT);
        A->SumVoice += (unsigned long long)pv;
        A->SumRef += (unsigned long long)pr;
        A->SumOut += (unsigned long long)po;
        A->Samples++;
    }

    // Slow leak: weights learnt under conditions that are gone (a level
    // match that has since moved, a talker who stopped) fade out instead of
    // staying applied for good.
    if (++A->LeakTick >= T2AUDIO_MIC_ANC_LEAK_PERIOD) {
        A->LeakTick = 0;
        for (k = 0; k < T2AUDIO_MIC_ANC_TAPS; k++) {
            A->W[k] -= T2AudioMicSar(A->W[k], T2AUDIO_MIC_ANC_LEAK_SHIFT);
        }
    }

    // Adapt only after the warm-up (level match settled) and while the
    // reference carries a noticeable share (no talker in front, or a
    // pause), never on silence.
    if (A->Warmup > 0) {
        A->Warmup--;
    } else if (A->PowVoice > T2AUDIO_MIC_ANC_FLOOR &&
        A->PowRef * T2AUDIO_MIC_ANC_ADAPT_RATIO > A->PowVoice) {
        long long ec = (e > (1ll << 25)) ? (1ll << 25) : ((e < -(1ll << 25)) ? -(1ll << 25) : e);
        // g = mu * e / (eps + norm), with GAIN_SHIFT extra bits: |e*mu| < 2^36,
        // << (8 + 13) < 2^57. Every reference in the ring is part of Norm,
        // so |g * Ref| < |e| * mu * 2^21 / sqrt(Norm + EPS) < 2^50.
        long long g = (ec * T2AUDIO_MIC_ANC_MU_Q16 * (1ll << (8 + T2AUDIO_MIC_ANC_GAIN_SHIFT))) /
                      (A->Norm + T2AUDIO_MIC_ANC_EPS);

        A->AdaptSamples++;
        idx = A->Pos;
        for (k = 0; k < T2AUDIO_MIC_ANC_TAPS; k++) {
            long long w = A->W[k] + T2AudioMicSar(g * A->Ref[idx], T2AUDIO_MIC_ANC_GAIN_SHIFT);

            if (w > T2AUDIO_MIC_ANC_WEIGHT_MAX) {
                w = T2AUDIO_MIC_ANC_WEIGHT_MAX;
            } else if (w < -T2AUDIO_MIC_ANC_WEIGHT_MAX) {
                w = -T2AUDIO_MIC_ANC_WEIGHT_MAX;
            }
            A->W[k] = w;
            idx = (idx == 0) ? T2AUDIO_MIC_ANC_TAPS - 1 : idx - 1;
        }
    }

    // Divergence guard: the canceller must never add noise. If the output
    // stays more than 3 dB above the plain voice, start over.
    if (A->PowVoice > T2AUDIO_MIC_ANC_FLOOR && A->PowOut > 2 * A->PowVoice) {
        if (++A->Diverging >= T2AUDIO_MIC_ANC_DIVERGE_HOLD) {
            for (k = 0; k < T2AUDIO_MIC_ANC_TAPS; k++) {
                A->W[k] = 0;
            }
            A->Diverging = 0;
            A->Resets++;
            return delayed;
        }
    } else {
        A->Diverging = 0;
    }
    return T2AudioMicAncClamp(e, 1ll << 23);
}

// One 24-bit sample from its 32-bit slot (low-aligned, sign in bit 23).
static __inline int
T2AudioMicSample24(unsigned int Raw)
{
    unsigned int v = Raw & 0x00FFFFFFu;

    return (v & 0x00800000u) ? (int)v - 0x01000000 : (int)v;
}

// One frame of the microphone DSP. Frame points at 3 raw slots. Dsp = 0
// passes mic 0 through unprocessed (diagnostics); 1 is kaiT2en's 16_1 chain
// (beam of mic 0 + mic 1); 2 replaces the beam with the three-microphone
// noise canceller around VoiceMic (T2AudioMicAncProcess), then the same
// high-pass and gain. Returns a 32-bit MSB-aligned PCM sample (what Windows
// expects for 32-bit PCM).
static __inline int
T2AudioMicProcessFrameEx(T2AUDIO_MIC_STATE *S, const unsigned int *Frame, unsigned int Dsp,
                         unsigned int VoiceMic)
{
    long long x, acc, y, out;

    if (!Dsp) {
        return (int)((unsigned int)T2AudioMicSample24(Frame[0]) << 8);
    }
    if (Dsp >= 3) {
        T2AUDIO_MIC_BEAM_STATE *b = &S->Beam;
        long long sum = 0;
        unsigned int k;

        for (k = 0; k < 3; k++) {
            long long v = (long long)T2AudioMicSample24(Frame[k]);
            long long m;

            if (!b->Primed) {
                b->Dc[k] = v * 256;
            }
            b->Dc[k] += T2AudioMicSar(v * 256 - b->Dc[k], 11);
            m = v - T2AudioMicSar(b->Dc[k], 8);
            b->Pow[k] += ((m * m) >> 4) - (b->Pow[k] >> T2AUDIO_MIC_BEAM_POWER_SHIFT);
            sum += T2AudioMicSar(m * (long long)b->Gain[k], 16);
        }
        b->Primed = 1;
        if (++b->Tick >= T2AUDIO_MIC_BEAM_PERIOD) {
            b->Tick = 0;
            for (k = 1; k < 3; k++) {
                unsigned long long p0 = (unsigned long long)b->Pow[0];
                unsigned long long pk = (unsigned long long)b->Pow[k];
                unsigned long long g;

                if (p0 < (16ull << T2AUDIO_MIC_BEAM_POWER_SHIFT) || pk < (16ull << T2AUDIO_MIC_BEAM_POWER_SHIFT)) {
                    continue;      // silent capsule: keep the gain
                }
                while (p0 > (1ull << 40) || pk > (1ull << 40)) {
                    p0 >>= 1;
                    pk >>= 1;
                }
                g = T2AudioMicIsqrt(((p0 << 22) / pk) << 10);   // sqrt(P0/Pk) in Q16
                if (g < T2AUDIO_MIC_BEAM_GAIN_MIN) {
                    g = T2AUDIO_MIC_BEAM_GAIN_MIN;
                } else if (g > T2AUDIO_MIC_BEAM_GAIN_MAX) {
                    g = T2AUDIO_MIC_BEAM_GAIN_MAX;
                }
                b->Gain[k] = (unsigned int)g;
            }
        }
        // Average of three (x 21845 / 65536), with the chain's extra precision.
        x = T2AudioMicSar(sum * 21845, 16) * (1ll << T2AUDIO_MIC_SHIFT);
    } else if (Dsp == 2) {
        int mic[3];

        mic[0] = T2AudioMicSample24(Frame[0]);
        mic[1] = T2AudioMicSample24(Frame[1]);
        mic[2] = T2AudioMicSample24(Frame[2]);
        x = (long long)T2AudioMicAncProcess(&S->Anc, mic, VoiceMic) * (1ll << T2AUDIO_MIC_SHIFT);
    } else {
        // Beam: average of mic 0 and mic 1 (24-bit range), then extra precision.
        x = ((long long)T2AudioMicSample24(Frame[0]) + (long long)T2AudioMicSample24(Frame[1]));
        x = x * (1ll << (T2AUDIO_MIC_SHIFT - 1));
    }
    if (!S->HpPrimed) {
        // Steady state for a constant input x: the high-pass outputs 0.
        S->X1 = S->X2 = x;
        S->Y1 = S->Y2 = 0;
        S->Err = 0;
        S->HpPrimed = 1;
    }
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
    out = T2AudioMicSar(y * ((S->OutGainQ16 > 0) ? S->OutGainQ16 : T2AUDIO_MIC_OUT_GAIN_Q16),
                        16 + T2AUDIO_MIC_SHIFT);
    if (out > 0x7FFFFF) {
        out = 0x7FFFFF;
    } else if (out < -0x800000) {
        out = -0x800000;
    }
    return (int)((unsigned int)(int)out << 8);
}

static __inline int
T2AudioMicProcessFrame(T2AUDIO_MIC_STATE *S, const unsigned int *Frame, unsigned int Dsp)
{
    return T2AudioMicProcessFrameEx(S, Frame, Dsp, 1);
}

// Store one MSB-aligned 32-bit capture sample into the client buffer in its
// container (Parameters\ExtraFormats: 16 or 24 bits, packed little-endian;
// otherwise 32). 16 and 24 bits round to nearest and saturate. Dest is the
// frame's first byte; returns the bytes written (0 for an unknown size).
static __inline unsigned int
T2AudioMicStoreSample(unsigned char *Dest, int Sample, unsigned int Bits)
{
    long long v;

    if (Bits == 32) {
        unsigned int u = (unsigned int)Sample;

        Dest[0] = (unsigned char)u;
        Dest[1] = (unsigned char)(u >> 8);
        Dest[2] = (unsigned char)(u >> 16);
        Dest[3] = (unsigned char)(u >> 24);
        return 4;
    }
    if (Bits != 24 && Bits != 16) {
        return 0;
    }
    v = T2AudioMicSar((long long)Sample + (1ll << (31 - Bits)), 32 - Bits);
    if (v > (1ll << (Bits - 1)) - 1) {
        v = (1ll << (Bits - 1)) - 1;
    }
    Dest[0] = (unsigned char)(v & 0xFF);
    Dest[1] = (unsigned char)((v >> 8) & 0xFF);
    if (Bits == 24) {
        Dest[2] = (unsigned char)((v >> 16) & 0xFF);
        return 3;
    }
    return 2;
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

// Auto-switch (Parameters\HeadphoneAutoMute): a built-in device that yields
// to a plugged-in jack device reports itself "unplugged" while that one is
// plugged, so Windows moves the default endpoint over and back by itself
// (it only changes the default when the current one goes away). Speakers
// yield to headphones; the built-in microphones yield to a headset. Without
// auto-switch, jack tracking, or a verified jack read it stays connected.
static __inline int
T2AudioJackYieldConnected(unsigned int AutoSwitch, unsigned int JackEvents, int Seen,
                          long OtherPlugged)
{
    if (!AutoSwitch || !JackEvents || !Seen) {
        return 1;
    }
    return OtherPlugged == 0;
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
