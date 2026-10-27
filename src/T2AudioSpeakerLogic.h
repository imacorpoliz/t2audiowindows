#ifndef T2AUDIO_SPEAKER_LOGIC_H
#define T2AUDIO_SPEAKER_LOGIC_H

// Pure, dependency-free helpers for the speaker path: stereo -> 6 speaker
// channel layout, the tweeter protection filter and the T2 clock correction.
// Integer-only (no floating point at DISPATCH_LEVEL) and free of kernel APIs,
// so tests/T2AudioSpeakerLogicTest.c exercises the same code on the host.
// `int` / `unsigned int` are 32 bits and `long long` 64 bits on every target.

// Speaker channels of the MacBookPro16,1 ring (S24_LE, 6 x 32-bit per frame),
// taken from kaiT2en's DSP profile dsp/profiles/16_1/graph.json, whose outputs
// AUX0..AUX5 are ordered like the ring: 0/1 and 3/4 are the left/right woofer
// pairs (low-passed there: -21 dB at 500 Hz), 2/5 the left/right tweeters
// (high-passed there: -90 dB at 500 Hz, -46 dB at 1 kHz). See
// docs/analysis/RE_speaker_layout_and_clock.md.
#define T2AUDIO_RING_CHANNELS          6u
#define T2AUDIO_RING_BYTES_PER_FRAME   (T2AUDIO_RING_CHANNELS * 4u)
#define T2AUDIO_SPK_LEFT_WOOFER_A      0u
#define T2AUDIO_SPK_LEFT_WOOFER_B      1u
#define T2AUDIO_SPK_LEFT_TWEETER       2u
#define T2AUDIO_SPK_RIGHT_WOOFER_A     3u
#define T2AUDIO_SPK_RIGHT_WOOFER_B     4u
#define T2AUDIO_SPK_RIGHT_TWEETER      5u

// Tweeter handling for the stereo path.
#define T2AUDIO_TWEETER_MUTE           0u  // tweeters get silence
#define T2AUDIO_TWEETER_HIGHPASS       1u  // 4th-order high-pass (default)

// Q30 fixed-point biquad (direct form I). The a0 term is normalized to 1.
typedef struct _T2AUDIO_BIQUAD_Q30 {
    int B0, B1, B2, A1, A2;
} T2AUDIO_BIQUAD_Q30;

typedef struct _T2AUDIO_BIQUAD_STATE {
    long long X1, X2, Y1, Y2;
} T2AUDIO_BIQUAD_STATE;

// Tweeter protection: 4th-order Butterworth high-pass at 3 kHz for 48 kHz, as
// two cascaded RBJ biquads (Q = 0.5412 and 1.3066), Q30. -24 dB/octave:
// about -38 dB at 1 kHz and -62 dB at 500 Hz. Apple's own tweeter chain is
// steeper (see the analysis doc); the first audible tests stay at low volume.
#define T2AUDIO_TWEETER_STAGES 2u
static const T2AUDIO_BIQUAD_Q30 g_T2AudioTweeterHighPass48k[T2AUDIO_TWEETER_STAGES] = {
    { 763084018, -1526168035, 763084018, -1465783472, 512810774 },
    { 900935945, -1801871890, 900935945, -1730578792, 799423163 },
};

// Filter state of the stereo path: one cascade per tweeter (left, right).
typedef struct _T2AUDIO_SPEAKER_STATE {
    T2AUDIO_BIQUAD_STATE Tweeter[2][T2AUDIO_TWEETER_STAGES];
} T2AUDIO_SPEAKER_STATE;

static __inline void
T2AudioSpeakerStateReset(T2AUDIO_SPEAKER_STATE *State)
{
    unsigned int side, stage;

    if (State == 0) {
        return;
    }
    for (side = 0; side < 2; side++) {
        for (stage = 0; stage < T2AUDIO_TWEETER_STAGES; stage++) {
            State->Tweeter[side][stage].X1 = 0;
            State->Tweeter[side][stage].X2 = 0;
            State->Tweeter[side][stage].Y1 = 0;
            State->Tweeter[side][stage].Y2 = 0;
        }
    }
}

// Portable arithmetic right shift of a 32-bit two's-complement value held in
// an unsigned int (shifting a negative int is implementation-defined in C).
static __inline unsigned int
T2AudioSar32(unsigned int Value, unsigned int Shift)
{
    if (Shift == 0) {
        return Value;
    }
    if (Shift > 31) {
        Shift = 31;
    }
    return (Value & 0x80000000u) ? ~((~Value) >> Shift) : (Value >> Shift);
}

// Sign-extend the low 24 bits of a 32-bit value.
static __inline long long
T2AudioS24ToLL(unsigned int Value)
{
    Value &= 0x00FFFFFFu;
    return (Value & 0x00800000u) ? (long long)Value - 0x01000000ll : (long long)Value;
}

static __inline long long
T2AudioClampS24(long long Value)
{
    if (Value > 0x7FFFFFll) {
        return 0x7FFFFFll;
    }
    if (Value < -0x800000ll) {
        return -0x800000ll;
    }
    return Value;
}

// Arithmetic right shift of a signed 64-bit value by 30, rounding toward
// minus infinity, without relying on implementation-defined behavior.
static __inline long long
T2AudioSar64By30(long long Value)
{
    if (Value >= 0) {
        return Value >> 30;
    }
    return -((-Value + ((1ll << 30) - 1)) >> 30);
}

// One Q30 biquad step on a 24-bit sample. |x|, |y| <= 2^23 and |coeff| < 2^31,
// so each product is below 2^54 and the five-term sum fits in 63 bits.
static __inline long long
T2AudioBiquadStep(const T2AUDIO_BIQUAD_Q30 *C, T2AUDIO_BIQUAD_STATE *S, long long X)
{
    long long acc = (long long)C->B0 * X + (long long)C->B1 * S->X1 +
                    (long long)C->B2 * S->X2 - (long long)C->A1 * S->Y1 -
                    (long long)C->A2 * S->Y2;
    long long y = T2AudioClampS24(T2AudioSar64By30(acc));

    S->X2 = S->X1;
    S->X1 = X;
    S->Y2 = S->Y1;
    S->Y1 = y;
    return y;
}

// Convert a 24-bit sample to the ring's 32-bit word for a ring that expects
// host samples shifted right by Shift (8 for S24_LE, 0 for a 32-bit ring).
static __inline unsigned int
T2AudioS24ToRing(long long Sample24, unsigned int Shift)
{
    unsigned int v = (unsigned int)(Sample24 & 0xFFFFFFFFll);

    if (Shift >= 8) {
        return T2AudioSar32(v, Shift - 8);
    }
    return v << (8 - Shift);
}

// Render Frames stereo frames (two MSB-aligned 32-bit samples, L then R) into
// the 6-channel speaker ring: each side's sample goes to its woofer pair as
// is and to its tweeter through the high-pass (or silence). Dest may be device
// memory: every store is one aligned 32-bit write.
static __inline void
T2AudioRenderStereoToSpeakers(volatile unsigned int *Dest,
                              const unsigned int *Source,
                              unsigned long Frames,
                              unsigned int Shift,
                              unsigned int TweeterMode,
                              T2AUDIO_SPEAKER_STATE *State)
{
    unsigned long i;
    unsigned int side;

    for (i = 0; i < Frames; i++) {
        unsigned int out[2];
        unsigned int tweeter[2];

        for (side = 0; side < 2; side++) {
            unsigned int host = Source[i * 2u + side];
            long long s24 = T2AudioS24ToLL(T2AudioSar32(host, 8));

            out[side] = T2AudioSar32(host, Shift);
            tweeter[side] = 0;
            if (TweeterMode == T2AUDIO_TWEETER_HIGHPASS && State != 0) {
                unsigned int stage;
                long long y = s24;

                for (stage = 0; stage < T2AUDIO_TWEETER_STAGES; stage++) {
                    y = T2AudioBiquadStep(&g_T2AudioTweeterHighPass48k[stage],
                                          &State->Tweeter[side][stage], y);
                }
                tweeter[side] = T2AudioS24ToRing(y, Shift);
            }
        }

        Dest[i * 6u + T2AUDIO_SPK_LEFT_WOOFER_A] = out[0];
        Dest[i * 6u + T2AUDIO_SPK_LEFT_WOOFER_B] = out[0];
        Dest[i * 6u + T2AUDIO_SPK_LEFT_TWEETER] = tweeter[0];
        Dest[i * 6u + T2AUDIO_SPK_RIGHT_WOOFER_A] = out[1];
        Dest[i * 6u + T2AUDIO_SPK_RIGHT_WOOFER_B] = out[1];
        Dest[i * 6u + T2AUDIO_SPK_RIGHT_TWEETER] = tweeter[1];
    }
}

// ---------------------------------------------------------------------------
// T2 clock (docs/analysis/RE_speaker_layout_and_clock.md).
//
// bridgeOS sends UPDATE_TIMESTAMP with hostTime (ns on the host clock that
// AppleUSBVHCI publishes to the T2: QPC -> 100 ns * 100) and sampleTime (the
// T2 read position in frames; sampleTime mod ring = read slot). AppleAudio
// keeps its write position T2AUDIO_CLOCK_TARGET_LEAD frames ahead of the read
// position and re-aims when the error leaves +-T2AUDIO_CLOCK_TOLERANCE.
// ---------------------------------------------------------------------------
#define T2AUDIO_CLOCK_TARGET_LEAD 1000u
#define T2AUDIO_CLOCK_TOLERANCE   500u
// A hostTime further than this from our own clock at reception is not in our
// time base (e.g. AppleUSBVHCI's 64-bit QPC*1e7 product wraps after ~25.6 h of
// uptime); the reception time is used instead.
#define T2AUDIO_CLOCK_MAX_DELIVERY_NS 50000000ull

// Host nanoseconds from a QPC value, without overflow.
static __inline unsigned long long
T2AudioQpcToNs(unsigned long long Qpc, unsigned long long Frequency)
{
    if (Frequency == 0) {
        return 0;
    }
    return (Qpc / Frequency) * 1000000000ull +
           ((Qpc % Frequency) * 1000000000ull) / Frequency;
}

// 1 if hostTime is plausible on our clock at reception: not in the future and
// at most T2AUDIO_CLOCK_MAX_DELIVERY_NS old (kaiT2en measures 1-3 ms; after a
// wake bridgeOS can be far off until it recalibrates).
static __inline int
T2AudioClockHostTimePlausible(unsigned long long HostTimeNs,
                              unsigned long long ReceptionNs)
{
    return HostTimeNs <= ReceptionNs &&
           ReceptionNs - HostTimeNs <= T2AUDIO_CLOCK_MAX_DELIVERY_NS;
}

// Time (our ns) an anchor stands for: hostTime when plausible, else reception.
// A stream ignores anchors from before it sent START_IO: after a pause the
// previous run's next timestamp can still arrive (kaiT2en 6128f86).
static __inline unsigned long long
T2AudioClockAnchorNs(unsigned long long HostTimeNs, unsigned long long ReceptionNs)
{
    return T2AudioClockHostTimePlausible(HostTimeNs, ReceptionNs) ? HostTimeNs : ReceptionNs;
}

// T2 read position (frames) at the moment the message was received, given its
// sampleTime and hostTime and our clock at reception. Uses the hostTime delta
// when it is plausible (0..T2AUDIO_CLOCK_MAX_DELIVERY_NS), else assumes the
// message is current.
static __inline unsigned long long
T2AudioClockFramesAtReception(unsigned long long SampleFrames,
                              unsigned long long HostTimeNs,
                              unsigned long long ReceptionNs,
                              unsigned long SampleRate)
{
    if (!T2AudioClockHostTimePlausible(HostTimeNs, ReceptionNs)) {
        return SampleFrames;
    }
    return SampleFrames + ((ReceptionNs - HostTimeNs) * SampleRate) / 1000000000ull;
}

// Signed distance (WriteFrame - ReadFrame) on a ring of RingFrames, folded
// into (-RingFrames/2, RingFrames/2].
static __inline long long
T2AudioRingDistance(unsigned long long WriteFrame,
                    unsigned long long ReadFrame,
                    unsigned long RingFrames)
{
    long long d;

    if (RingFrames == 0) {
        return 0;
    }
    d = (long long)((WriteFrame % RingFrames + RingFrames - ReadFrame % RingFrames) % RingFrames);
    if (d > (long long)(RingFrames / 2u)) {
        d -= (long long)RingFrames;
    }
    return d;
}

// Given the next frame we will write (source frame + DestOffset) and the T2's
// current read frame, return 1 and the new DestOffset (0..RingFrames-1) when
// the lead left [TARGET - TOLERANCE, TARGET + TOLERANCE]; *OutJump is the
// signed change of the write position (positive: slots skipped this lap).
// Returns 0 when no correction is needed.
static __inline int
T2AudioClockCorrection(unsigned long long NextSourceFrame,
                       unsigned long long DestOffset,
                       unsigned long long ReadFrame,
                       unsigned long RingFrames,
                       unsigned long long *OutDestOffset,
                       long long *OutJump)
{
    long long lead;
    long long jump;
    long long offset;

    if (RingFrames == 0 || OutDestOffset == 0 || OutJump == 0) {
        return 0;
    }
    lead = T2AudioRingDistance(NextSourceFrame + DestOffset, ReadFrame, RingFrames);
    if (lead >= (long long)(T2AUDIO_CLOCK_TARGET_LEAD - T2AUDIO_CLOCK_TOLERANCE) &&
        lead <= (long long)(T2AUDIO_CLOCK_TARGET_LEAD + T2AUDIO_CLOCK_TOLERANCE)) {
        return 0;
    }
    jump = (long long)T2AUDIO_CLOCK_TARGET_LEAD - lead;
    offset = (long long)(DestOffset % RingFrames) + jump;
    offset %= (long long)RingFrames;
    if (offset < 0) {
        offset += (long long)RingFrames;
    }
    *OutDestOffset = (unsigned long long)offset;
    *OutJump = jump;
    return 1;
}

#endif
