#ifndef T2AUDIO_DSP_H
#define T2AUDIO_DSP_H

// Speaker DSP for the stereo path (Parameters\SpeakerDsp=1), integer only so
// it can run in the copy DPC, and free of kernel APIs so the host tests
// (tests/T2AudioDspTest.c) run the same code.
//
// Signal flow per frame (the linear part of KAIT2EN's MacBookPro16,1 graph,
// coefficients in T2AudioDspProfile16_1.h):
//
//   L, R ──► Tone1 (12th-order high-pass, woofer excursion protection)
//        ──► XTC: mid = (L+R)/2 delayed, side = (L-R)/2 through the FIR,
//                 L = mid + side, R = mid - side   (crosstalk cancellation)
//        ──► Tone2 (midrange EQ)
//        ──► per side: woofer chain (crossover + EQ) -> ring 0/1 (L), 3/4 (R)
//                      tweeter chain (crossover + EQ) -> ring 2 (L), 5 (R)
//        ──► limiter (linked: left woofers, right woofers, both tweeters)
//
// Not reproduced (nonlinear in the graph): autogain, virtual bass,
// multiband compressor, woofer "Buzz" compressor; KAIT2EN's limiters are
// replaced by the simple peak limiter below.
//
// Number format: samples are 64-bit integers scaled so that full scale is
// 2^27 inside the chain: 24 dB of headroom, because the cascades reach
// +17.5 dB in their middle sections (Tone1 +14.3 dB, woofer chain +17.5 dB)
// before the later sections bring the level down; each stage saturates at
// +-2^31. Error feedback in every biquad keeps the noise below the 24-bit
// output floor despite the 4 bits given up. At the output the signal is
// scaled back to full scale 2^31, limited, and converted to the ring word
// like the plain stereo path.
// Coefficients are Q28 (|c| < 2): each product is below 2^60 and a 5-term
// biquad sum (plus the < 2^28 error feedback) below 2^63; the FIR sum is
// below 2^61 (sum |h| < 1.1).

#include "T2AudioSpeakerLogic.h"
#include "T2AudioDspProfile16_1.h"

#define T2DSP_FULL_SCALE      (1ll << 31)
#define T2DSP_INTERNAL_SHIFT  4     // chain runs 24 dB below full scale
// Limiter: threshold -1 dBFS (KAIT2EN ControlFreak "th" 0.891), instant
// attack, release with a time constant of 2^10 samples (~21 ms at 48 kHz).
#define T2DSP_LIMIT_THRESHOLD 1913946815ll   // 0.8912509 * 2^31
#define T2DSP_GAIN_ONE        (1ll << 30)    // limiter gain, Q30
#define T2DSP_RELEASE_SHIFT   10

typedef struct _T2DSP_BQ_STATE {
    long long X1, X2, Y1, Y2;
    long long E;    // truncation remainder fed back into the next sample
} T2DSP_BQ_STATE;

typedef struct _T2DSP_STATE {
    T2DSP_BQ_STATE Tone1[2][T2DSP_TONE1_STAGES];
    T2DSP_BQ_STATE Tone2[2][T2DSP_TONE2_STAGES];
    T2DSP_BQ_STATE Woofer[2][T2DSP_WOOFER_STAGES];
    T2DSP_BQ_STATE Tweeter[2][T2DSP_TWEETER_STAGES];
    // Circular histories, newest at index Pos.
    long long Mid[T2DSP_XTC_DELAY + 1];
    long long Side[T2DSP_XTC_TAPS];
    unsigned int MidPos;
    unsigned int SidePos;
    // Limiter gains (Q30): left woofers, right woofers, tweeters.
    long long Gain[3];
    // Diagnostics: frames where a limiter reduced the gain.
    unsigned long LimitedFrames;
} T2DSP_STATE;

static __inline void
T2DspReset(T2DSP_STATE *S)
{
    unsigned char *p = (unsigned char *)S;
    unsigned long i;

    if (S == 0) {
        return;
    }
    for (i = 0; i < sizeof(*S); i++) {
        p[i] = 0;
    }
    S->Gain[0] = T2DSP_GAIN_ONE;
    S->Gain[1] = T2DSP_GAIN_ONE;
    S->Gain[2] = T2DSP_GAIN_ONE;
}

static __inline long long
T2DspSat(long long v)
{
    if (v > T2DSP_FULL_SCALE - 1) {
        return T2DSP_FULL_SCALE - 1;
    }
    if (v < -T2DSP_FULL_SCALE) {
        return -T2DSP_FULL_SCALE;
    }
    return v;
}

// Arithmetic right shift of a signed 64-bit value, rounding toward minus
// infinity, without implementation-defined behavior.
static __inline long long
T2DspSar(long long v, unsigned int shift)
{
    if (v >= 0) {
        return v >> shift;
    }
    return -((-v + ((1ll << shift) - 1)) >> shift);
}

static __inline long long
T2DspBiquad(const int *C, T2DSP_BQ_STATE *S, long long x)
{
    long long acc = (long long)C[0] * x + (long long)C[1] * S->X1 + (long long)C[2] * S->X2 -
                    (long long)C[3] * S->Y1 - (long long)C[4] * S->Y2 + S->E;
    long long y = T2DspSar(acc, T2DSP_Q);

    // First-order error feedback: the bits dropped by the shift (0..2^Q-1)
    // are added to the next sample's sum. Without it the truncation noise of
    // the low-frequency sections (poles close to z = 1) is amplified to
    // about -100 dBFS; with it the noise stays near the 24-bit floor.
    S->E = acc - y * (1ll << T2DSP_Q);
    y = T2DspSat(y);

    S->X2 = S->X1;
    S->X1 = x;
    S->Y2 = S->Y1;
    S->Y1 = y;
    return y;
}

static __inline long long
T2DspCascade(const int (*C)[5], T2DSP_BQ_STATE *S, unsigned int Stages, long long x)
{
    unsigned int i;

    for (i = 0; i < Stages; i++) {
        x = T2DspBiquad(C[i], &S[i], x);
    }
    return x;
}

// XTC on one frame (in place).
static __inline void
T2DspXtc(T2DSP_STATE *S, long long *L, long long *R)
{
    long long mid = T2DspSar(*L + *R, 1);
    long long side = T2DspSar(*L - *R, 1);
    long long midDelayed, acc = 0;
    unsigned int i, idx;

    S->MidPos = (S->MidPos + 1u) % (T2DSP_XTC_DELAY + 1u);
    S->Mid[S->MidPos] = mid;
    midDelayed = S->Mid[(S->MidPos + 1u) % (T2DSP_XTC_DELAY + 1u)];

    S->SidePos = (S->SidePos + 1u) % T2DSP_XTC_TAPS;
    S->Side[S->SidePos] = side;
    idx = S->SidePos;
    for (i = 0; i < T2DSP_XTC_TAPS; i++) {
        acc += (long long)g_T2DspXtcFir[i] * S->Side[idx];
        idx = (idx == 0) ? (T2DSP_XTC_TAPS - 1u) : (idx - 1u);
    }
    side = T2DspSar(acc, T2DSP_Q);
    *L = T2DspSat(midDelayed + side);
    *R = T2DspSat(midDelayed - side);
}

// Peak limiter on Count linked samples (full scale 2^31, |x| < 2^35): scales
// them by the shared gain (Q30) kept in *Gain.
static __inline void
T2DspLimit(long long *Gain, long long *Samples, unsigned int Count, unsigned long *Limited)
{
    long long peak = 0;
    long long g = *Gain;
    unsigned int i;

    for (i = 0; i < Count; i++) {
        long long a = Samples[i] < 0 ? -Samples[i] : Samples[i];
        if (a > peak) {
            peak = a;
        }
    }
    // Release toward unity, then attack instantly if needed.
    g += (T2DSP_GAIN_ONE - g) >> T2DSP_RELEASE_SHIFT;
    if (peak > 0 && T2DspSar(peak * (g >> 8), 22) > T2DSP_LIMIT_THRESHOLD) {
        // g = threshold / peak in Q30 (peak < 2^33, threshold < 2^31).
        g = (T2DSP_LIMIT_THRESHOLD << 30) / peak;
        (*Limited)++;
    }
    *Gain = g;
    for (i = 0; i < Count; i++) {
        Samples[i] = T2DspSat(T2DspSar(Samples[i] * (g >> 8), 22));
    }
}

// Render Frames stereo frames (MSB-aligned 32-bit host samples, L then R)
// into the 6-channel ring through the DSP. AttenuationShift is the same
// safety cut as the plain path (applied to the input); Shift converts to the
// ring word (8 for S24_LE).
static __inline void
T2DspRenderStereoToSpeakers(volatile unsigned int *Dest,
                            const unsigned int *Source,
                            unsigned long Frames,
                            unsigned int Shift,
                            unsigned int AttenuationShift,
                            T2DSP_STATE *S)
{
    unsigned long f;

    AttenuationShift = T2AudioClampAttenuation(AttenuationShift);
    for (f = 0; f < Frames; f++) {
        long long in[2], w[2], t[2], lw[1], rw[1], tw[2];
        unsigned int side;

        for (side = 0; side < 2; side++) {
            unsigned int host = T2AudioSar32(Source[f * 2u + side], AttenuationShift);
            long long x;

            if (host & 0x80000000u) {
                x = (long long)host - 0x100000000ll;
            } else {
                x = (long long)host;
            }
            in[side] = T2DspSar(x, T2DSP_INTERNAL_SHIFT);
            in[side] = T2DspCascade(g_T2DspTone1, S->Tone1[side], T2DSP_TONE1_STAGES, in[side]);
        }
        T2DspXtc(S, &in[0], &in[1]);
        for (side = 0; side < 2; side++) {
            long long x = T2DspCascade(g_T2DspTone2, S->Tone2[side], T2DSP_TONE2_STAGES, in[side]);

            w[side] = T2DspCascade(g_T2DspWoofer, S->Woofer[side], T2DSP_WOOFER_STAGES, x);
            t[side] = T2DspCascade(g_T2DspTweeter, S->Tweeter[side], T2DSP_TWEETER_STAGES, x);
            // Back to full scale (2^31) for the limiter and the ring.
            w[side] *= (1ll << T2DSP_INTERNAL_SHIFT);
            t[side] *= (1ll << T2DSP_INTERNAL_SHIFT);
        }
        lw[0] = w[0];
        rw[0] = w[1];
        tw[0] = t[0];
        tw[1] = t[1];
        T2DspLimit(&S->Gain[0], lw, 1, &S->LimitedFrames);
        T2DspLimit(&S->Gain[1], rw, 1, &S->LimitedFrames);
        T2DspLimit(&S->Gain[2], tw, 2, &S->LimitedFrames);

        {
            unsigned int lwWord = T2AudioSar32((unsigned int)(lw[0] & 0xFFFFFFFFll), Shift);
            unsigned int rwWord = T2AudioSar32((unsigned int)(rw[0] & 0xFFFFFFFFll), Shift);

            Dest[f * 6u + T2AUDIO_SPK_LEFT_WOOFER_A] = lwWord;
            Dest[f * 6u + T2AUDIO_SPK_LEFT_WOOFER_B] = lwWord;
            Dest[f * 6u + T2AUDIO_SPK_LEFT_TWEETER] =
                T2AudioSar32((unsigned int)(tw[0] & 0xFFFFFFFFll), Shift);
            Dest[f * 6u + T2AUDIO_SPK_RIGHT_WOOFER_A] = rwWord;
            Dest[f * 6u + T2AUDIO_SPK_RIGHT_WOOFER_B] = rwWord;
            Dest[f * 6u + T2AUDIO_SPK_RIGHT_TWEETER] =
                T2AudioSar32((unsigned int)(tw[1] & 0xFFFFFFFFll), Shift);
        }
    }
}

#endif
