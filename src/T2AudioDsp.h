#ifndef T2AUDIO_DSP_H
#define T2AUDIO_DSP_H

// Speaker DSP for the stereo path (Parameters\SpeakerDsp=1), integer only so
// it can run in the copy DPC, and free of kernel APIs so the host tests
// (tests/T2AudioDspTest.c) run the same code.
//
// Signal flow per frame (KAIT2EN's MacBookPro16,1 graph, coefficients in
// T2AudioDspProfile16_1.h). Level 1 = linear chain; level 2 adds the virtual
// bass in front of it:
//
//   L, R ──► [level 2] virtual bass (bankstown): harmonics of 40-150 Hz,
//                 band-limited to 200-450 Hz, added to the dry signal
//        ──► Tone1 (12th-order high-pass, woofer excursion protection)
//        ──► XTC: mid = (L+R)/2 delayed, side = (L-R)/2 through the FIR,
//                 L = mid + side, R = mid - side   (crosstalk cancellation)
//        ──► Tone2 (midrange EQ)
//        ──► [level 3] multiband compressor (4 bands, LR4 crossover)
//        ──► per side: woofer chain (crossover + EQ) [level 4: + buzz
//                      compressor 110-145 Hz] -> ring 0/1 (L), 3/4 (R)
//                      tweeter chain (crossover + EQ) -> ring 2 (L), 5 (R)
//        ──► lookahead limiter (linked: left woofers, right woofers, both
//            tweeters; 0.5 ms lookahead like KAIT2EN's ControlFreak)
//
// Optional (Parameters\DspAutoGain=1, any level): a slow autogain in front of
// the chain (approximation of the graph's "Norm" node, see T2DspAutoGain).
// KAIT2EN's lsp limiters are approximated by the lookahead limiter below. The woofer "Buzz" compressor
// (level 4) is one LR4-split band with the graph's setting. The multiband compressor is an approximation of lsp's (hard knee,
// peak envelope linked over L/R, gain updated every 16 frames and ramped).
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
// Limiter: threshold -1 dBFS (KAIT2EN ControlFreak "th" 0.891), lookahead
// 24 frames (0.5 ms, ControlFreak "lk"): the gain ramps down linearly before
// a peak instead of jumping on it, then releases with a time constant of
// 2^10 samples (~21 ms at 48 kHz). The output is delayed by the lookahead.
#define T2DSP_LIMIT_THRESHOLD 1913946815ll   // 0.8912509 * 2^31
#define T2DSP_GAIN_ONE        (1ll << 30)    // limiter gain, Q30
#define T2DSP_RELEASE_SHIFT   10
#define T2DSP_LOOKAHEAD       24u
// 2^30 / k for k = 1..T2DSP_LOOKAHEAD + 1 (ramp slopes without a division).
static const long long g_T2DspRecipQ30[T2DSP_LOOKAHEAD + 2] = { 0,
    1073741824, 536870912, 357913941, 268435456, 214748364, 178956970, 153391689, 134217728,
    119304647, 107374182, 97612893, 89478485, 82595524, 76695844, 71582788, 67108864,
    63161283, 59652323, 56512727, 53687091, 51130563, 48806446, 46684427, 44739242, 42949672 };

// Autogain (Parameters\DspAutoGain): RMS of the input (linked L/R, one-pole
// mean square with a 2^14-frame time constant, ~341 ms) is brought toward
// -12 dBFS (KAIT2EN Norm: -12 LUFS, unweighted here), by at most +6 dB and
// -12 dB; below -60 dBFS the gain holds (silence and fades are not pumped).
// The gain moves in the log2 domain every 16 frames: down with a time
// constant of 16 updates (~5 ms on top of the envelope), up with 128
// (~43 ms), and is ramped per frame in between. Log2 values are Q16.
#define T2DSP_AGC_TARGET      (-130624ll)   // -12 dB
#define T2DSP_AGC_MAX_BOOST   65312ll       // +6 dB
#define T2DSP_AGC_MAX_CUT     (-130624ll)   // -12 dB
#define T2DSP_AGC_GATE        (-653117ll)   // -60 dB
#define T2DSP_AGC_ENV_SHIFT   14u
#define T2DSP_AGC_DOWN_SHIFT  4u
#define T2DSP_AGC_UP_SHIFT    7u

// Make-up gain after the DSP (Parameters\DspGainDb, 0..12 dB, default 6),
// applied in front of the limiter. The KAIT2EN chain is ~5-9 dB quieter
// than the plain path on music-like signals (it relies on an autogain stage
// that is optional here: DspAutoGain), so without it every DSP level loses an A/B on
// loudness alone; the limiter still caps the output at -1 dBFS.
#define T2DSP_GAIN_DB_DEFAULT  6u
#define T2DSP_GAIN_DB_MAX      12u
static const int g_T2DspGainDbQ28[T2DSP_GAIN_DB_MAX + 1] = { 268435456, 301189535, 337940217, 379175160, 425441527, 477353244, 535599149, 600952130, 674279380, 756553907, 848867446, 952444939, 1068660799 };

static __inline long long
T2DspGainFromDb(unsigned int Db)
{
    return g_T2DspGainDbQ28[Db > T2DSP_GAIN_DB_MAX ? T2DSP_GAIN_DB_MAX : Db];
}

// SpeakerDsp levels.
#define T2DSP_LEVEL_OFF        0u
#define T2DSP_LEVEL_LINEAR     1u
#define T2DSP_LEVEL_VIRTBASS   2u
#define T2DSP_LEVEL_COMP       3u
#define T2DSP_LEVEL_BUZZ       4u
#define T2DSP_LEVEL_MAX        4u
#define T2DSP_COMP_BLOCK       16u    // frames per compressor gain update

typedef struct _T2DSP_BQ_STATE {
    long long X1, X2, Y1, Y2;
    long long E;    // truncation remainder fed back into the next sample
} T2DSP_BQ_STATE;

// Lookahead limiter for up to 2 linked samples: a ring of the last
// T2DSP_LOOKAHEAD + 1 frames with the gain each one needs (Q30).
typedef struct _T2DSP_LIMITER {
    long long Delay[T2DSP_LOOKAHEAD + 1][2];
    long long Need[T2DSP_LOOKAHEAD + 1];
    unsigned int Pos;        // slot of the newest frame
    unsigned int Pending;    // frames in the ring that need gain < 1
    long long Gain;          // gain applied to the frame leaving now, Q30
} T2DSP_LIMITER;

typedef struct _T2DSP_STATE {
    T2DSP_BQ_STATE VirtBass[2][4];
    T2DSP_BQ_STATE Tone1[2][T2DSP_TONE1_STAGES];
    T2DSP_BQ_STATE Tone2[2][T2DSP_TONE2_STAGES];
    // Compressor crossover per side: LPf2 x2, HPf2 x2, APf3, APf1, LPf1 x2,
    // HPf1 x2, LPf3 x2, HPf3 x2.
    T2DSP_BQ_STATE CompXo[2][14];
    long long CompEnv[4];       // peak envelope per band (internal scale)
    long long CompGain[4];      // current band gain incl. makeup, Q30
    long long CompStep[4];      // per-frame gain ramp, Q30
    unsigned int CompCount;     // frames until the next gain update
    // Woofer buzz compressor per side: LPhi x2, HPhi x2, APlo, LPlo x2, HPlo x2.
    T2DSP_BQ_STATE BuzzXo[2][9];
    long long BuzzEnv[2];
    long long BuzzGain[2];      // Q30, includes makeup
    long long BuzzStep[2];
    unsigned int BuzzCount;
    T2DSP_BQ_STATE Woofer[2][T2DSP_WOOFER_STAGES];
    T2DSP_BQ_STATE Tweeter[2][T2DSP_TWEETER_STAGES];
    // Circular histories, newest at index Pos.
    long long Mid[T2DSP_XTC_DELAY + 1];
    long long Side[T2DSP_XTC_TAPS];
    unsigned int MidPos;
    unsigned int SidePos;
    // Lookahead limiters: left woofers, right woofers, tweeters.
    T2DSP_LIMITER Limiter[3];
    // Autogain: mean square of the input, current log2 gain (Q16), the
    // linear gain (Q30) with its per-frame ramp, frames to the next update.
    long long AgcEnv;
    long long AgcLog;
    long long AgcGain;
    long long AgcStep;
    unsigned int AgcCount;
    // Diagnostics: frames where a limiter reduced the gain (all limiters),
    // and those of the woofer limiters alone.
    unsigned long LimitedFrames;
    unsigned long WooferLimitedFrames;
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
    for (i = 0; i < 3; i++) {
        unsigned int k;

        S->Limiter[i].Gain = T2DSP_GAIN_ONE;
        for (k = 0; k <= T2DSP_LOOKAHEAD; k++) {
            S->Limiter[i].Need[k] = T2DSP_GAIN_ONE;
        }
    }
    S->AgcGain = T2DSP_GAIN_ONE;
    for (i = 0; i < 4; i++) {
        S->CompGain[i] = T2DSP_GAIN_ONE;
    }
    S->BuzzGain[0] = T2DSP_GAIN_ONE;
    S->BuzzGain[1] = T2DSP_GAIN_ONE;
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

// tanh of an internal-scale value (2^27 = 1.0), result in the same scale;
// table lookup with linear interpolation (error < 2e-6).
static __inline long long
T2DspTanh(long long u)
{
    long long a = u < 0 ? -u : u;
    long long idx = a >> (27 - 8);              // 256 steps per unit
    long long frac = a & ((1ll << (27 - 8)) - 1);
    long long v;

    if (idx >= T2DSP_TANH_ENTRIES - 1) {
        v = 1ll << 30;
    } else {
        long long t0 = g_T2DspTanh[idx];
        long long t1 = g_T2DspTanh[idx + 1];
        v = t0 + T2DspSar((t1 - t0) * frac, 27 - 8);
    }
    v = T2DspSar(v, 3);                         // Q30 -> 2^27 scale
    return u < 0 ? -v : v;
}

// Virtual bass (bankstown): x + LP3c(HPf(sat(LPc(HPfloor(x))))).
static __inline long long
T2DspVirtBass(T2DSP_BQ_STATE *S, long long x)
{
    long long p = T2DspBiquad(g_T2DspVirtBass[0], &S[0], x);
    long long wet;

    p = T2DspBiquad(g_T2DspVirtBass[1], &S[1], p);
    wet = T2DspSar(T2DspTanh(T2DspSar(p * T2DSP_VB_THIRD_SCALE, T2DSP_Q)) * T2DSP_VB_THIRD_GAIN,
                   T2DSP_Q);
    if (p <= 0) {
        wet += T2DspSar(T2DspTanh(T2DspSar(p * T2DSP_VB_SECOND_SCALE, T2DSP_Q)) *
                            T2DSP_VB_SECOND_GAIN,
                        T2DSP_Q);
    }
    wet = T2DspBiquad(g_T2DspVirtBass[2], &S[2], wet);
    wet = T2DspBiquad(g_T2DspVirtBass[3], &S[3], wet);
    return T2DspSat(x + wet);
}

// log2(v) in Q16 for 0 < v < 2^40 (table + linear interpolation).
static __inline long long
T2DspLog2Q16(long long v)
{
    long long n, frac, t0, t1;
    unsigned int msb = 0, idx;

    if (v <= 0) {
        return -(64ll << 16);
    }
    while (msb < 62 && (v >> (msb + 1)) != 0) {
        msb++;
    }
    n = (msb <= 40) ? (v << (40 - msb)) : (v >> (msb - 40));   // [2^40, 2^41)
    idx = (unsigned int)((n >> 32) & 255);
    frac = (n >> 16) & 0xFFFF;
    t0 = g_T2DspLog2[idx];
    t1 = g_T2DspLog2[idx + 1];
    return ((long long)msb << 16) + t0 + T2DspSar((t1 - t0) * frac, 16);
}

// 2^(e / 65536) in Q30 for e <= 0 (Q16 exponent).
static __inline long long
T2DspExp2Q30(long long e)
{
    long long ip = T2DspSar(e, 16);
    long long frac = e - ip * 65536;          // 0..65535
    unsigned int idx = (unsigned int)(frac >> 8);
    long long t0 = g_T2DspExp2[idx];
    long long t1 = g_T2DspExp2[idx + 1];
    long long v = t0 + T2DspSar((t1 - t0) * (frac & 255), 8);

    if (ip >= 0) {
        return v;                               // e > 0 is not used
    }
    if (ip < -40) {
        return 0;
    }
    return v >> (unsigned int)(-ip);
}

// Gain (Q30, with makeup) of a downward compressor for an envelope, from a
// band row {log2 threshold Q16, 1 - 1/ratio Q16, makeup Q28, ...}.
static __inline long long
T2DspCompTarget(const long long *Band, long long Env)
{
    long long over = T2DspLog2Q16(Env) - (27ll << 16) - Band[0];
    long long target = T2DSP_GAIN_ONE;

    if (over > 0) {
        target = T2DspExp2Q30(-T2DspSar(over * Band[1], 16));
    }
    return T2DspSar(target * Band[2], T2DSP_Q);
}

// Multiband compressor on one stereo frame (in place), level 3.
static __inline void
T2DspComp(T2DSP_STATE *S, long long *L, long long *R)
{
    long long band[2][4];
    long long *io[2];
    unsigned int side, k;

    io[0] = L;
    io[1] = R;
    for (side = 0; side < 2; side++) {
        T2DSP_BQ_STATE *x = S->CompXo[side];
        long long in = T2DspSat(T2DspSar(*io[side] * T2DSP_COMP_INPUT_GAIN, T2DSP_Q));
        long long lo = T2DspBiquad(g_T2DspCompXover[0], &x[1], T2DspBiquad(g_T2DspCompXover[0], &x[0], in));
        long long hi = T2DspBiquad(g_T2DspCompXover[1], &x[3], T2DspBiquad(g_T2DspCompXover[1], &x[2], in));

        lo = T2DspBiquad(g_T2DspCompXover[2], &x[4], lo);
        hi = T2DspBiquad(g_T2DspCompXover[3], &x[5], hi);
        band[side][0] = T2DspBiquad(g_T2DspCompXover[4], &x[7], T2DspBiquad(g_T2DspCompXover[4], &x[6], lo));
        band[side][1] = T2DspBiquad(g_T2DspCompXover[5], &x[9], T2DspBiquad(g_T2DspCompXover[5], &x[8], lo));
        band[side][2] = T2DspBiquad(g_T2DspCompXover[6], &x[11], T2DspBiquad(g_T2DspCompXover[6], &x[10], hi));
        band[side][3] = T2DspBiquad(g_T2DspCompXover[7], &x[13], T2DspBiquad(g_T2DspCompXover[7], &x[12], hi));
    }

    for (k = 0; k < 4; k++) {
        long long a = band[0][k] < 0 ? -band[0][k] : band[0][k];
        long long b = band[1][k] < 0 ? -band[1][k] : band[1][k];
        long long peak = a > b ? a : b;
        long long coef = (peak > S->CompEnv[k]) ? g_T2DspCompBand[k][3] : g_T2DspCompBand[k][4];

        S->CompEnv[k] += T2DspSar((peak - S->CompEnv[k]) * coef, 30);
    }

    if (S->CompCount == 0) {
        S->CompCount = T2DSP_COMP_BLOCK;
        for (k = 0; k < 4; k++) {
            S->CompStep[k] = (T2DspCompTarget(g_T2DspCompBand[k], S->CompEnv[k]) - S->CompGain[k]) /
                             (long long)T2DSP_COMP_BLOCK;
        }
    }
    S->CompCount--;

    for (side = 0; side < 2; side++) {
        long long y = 0;

        for (k = 0; k < 4; k++) {
            y += T2DspSar(band[side][k] * S->CompGain[k], 30);
        }
        *io[side] = T2DspSat(y);
    }
    for (k = 0; k < 4; k++) {
        S->CompGain[k] += S->CompStep[k];
    }
}

// Woofer buzz compressor on both sides' woofer signals (in place), level 4:
// the 110-145 Hz band is split out (LR4, all-pass compensated) and
// compressed; the rest passes unchanged.
static __inline void
T2DspBuzz(T2DSP_STATE *S, long long *W)
{
    long long band[2], rest[2];
    unsigned int side;

    for (side = 0; side < 2; side++) {
        T2DSP_BQ_STATE *x = S->BuzzXo[side];
        long long lo = T2DspBiquad(g_T2DspBuzzXover[0], &x[1], T2DspBiquad(g_T2DspBuzzXover[0], &x[0], W[side]));
        long long hi = T2DspBiquad(g_T2DspBuzzXover[1], &x[3], T2DspBiquad(g_T2DspBuzzXover[1], &x[2], W[side]));
        long long a;
        long long coef;

        hi = T2DspBiquad(g_T2DspBuzzXover[2], &x[4], hi);
        band[side] = T2DspBiquad(g_T2DspBuzzXover[4], &x[8], T2DspBiquad(g_T2DspBuzzXover[4], &x[7], lo));
        rest[side] = hi + T2DspBiquad(g_T2DspBuzzXover[3], &x[6], T2DspBiquad(g_T2DspBuzzXover[3], &x[5], lo));

        a = band[side] < 0 ? -band[side] : band[side];
        coef = (a > S->BuzzEnv[side]) ? g_T2DspBuzzBand[3] : g_T2DspBuzzBand[4];
        S->BuzzEnv[side] += T2DspSar((a - S->BuzzEnv[side]) * coef, 30);
    }
    if (S->BuzzCount == 0) {
        S->BuzzCount = T2DSP_COMP_BLOCK;
        for (side = 0; side < 2; side++) {
            S->BuzzStep[side] = (T2DspCompTarget(g_T2DspBuzzBand, S->BuzzEnv[side]) - S->BuzzGain[side]) /
                                (long long)T2DSP_COMP_BLOCK;
        }
    }
    S->BuzzCount--;
    for (side = 0; side < 2; side++) {
        W[side] = T2DspSat(rest[side] + T2DspSar(band[side] * S->BuzzGain[side], 30));
        S->BuzzGain[side] += S->BuzzStep[side];
    }
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

// Lookahead peak limiter on Count (1 or 2) linked samples (full scale 2^31,
// |x| < 2^40), in place: the samples come out T2DSP_LOOKAHEAD frames later,
// scaled so that they stay at or below Threshold. Each new frame records the
// gain it needs; the applied gain moves down along the steepest straight line
// that reaches every recorded need by the time that frame leaves, so a peak
// is met by a ramp of up to 0.5 ms instead of a jump. With nothing pending it
// releases toward 1 (or toward the lowest need in the window). *Limited
// counts frames that exceeded the threshold on entry.
static __inline void
T2DspLimit(T2DSP_LIMITER *Lim, long long *Samples, unsigned int Count, long long Threshold,
           unsigned long *Limited)
{
    const unsigned int n = T2DSP_LOOKAHEAD + 1;
    long long peak = 0;
    long long need = T2DSP_GAIN_ONE;
    long long g = Lim->Gain;
    unsigned int i, pos, age;

    for (i = 0; i < Count; i++) {
        long long a = Samples[i] < 0 ? -Samples[i] : Samples[i];
        if (a > peak) {
            peak = a;
        }
    }
    if (peak > Threshold) {
        // need = threshold / peak in Q30 (peak < 2^40, threshold < 2^31).
        need = (Threshold << 30) / peak;
        (*Limited)++;
    }

    // The newest frame replaces the one that left last time.
    pos = (Lim->Pos + 1u) % n;
    if (Lim->Need[pos] < T2DSP_GAIN_ONE) {
        Lim->Pending--;
    }
    Lim->Need[pos] = need;
    Lim->Delay[pos][0] = Samples[0];
    Lim->Delay[pos][1] = (Count > 1) ? Samples[1] : 0;
    if (need < T2DSP_GAIN_ONE) {
        Lim->Pending++;
    }
    Lim->Pos = pos;

    // Frame of age a (0 = leaving now, T2DSP_LOOKAHEAD = newest) sits at
    // slot (pos + 1 + a) % n and must be met within a + 1 gain steps.
    if (Lim->Pending == 0) {
        g += (T2DSP_GAIN_ONE - g) >> T2DSP_RELEASE_SHIFT;
    } else {
        long long step = T2DSP_GAIN_ONE;
        long long lowest = T2DSP_GAIN_ONE;

        for (age = 0; age < n; age++) {
            long long v = Lim->Need[(pos + 1u + age) % n];
            long long s = T2DspSar((v - g) * g_T2DspRecipQ30[age + 1u], 30);

            if (s < step) {
                step = s;
            }
            if (v < lowest) {
                lowest = v;
            }
        }
        if (step < 0) {
            g += step;
        } else {
            // Every need is above g: release, never past the lowest one.
            g += (lowest - g) >> T2DSP_RELEASE_SHIFT;
        }
    }
    if (g < 1) {
        g = 1;
    }
    Lim->Gain = g;

    pos = (pos + 1u) % n;   // the oldest frame, leaving now
    for (i = 0; i < Count; i++) {
        long long y = T2DspSar(Lim->Delay[pos][i] * (g >> 8), 22);

        // Rounding of the ramp can leave an LSB over the threshold.
        if (y > Threshold) {
            y = Threshold;
        } else if (y < -Threshold) {
            y = -Threshold;
        }
        Samples[i] = T2DspSat(y);
    }
}

// Autogain on one stereo frame at the input of the chain (internal scale,
// full scale 2^27), in place; see T2DSP_AGC_*. The level is measured on the
// input before the gain (feed-forward), so it cannot oscillate.
static __inline void
T2DspAutoGain(T2DSP_STATE *S, long long *L, long long *R)
{
    long long a = T2DspSar(*L, 8);   // |a| < 2^23 after saturation
    long long b = T2DspSar(*R, 8);
    long long p = (a * a + b * b) >> 1;

    S->AgcEnv += T2DspSar(p - S->AgcEnv, T2DSP_AGC_ENV_SHIFT);
    if (S->AgcCount == 0) {
        // RMS in log2 relative to a full-scale square (power 2^38 at 2^19).
        long long level = T2DspSar(T2DspLog2Q16(S->AgcEnv) - (38ll << 16), 1);
        long long target = S->AgcLog;
        long long lin;

        if (level > T2DSP_AGC_GATE) {
            target = T2DSP_AGC_TARGET - level;
            if (target > T2DSP_AGC_MAX_BOOST) {
                target = T2DSP_AGC_MAX_BOOST;
            } else if (target < T2DSP_AGC_MAX_CUT) {
                target = T2DSP_AGC_MAX_CUT;
            }
        }
        S->AgcLog += T2DspSar(target - S->AgcLog,
                              target < S->AgcLog ? T2DSP_AGC_DOWN_SHIFT : T2DSP_AGC_UP_SHIFT);
        if (S->AgcLog <= 0) {
            lin = T2DspExp2Q30(S->AgcLog);
        } else {
            lin = T2DspExp2Q30(S->AgcLog - 65536) << 1;
        }
        S->AgcCount = T2DSP_COMP_BLOCK;
        S->AgcStep = T2DspSar(lin - S->AgcGain, 4);   // T2DSP_COMP_BLOCK = 16
    }
    S->AgcCount--;
    // |x| < 2^31, gain < 2^31: the products stay below 2^62.
    *L = T2DspSat(T2DspSar(*L * S->AgcGain, 30));
    *R = T2DspSat(T2DspSar(*R * S->AgcGain, 30));
    S->AgcGain += S->AgcStep;
}

// Render Frames stereo frames (MSB-aligned 32-bit host samples, L then R)
// into the 6-channel ring through the DSP. Level is T2DSP_LEVEL_LINEAR,
// T2DSP_LEVEL_VIRTBASS, T2DSP_LEVEL_COMP (virtual bass + compressor) or
// T2DSP_LEVEL_BUZZ (all of it + the woofer buzz compressor). GainDb is the
// make-up gain (0..12 dB) in front of the limiter; TweeterTrimDb (0..12 dB)
// lifts the tweeters only; WooferLimitDb (0..12 dB) lowers the woofers'
// limiter ceiling below -1 dBFS. AttenuationShift is the same safety cut as the
// plain path (applied to the input); Shift converts to the ring word (8 for
// S24_LE). AutoGain (0/1) enables T2DspAutoGain in front of the chain.
// The output lags the input by T2DSP_LOOKAHEAD frames (limiter).
static __inline void
T2DspRenderStereoToSpeakers(volatile unsigned int *Dest,
                            const unsigned int *Source,
                            unsigned long Frames,
                            unsigned int Shift,
                            unsigned int AttenuationShift,
                            unsigned int Level,
                            unsigned int GainDb,
                            unsigned int TweeterTrimDb,
                            unsigned int WooferLimitDb,
                            unsigned int AutoGain,
                            T2DSP_STATE *S)
{
    unsigned long f;
    long long gain = T2DspGainFromDb(GainDb);
    // Tweeter trim: extra 0..12 dB on the tweeters only, on top of the
    // make-up gain (the chain leaves them ~13 dB under the woofers).
    long long tgain = T2DspSar(gain * T2DspGainFromDb(TweeterTrimDb), T2DSP_Q);
    // Woofer limiter ceiling: WooferLimitDb (0..12 dB) below the -1 dBFS
    // threshold, against woofer overload / rattle at full volume.
    long long wooferThreshold = (T2DSP_LIMIT_THRESHOLD << T2DSP_Q) / T2DspGainFromDb(WooferLimitDb);

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
        }
        if (AutoGain != 0) {
            T2DspAutoGain(S, &in[0], &in[1]);
        }
        for (side = 0; side < 2; side++) {
            if (Level >= T2DSP_LEVEL_VIRTBASS) {
                in[side] = T2DspVirtBass(S->VirtBass[side], in[side]);
            }
            in[side] = T2DspCascade(g_T2DspTone1, S->Tone1[side], T2DSP_TONE1_STAGES, in[side]);
        }
        T2DspXtc(S, &in[0], &in[1]);
        for (side = 0; side < 2; side++) {
            in[side] = T2DspCascade(g_T2DspTone2, S->Tone2[side], T2DSP_TONE2_STAGES, in[side]);
        }
        if (Level >= T2DSP_LEVEL_COMP) {
            T2DspComp(S, &in[0], &in[1]);
        }
        for (side = 0; side < 2; side++) {
            long long x = in[side];

            w[side] = T2DspCascade(g_T2DspWoofer, S->Woofer[side], T2DSP_WOOFER_STAGES, x);
            t[side] = T2DspCascade(g_T2DspTweeter, S->Tweeter[side], T2DSP_TWEETER_STAGES, x);
        }
        if (Level >= T2DSP_LEVEL_BUZZ) {
            T2DspBuzz(S, w);
        }
        for (side = 0; side < 2; side++) {
            // Back to full scale (2^31) for the limiter and the ring, with
            // the make-up gain and, on the tweeters, the trim. Gains are
            // < 2^30 (woofer) and < 2^32 (tweeter, Q28); the tweeter is first
            // held to +12 dB over full scale (2^29 internal), so both products
            // stay below 2^62 and the results below 2^38 (limiter: < 2^40).
            long long tc = t[side];

            if (tc > (1ll << 29)) {
                tc = 1ll << 29;
            } else if (tc < -(1ll << 29)) {
                tc = -(1ll << 29);
            }
            w[side] = T2DspSar(w[side] * gain, T2DSP_Q - T2DSP_INTERNAL_SHIFT);
            t[side] = T2DspSar(tc * tgain, T2DSP_Q - T2DSP_INTERNAL_SHIFT);
        }
        lw[0] = w[0];
        rw[0] = w[1];
        tw[0] = t[0];
        tw[1] = t[1];
        {
            unsigned long woofersBefore = S->WooferLimitedFrames;

            T2DspLimit(&S->Limiter[0], lw, 1, wooferThreshold, &S->WooferLimitedFrames);
            T2DspLimit(&S->Limiter[1], rw, 1, wooferThreshold, &S->WooferLimitedFrames);
            T2DspLimit(&S->Limiter[2], tw, 2, T2DSP_LIMIT_THRESHOLD, &S->LimitedFrames);
            S->LimitedFrames += S->WooferLimitedFrames - woofersBefore;
        }

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
