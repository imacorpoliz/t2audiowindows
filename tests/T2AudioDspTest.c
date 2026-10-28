// Host-side test for the speaker DSP in ../src/T2AudioDsp.h (the exact code
// compiled into the driver): the fixed-point chain is compared with a
// double-precision model of the same KAIT2EN graph built from the original
// coefficients, and checked for stability, headroom and the limiter.
//
//   cl /nologo /W4 /Fe:tests\T2AudioDspTest.exe tests\T2AudioDspTest.c
//   gcc -Wall -Wextra -Werror -o T2AudioDspTest tests/T2AudioDspTest.c -lm

#define T2AUDIO_DSP_REFERENCE
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include "../src/T2AudioDsp.h"

static int g_failures = 0;
static unsigned int g_level = T2DSP_LEVEL_LINEAR;   // DSP level under test

static void check(const char *name, int condition)
{
    printf("  [%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) {
        g_failures++;
    }
}

static double ring_value(unsigned int word)
{
    // S24_LE word (shift 8): sign-extended 24-bit value.
    long long v = (word & 0x80000000u) ? (long long)word - 0x100000000ll : (long long)word;
    return (double)v / 8388608.0;
}

// ---- double-precision reference of the same chain ---------------------
typedef struct { double x1, x2, y1, y2; } RBQ;
typedef struct {
    RBQ xo[2][14];
    double env[4], g[4], step[4];
    unsigned int count;
} RCOMP;

typedef struct {
    RBQ xo[2][9];
    double env[2], g[2], step[2];
    unsigned int count;
} RBUZZ;

typedef struct {
    RBUZZ buzz;
    RCOMP comp;
    RBQ vb[2][4];
    RBQ t1[2][T2DSP_TONE1_STAGES], t2[2][T2DSP_TONE2_STAGES];
    RBQ w[2][T2DSP_WOOFER_STAGES], tw[2][T2DSP_TWEETER_STAGES];
    double mid[T2DSP_XTC_DELAY + 1], side[T2DSP_XTC_TAPS];
    unsigned int mp, sp;
} REF;

static double rbq(const double *c, RBQ *s, double x)
{
    double y = c[0] * x + c[1] * s->x1 + c[2] * s->x2 - c[3] * s->y1 - c[4] * s->y2;
    s->x2 = s->x1; s->x1 = x; s->y2 = s->y1; s->y1 = y;
    return y;
}

static double rcas(const double (*c)[5], RBQ *s, unsigned int n, double x)
{
    unsigned int i;
    for (i = 0; i < n; i++) x = rbq(c[i], &s[i], x);
    return x;
}

static void rcomp_reset(RCOMP *c)
{
    int k;
    memset(c, 0, sizeof(*c));
    for (k = 0; k < 4; k++) c->g[k] = 1.0;
}

// Double model of T2DspComp (same structure, exact math).
static void rcomp(RCOMP *c, double *L, double *R)
{
    double band[2][4], *io[2] = { L, R };
    int s, k;

    for (s = 0; s < 2; s++) {
        RBQ *x = c->xo[s];
        double in = *io[s] * g_T2DspRefCompInputGain;
        double lo = rbq(g_T2DspRefCompXover[0], &x[1], rbq(g_T2DspRefCompXover[0], &x[0], in));
        double hi = rbq(g_T2DspRefCompXover[1], &x[3], rbq(g_T2DspRefCompXover[1], &x[2], in));
        lo = rbq(g_T2DspRefCompXover[2], &x[4], lo);
        hi = rbq(g_T2DspRefCompXover[3], &x[5], hi);
        band[s][0] = rbq(g_T2DspRefCompXover[4], &x[7], rbq(g_T2DspRefCompXover[4], &x[6], lo));
        band[s][1] = rbq(g_T2DspRefCompXover[5], &x[9], rbq(g_T2DspRefCompXover[5], &x[8], lo));
        band[s][2] = rbq(g_T2DspRefCompXover[6], &x[11], rbq(g_T2DspRefCompXover[6], &x[10], hi));
        band[s][3] = rbq(g_T2DspRefCompXover[7], &x[13], rbq(g_T2DspRefCompXover[7], &x[12], hi));
    }
    for (k = 0; k < 4; k++) {
        double p = fmax(fabs(band[0][k]), fabs(band[1][k]));
        double coef = p > c->env[k] ? g_T2DspRefCompBand[k][3] : g_T2DspRefCompBand[k][4];
        c->env[k] += (p - c->env[k]) * coef;
    }
    if (c->count == 0) {
        c->count = T2DSP_COMP_BLOCK;
        for (k = 0; k < 4; k++) {
            double t = 1.0;
            if (c->env[k] > g_T2DspRefCompBand[k][0]) {
                double over = log2(c->env[k] / g_T2DspRefCompBand[k][0]);
                t = pow(2.0, -over * (1.0 - 1.0 / g_T2DspRefCompBand[k][1]));
            }
            t *= g_T2DspRefCompBand[k][2];
            c->step[k] = (t - c->g[k]) / T2DSP_COMP_BLOCK;
        }
    }
    c->count--;
    for (s = 0; s < 2; s++) {
        double y = 0;
        for (k = 0; k < 4; k++) y += band[s][k] * c->g[k];
        *io[s] = y;
    }
    for (k = 0; k < 4; k++) c->g[k] += c->step[k];
}

static void rbuzz_reset(RBUZZ *b)
{
    memset(b, 0, sizeof(*b));
    b->g[0] = b->g[1] = 1.0;
}

static double rbuzz_target(double env)
{
    double t = 1.0;
    if (env > g_T2DspRefBuzzBand[0]) {
        t = pow(2.0, -log2(env / g_T2DspRefBuzzBand[0]) * (1.0 - 1.0 / g_T2DspRefBuzzBand[1]));
    }
    return t * g_T2DspRefBuzzBand[2];
}

// Double model of T2DspBuzz.
static void rbuzz(RBUZZ *b, double *w)
{
    double band[2], rest[2];
    int s;

    for (s = 0; s < 2; s++) {
        RBQ *x = b->xo[s];
        double lo = rbq(g_T2DspRefBuzzXover[0], &x[1], rbq(g_T2DspRefBuzzXover[0], &x[0], w[s]));
        double hi = rbq(g_T2DspRefBuzzXover[1], &x[3], rbq(g_T2DspRefBuzzXover[1], &x[2], w[s]));
        double a, coef;
        hi = rbq(g_T2DspRefBuzzXover[2], &x[4], hi);
        band[s] = rbq(g_T2DspRefBuzzXover[4], &x[8], rbq(g_T2DspRefBuzzXover[4], &x[7], lo));
        rest[s] = hi + rbq(g_T2DspRefBuzzXover[3], &x[6], rbq(g_T2DspRefBuzzXover[3], &x[5], lo));
        a = fabs(band[s]);
        coef = a > b->env[s] ? g_T2DspRefBuzzBand[3] : g_T2DspRefBuzzBand[4];
        b->env[s] += (a - b->env[s]) * coef;
    }
    if (b->count == 0) {
        b->count = T2DSP_COMP_BLOCK;
        for (s = 0; s < 2; s++) b->step[s] = (rbuzz_target(b->env[s]) - b->g[s]) / T2DSP_COMP_BLOCK;
    }
    b->count--;
    for (s = 0; s < 2; s++) {
        w[s] = rest[s] + band[s] * b->g[s];
        b->g[s] += b->step[s];
    }
}

// out[6]: ring channels, full scale 1.0 (no limiter, no attenuation).
static void ref_frame(REF *r, double l, double rr, double *out)
{
    double in[2] = { l, rr }, mid, side, md, acc = 0;
    unsigned int s, i, idx;

    if (g_level >= T2DSP_LEVEL_VIRTBASS) {
        for (s = 0; s < 2; s++) {
            double p = rbq(g_T2DspRefVirtBass[1], &r->vb[s][1], rbq(g_T2DspRefVirtBass[0], &r->vb[s][0], in[s]));
            double wet = g_T2DspRefVb[2] * tanh(p * g_T2DspRefVb[0]);
            if (p <= 0) wet += g_T2DspRefVb[3] * tanh(p * g_T2DspRefVb[1]);
            wet = rbq(g_T2DspRefVirtBass[3], &r->vb[s][3], rbq(g_T2DspRefVirtBass[2], &r->vb[s][2], wet));
            in[s] += wet;
        }
    }
    for (s = 0; s < 2; s++) in[s] = rcas(g_T2DspRefTone1, r->t1[s], T2DSP_TONE1_STAGES, in[s]);
    mid = 0.5 * (in[0] + in[1]);
    side = 0.5 * (in[0] - in[1]);
    r->mp = (r->mp + 1) % (T2DSP_XTC_DELAY + 1);
    r->mid[r->mp] = mid;
    md = r->mid[(r->mp + 1) % (T2DSP_XTC_DELAY + 1)];
    r->sp = (r->sp + 1) % T2DSP_XTC_TAPS;
    r->side[r->sp] = side;
    idx = r->sp;
    for (i = 0; i < T2DSP_XTC_TAPS; i++) {
        acc += g_T2DspRefXtcFir[i] * r->side[idx];
        idx = idx ? idx - 1 : T2DSP_XTC_TAPS - 1;
    }
    in[0] = md + acc;
    in[1] = md - acc;
    for (s = 0; s < 2; s++) in[s] = rcas(g_T2DspRefTone2, r->t2[s], T2DSP_TONE2_STAGES, in[s]);
    if (g_level >= T2DSP_LEVEL_COMP) rcomp(&r->comp, &in[0], &in[1]);
    {
        double w[2], t[2];
        for (s = 0; s < 2; s++) {
            w[s] = rcas(g_T2DspRefWoofer, r->w[s], T2DSP_WOOFER_STAGES, in[s]);
            t[s] = rcas(g_T2DspRefTweeter, r->tw[s], T2DSP_TWEETER_STAGES, in[s]);
        }
        if (g_level >= T2DSP_LEVEL_BUZZ) rbuzz(&r->buzz, w);
        for (s = 0; s < 2; s++) {
            out[s * 3 + 0] = w[s];
            out[s * 3 + 1] = w[s];
            out[s * 3 + 2] = t[s];
        }
    }
}

static unsigned int host_word(double v)
{
    long long s = (long long)floor(v * 2147483647.0 + 0.5);
    if (s > 2147483647ll) s = 2147483647ll;
    if (s < -2147483648ll) s = -2147483648ll;
    return (unsigned int)(s & 0xFFFFFFFFll);
}

// Steady-state peak of ring channel ch for a sine of amp at hz on both
// sides (phase-shifted on the right by rphase), fixed point vs reference.
static void sine_gain(double hz, double amp, double rphase, int ch, double *fixedDb, double *refDb)
{
    static T2DSP_STATE st;
    REF ref;
    unsigned int src[2], dst[6];
    double out[6], pf = 0, pr = 0;
    int n, total = 48000 + 9600;

    T2DspReset(&st);
    memset(&ref, 0, sizeof(ref));
    rcomp_reset(&ref.comp);
    rbuzz_reset(&ref.buzz);
    for (n = 0; n < total; n++) {
        double ph = 2.0 * 3.14159265358979323846 * hz * n / 48000.0;
        double l = amp * sin(ph), r = amp * sin(ph + rphase);
        src[0] = host_word(l);
        src[1] = host_word(r);
        T2DspRenderStereoToSpeakers(dst, src, 1, 8, 0, g_level, 0, 0, 0, &st);
        ref_frame(&ref, l, r, out);
        if (n >= 48000) {
            double a = fabs(ring_value(dst[ch]));
            int rc = (ch < 3) ? ch : ch;   // ring order == ref order
            if (a > pf) pf = a;
            if (fabs(out[rc]) > pr) pr = fabs(out[rc]);
        }
    }
    *fixedDb = 20.0 * log10(pf > 1e-12 ? pf / amp : 1e-12);
    *refDb = 20.0 * log10(pr > 1e-12 ? pr / amp : 1e-12);
}

// Allowed deviation from the double reference at a given level (dB rel. to
// the input): the 24-bit output floor is about -118 dB below a 0.1 input.
static double tol(double refDb)
{
    if (refDb > -60) return 0.05;
    if (refDb > -100) return 0.5;
    return 1000.0;
}

static void test_match_reference(void)
{
    static const double freqs[] = { 50, 100, 200, 500, 1000, 2000, 3000, 5000, 8000, 12000, 16000 };
    unsigned int i;
    int ok = 1;

    printf("Fixed point vs double reference (amp 0.1, in-phase stereo)\n");
    printf("     Hz   woofer(fix/ref dB)   tweeter(fix/ref dB)\n");
    for (i = 0; i < sizeof(freqs) / sizeof(freqs[0]); i++) {
        double wf, wr, tf, tr;

        sine_gain(freqs[i], 0.1, 0.0, T2AUDIO_SPK_LEFT_WOOFER_A, &wf, &wr);
        sine_gain(freqs[i], 0.1, 0.0, T2AUDIO_SPK_LEFT_TWEETER, &tf, &tr);
        printf("  %6.0f   %7.2f / %7.2f     %7.2f / %7.2f\n", freqs[i], wf, wr, tf, tr);
        // Compare where the signal is well above the 24-bit floor.
        if (fabs(wf - wr) > tol(wr)) ok = 0;
        if (fabs(tf - tr) > tol(tr)) ok = 0;
    }
    check("fixed point matches the reference (0.05 dB above -60 dB, 0.5 dB above -100 dB)", ok);

    // Left and right 90 degrees apart: exercises the XTC side path.
    ok = 1;
    for (i = 0; i < sizeof(freqs) / sizeof(freqs[0]); i++) {
        double rf, rr, tf, tr;

        sine_gain(freqs[i], 0.1, 1.5707963267948966, T2AUDIO_SPK_RIGHT_WOOFER_A, &rf, &rr);
        sine_gain(freqs[i], 0.1, 1.5707963267948966, T2AUDIO_SPK_RIGHT_TWEETER, &tf, &tr);
        if (fabs(rf - rr) > tol(rr) || fabs(tf - tr) > tol(tr)) ok = 0;
    }
    check("stereo (90 deg) through XTC matches the reference on the right side", ok);
}

static void test_shape(void)
{
    double f, r;

    printf("Response shape (from the KAIT2EN 16_1 graph)\n");
    sine_gain(30, 0.1, 0.0, T2AUDIO_SPK_LEFT_WOOFER_A, &f, &r);
    check("woofer: 30 Hz cut by Tone1 (< -40 dB)", f < -40);
    sine_gain(100, 0.1, 0.0, T2AUDIO_SPK_LEFT_WOOFER_A, &f, &r);
    check("woofer: 100 Hz passes (> -6 dB)", f > -6);
    sine_gain(4000, 0.1, 0.0, T2AUDIO_SPK_LEFT_WOOFER_A, &f, &r);
    check("woofer: 4 kHz crossed over (< -50 dB)", f < -50);
    sine_gain(200, 0.1, 0.0, T2AUDIO_SPK_LEFT_TWEETER, &f, &r);
    check("tweeter: 200 Hz blocked (< -90 dB)", f < -90);
    sine_gain(8000, 0.1, 0.0, T2AUDIO_SPK_LEFT_TWEETER, &f, &r);
    check("tweeter: 8 kHz passes at about -13 dB", f > -18 && f < -8);
}

static void test_stability(void)
{
    static T2DSP_STATE st;
    unsigned int src[2] = { 0, 0 }, dst[6];
    int n, nonzero = 0;
    double tail = 0;

    printf("Stability\n");
    T2DspReset(&st);
    for (n = 0; n < 48000; n++) {
        T2DspRenderStereoToSpeakers(dst, src, 1, 8, 0, g_level, 0, 0, 0, &st);
        if (dst[0] | dst[2] | dst[3] | dst[5]) nonzero++;
    }
    check("silence in -> exact silence out", nonzero == 0);

    // Impulse, then 2 s of silence: the output must decay to (near) zero.
    T2DspReset(&st);
    src[0] = src[1] = 0x7FFFFF00u;
    T2DspRenderStereoToSpeakers(dst, src, 1, 8, 0, g_level, 0, 0, 0, &st);
    src[0] = src[1] = 0;
    for (n = 0; n < 96000; n++) {
        T2DspRenderStereoToSpeakers(dst, src, 1, 8, 0, g_level, 0, 0, 0, &st);
        if (n > 95000) {
            int c;
            for (c = 0; c < 6; c++) if (fabs(ring_value(dst[c])) > tail) tail = fabs(ring_value(dst[c]));
        }
    }
    check("impulse response decays (< -120 dB after 2 s)", tail < 1e-6);
}

static void test_headroom_and_limiter(void)
{
    static T2DSP_STATE st;
    unsigned int src[2], dst[6];
    int n;
    double peak = 0;

    printf("Headroom and limiter\n");
    // Full-scale 100 Hz sine: the woofer chain boosts it by ~1 dB, so the
    // limiter must hold the woofers at the -1 dBFS threshold.
    T2DspReset(&st);
    for (n = 0; n < 48000; n++) {
        double s = 0.999 * sin(2.0 * 3.14159265358979323846 * 100.0 * n / 48000.0);
        int c;
        src[0] = src[1] = host_word(s);
        T2DspRenderStereoToSpeakers(dst, src, 1, 8, 0, g_level, 0, 0, 0, &st);
        for (c = 0; c < 6; c++) if (fabs(ring_value(dst[c])) > peak) peak = fabs(ring_value(dst[c]));
    }
    printf("  full-scale 100 Hz: output peak %.4f, limited frames %lu\n", peak, st.LimitedFrames);
    check("output never exceeds the -1 dBFS threshold (+2 LSB)", peak <= 0.8912509 + 2.0 / 8388608.0);
    if (g_level >= T2DSP_LEVEL_COMP) {
        // The compressor (band 0: -24 dB, ratio 4) already holds the bass
        // well below the limiter threshold.
        check("compressor keeps a full-scale 100 Hz below -6 dBFS", peak < 0.5);
    } else {
        check("limiter engaged on the full-scale 100 Hz sine", st.LimitedFrames > 0);
    }

    // Full-scale square at 80 Hz: no overflow anywhere.
    T2DspReset(&st);
    peak = 0;
    for (n = 0; n < 48000; n++) {
        unsigned int v = ((n / 300) & 1) ? 0x80000000u : 0x7FFFFFFFu;
        int c;
        src[0] = src[1] = v;
        T2DspRenderStereoToSpeakers(dst, src, 1, 8, 0, g_level, 0, 0, 0, &st);
        for (c = 0; c < 6; c++) if (fabs(ring_value(dst[c])) > peak) peak = fabs(ring_value(dst[c]));
    }
    check("full-scale square: output bounded by the threshold", peak <= 0.8912509 + 2.0 / 8388608.0);

    // Quiet signal: limiter idle.
    T2DspReset(&st);
    for (n = 0; n < 48000; n++) {
        double s = 0.05 * sin(2.0 * 3.14159265358979323846 * 440.0 * n / 48000.0);
        src[0] = src[1] = host_word(s);
        T2DspRenderStereoToSpeakers(dst, src, 1, 8, 0, g_level, 0, 0, 0, &st);
    }
    check("limiter idle at -26 dBFS", st.LimitedFrames == 0);

    // Attenuation shift works through the DSP (-6 dB per step).
    {
        double a0, a1, r;
        (void)r;
        sine_gain(1000, 0.1, 0.0, T2AUDIO_SPK_LEFT_WOOFER_A, &a0, &r);
        T2DspReset(&st);
        {
            double pf = 0;
            for (n = 0; n < 57600; n++) {
                double s = 0.1 * sin(2.0 * 3.14159265358979323846 * 1000.0 * n / 48000.0);
                src[0] = src[1] = host_word(s);
                T2DspRenderStereoToSpeakers(dst, src, 1, 8, 1, g_level, 0, 0, 0, &st);
                if (n >= 48000 && fabs(ring_value(dst[0])) > pf) pf = fabs(ring_value(dst[0]));
            }
            a1 = 20.0 * log10(pf / 0.1);
        }
        check("attenuation 1 lowers the output by 6 dB", fabs((a0 - a1) - 6.02) < 0.1);
    }
}

static void test_cost(void)
{
    static T2DSP_STATE st;
    static unsigned int src[96 * 2], dst[96 * 6];
    clock_t t0, t1;
    int i;
    double us;

    T2DspReset(&st);
    for (i = 0; i < 96 * 2; i++) src[i] = host_word(0.1 * sin(i * 0.1));
    t0 = clock();
    for (i = 0; i < 5000; i++) T2DspRenderStereoToSpeakers(dst, src, 96, 8, 0, g_level, 0, 0, 0, &st);
    t1 = clock();
    us = (double)(t1 - t0) / CLOCKS_PER_SEC * 1e6 / 5000.0;
    printf("Cost: %.1f us per 2 ms DPC tick (96 frames) on this host\n", us);
}

// Level 2: the fixed-point virtual bass matches the double reference, adds
// harmonics of a 60 Hz tone in the 200-450 Hz band, and leaves a 1 kHz
// tone alone. Harmonic content is measured on the woofer output.
static double tone_band_energy(double hz, double amp, double lo, double hi, int fixed)
{
    static T2DSP_STATE st;
    REF ref;
    unsigned int src[2], dst[6];
    double out[6], e = 0;
    int n, total = 48000;
    double bin;

    T2DspReset(&st);
    memset(&ref, 0, sizeof(ref));
    rcomp_reset(&ref.comp);
    rbuzz_reset(&ref.buzz);
    // Goertzel-free: correlate with sines on a grid of 10 Hz bins.
    {
        static double y[48000];
        for (n = 0; n < total; n++) {
            double s = amp * sin(2.0 * 3.14159265358979323846 * hz * n / 48000.0);
            src[0] = src[1] = host_word(s);
            if (fixed) {
                T2DspRenderStereoToSpeakers(dst, src, 1, 8, 0, g_level, 0, 0, 0, &st);
                y[n] = ring_value(dst[T2AUDIO_SPK_LEFT_WOOFER_A]);
            } else {
                ref_frame(&ref, s, s, out);
                y[n] = out[0];
            }
        }
        for (bin = lo; bin <= hi; bin += 10.0) {
            double c = 0, sn = 0;
            for (n = 24000; n < total; n++) {
                double ph = 2.0 * 3.14159265358979323846 * bin * n / 48000.0;
                c += y[n] * cos(ph);
                sn += y[n] * sin(ph);
            }
            e += (c * c + sn * sn) / (24000.0 * 24000.0) * 4.0;
        }
    }
    return 10.0 * log10(e + 1e-30);
}

static void test_virtual_bass(void)
{
    double lin, vbf, vbr, wf, wr;

    printf("Virtual bass (SpeakerDsp=2)\n");
    g_level = T2DSP_LEVEL_LINEAR;
    lin = tone_band_energy(60, 0.25, 170, 460, 1);
    g_level = T2DSP_LEVEL_VIRTBASS;
    vbf = tone_band_energy(60, 0.25, 170, 460, 1);
    vbr = tone_band_energy(60, 0.25, 170, 460, 0);
    printf("  60 Hz tone, 180-450 Hz harmonics on the woofer: linear %.1f dB, virtual bass %.1f dB (ref %.1f dB)\n",
           lin, vbf, vbr);
    check("virtual bass adds harmonics (> +30 dB over the linear chain)", vbf > lin + 30);
    check("harmonics match the double reference (0.5 dB)", fabs(vbf - vbr) < 0.5);
    sine_gain(1000, 0.1, 0.0, T2AUDIO_SPK_LEFT_TWEETER, &wf, &wr);
    check("1 kHz tweeter level unchanged by the virtual bass (0.05 dB of ref)", fabs(wf - wr) < 0.05);
    g_level = T2DSP_LEVEL_LINEAR;
}

// Compressor alone: static curve at 730 Hz (band 1: threshold -12.85 dB,
// ratio 1.7, makeup +0.08 dB, input gain +2 dB), fixed vs double model.
static double comp_level(double amp, double hz, int fixed)
{
    static T2DSP_STATE st;
    RCOMP rc;
    double pk = 0;
    int n;

    T2DspReset(&st);
    rcomp_reset(&rc);
    for (n = 0; n < 48000; n++) {
        double s = amp * sin(2.0 * 3.14159265358979323846 * hz * n / 48000.0);
        double y;
        if (fixed) {
            long long l = (long long)floor(s * 134217728.0 + 0.5), r = l;   // 2^27 scale
            T2DspComp(&st, &l, &r);
            y = (double)l / 134217728.0;
        } else {
            double l = s, r = s;
            rcomp(&rc, &l, &r);
            y = l;
        }
        if (n >= 38400 && fabs(y) > pk) pk = fabs(y);
    }
    return 20.0 * log10(pk / amp);
}

// Buzz compressor alone on the woofer signal (2^27 scale): fixed vs model.
static double buzz_level(double amp, double hz, int fixed)
{
    static T2DSP_STATE st;
    RBUZZ rb;
    double pk = 0;
    int n;

    T2DspReset(&st);
    rbuzz_reset(&rb);
    for (n = 0; n < 48000; n++) {
        double s = amp * sin(2.0 * 3.14159265358979323846 * hz * n / 48000.0), y;
        if (fixed) {
            long long w[2];
            w[0] = w[1] = (long long)floor(s * 134217728.0 + 0.5);
            T2DspBuzz(&st, w);
            y = (double)w[0] / 134217728.0;
        } else {
            double w[2] = { s, s };
            rbuzz(&rb, w);
            y = w[0];
        }
        if (n >= 38400 && fabs(y) > pk) pk = fabs(y);
    }
    return 20.0 * log10(pk / amp);
}

static void test_buzz(void)
{
    double lv[] = { -30, -18, -12, -6, 0 };
    unsigned int i;
    int ok = 1;

    printf("Woofer buzz compressor (SpeakerDsp=4), 125 Hz gain vs level\n");
    for (i = 0; i < sizeof(lv) / sizeof(lv[0]); i++) {
        double amp = pow(10.0, lv[i] / 20.0);
        double f = buzz_level(amp, 125.5, 1), r = buzz_level(amp, 125.5, 0);
        printf("  in %5.1f dBFS: gain fixed %6.2f dB, model %6.2f dB\n", lv[i], f, r);
        if (fabs(f - r) > 0.1) ok = 0;
    }
    check("fixed point matches the double model (0.1 dB)", ok);
    check("125 Hz below threshold passes flat (|gain| < 0.2 dB)", fabs(buzz_level(pow(10.0, -30 / 20.0), 125.5, 1)) < 0.2);
    // The 110-145 Hz band is narrow: the LR4 split puts only part of a
    // 125 Hz tone into it (the rest stays in the neighbours), so the
    // overall gain changes by about 1 dB at full scale (as in lsp).
    check("125 Hz at 0 dBFS compressed (< -0.5 dB)", buzz_level(1.0, 125.5, 1) < -0.5);
    check("300 Hz at 0 dBFS not compressed (|gain| < 0.5 dB)", fabs(buzz_level(1.0, 300, 1)) < 0.5);
    check("60 Hz at 0 dBFS not compressed (|gain| < 0.5 dB)", fabs(buzz_level(1.0, 60, 1)) < 0.5);
}

// Make-up gain (DspGainDb): +6 dB is +6.02 dB on a quiet tone; at +12 dB a
// full-scale signal is still held at the limiter threshold.
static double tweeter_8k_peak(unsigned int gainDb, double amp)
{
    static T2DSP_STATE st;
    unsigned int src[2], dst[6];
    double pk = 0;
    int n;

    T2DspReset(&st);
    for (n = 0; n < 24000; n++) {
        double s = amp * sin(2.0 * 3.14159265358979323846 * 8000.0 * n / 48000.0);
        src[0] = src[1] = host_word(s);
        T2DspRenderStereoToSpeakers(dst, src, 1, 8, 0, T2DSP_LEVEL_LINEAR, gainDb, 0, 0, &st);
        if (n > 12000 && fabs(ring_value(dst[T2AUDIO_SPK_LEFT_TWEETER])) > pk) pk = fabs(ring_value(dst[T2AUDIO_SPK_LEFT_TWEETER]));
    }
    return pk;
}

static void test_makeup_gain(void)
{
    double g0 = tweeter_8k_peak(0, 0.05), g6 = tweeter_8k_peak(6, 0.05), g12 = tweeter_8k_peak(12, 0.999);

    printf("Make-up gain (DspGainDb)\n");
    check("+6 dB raises a quiet tone by 6.0 dB (+-0.05)", fabs(20.0 * log10(g6 / g0) - 6.0) < 0.05);
    check("+12 dB at full scale still capped by the limiter", g12 <= 0.8912509 + 2.0 / 8388608.0);
    check("gain table clamps above 12 dB", T2DspGainFromDb(40) == T2DspGainFromDb(12));
    {
        // Tweeter trim lifts the tweeter by 6 dB and leaves the woofer alone.
        static T2DSP_STATE a, b;
        unsigned int src[2], da[6], db[6];
        double ta = 0, tb = 0, wa = 0, wb = 0;
        int n;
        T2DspReset(&a);
        T2DspReset(&b);
        for (n = 0; n < 24000; n++) {
            double s = 0.05 * (sin(2.0 * 3.14159265358979323846 * 8000.0 * n / 48000.0) +
                               sin(2.0 * 3.14159265358979323846 * 150.0 * n / 48000.0));
            src[0] = src[1] = host_word(s);
            T2DspRenderStereoToSpeakers(da, src, 1, 8, 0, T2DSP_LEVEL_LINEAR, 0, 0, 0, &a);
            T2DspRenderStereoToSpeakers(db, src, 1, 8, 0, T2DSP_LEVEL_LINEAR, 0, 6, 0, &b);
            if (n > 12000) {
                ta = fmax(ta, fabs(ring_value(da[T2AUDIO_SPK_LEFT_TWEETER])));
                tb = fmax(tb, fabs(ring_value(db[T2AUDIO_SPK_LEFT_TWEETER])));
                wa = fmax(wa, fabs(ring_value(da[T2AUDIO_SPK_LEFT_WOOFER_A])));
                wb = fmax(wb, fabs(ring_value(db[T2AUDIO_SPK_LEFT_WOOFER_A])));
            }
        }
        check("TweeterTrimDb 6 lifts the tweeter by 6.0 dB (+-0.05)", fabs(20.0 * log10(tb / ta) - 6.0) < 0.05);
        check("TweeterTrimDb leaves the woofer unchanged", wa == wb);
    }
    {
        // Worst case: full-scale square at every level with +12 dB make-up
        // and +12 dB tweeter trim; no overflow, output held at the limiter.
        static T2DSP_STATE st;
        unsigned int src[2], dst[6];
        double pk = 0;
        unsigned int lvl;
        int n, c;
        for (lvl = 1; lvl <= T2DSP_LEVEL_MAX; lvl++) {
            T2DspReset(&st);
            for (n = 0; n < 24000; n++) {
                src[0] = src[1] = ((n / 3) & 1) ? 0x80000000u : 0x7FFFFFFFu;   // 8 kHz square
                if (n > 12000) src[1] = ((n / 200) & 1) ? 0x80000000u : 0x7FFFFFFFu;
                T2DspRenderStereoToSpeakers(dst, src, 1, 8, 0, lvl, 12, 12, 0, &st);
                for (c = 0; c < 6; c++) pk = fmax(pk, fabs(ring_value(dst[c])));
            }
        }
        check("+12 dB make-up and +12 dB trim at full scale: output held at -1 dBFS", pk <= 0.8912509 + 2.0 / 8388608.0);
    }
    {
        // WooferLimitDb 6: a full-scale 100 Hz is held 6 dB under -1 dBFS on
        // the woofers; the tweeters keep the -1 dBFS ceiling.
        static T2DSP_STATE st;
        unsigned int src[2], dst[6];
        double wpk = 0;
        int n;
        T2DspReset(&st);
        for (n = 0; n < 48000; n++) {
            src[0] = src[1] = host_word(0.999 * sin(2.0 * 3.14159265358979323846 * 100.0 * n / 48000.0));
            T2DspRenderStereoToSpeakers(dst, src, 1, 8, 0, T2DSP_LEVEL_LINEAR, 0, 0, 6, &st);
            if (n > 24000) wpk = fmax(wpk, fabs(ring_value(dst[T2AUDIO_SPK_LEFT_WOOFER_A])));
        }
        printf("  WooferLimitDb 6: woofer peak %.4f (ceiling %.4f), woofer-limited frames %lu\n",
               wpk, 0.8912509 / 1.9952623, st.WooferLimitedFrames);
        check("WooferLimitDb 6 holds the woofers at -7 dBFS", wpk <= 0.8912509 / 1.9952623 + 3.0 / 8388608.0 && wpk > 0.43);
        check("woofer limiter counted separately", st.WooferLimitedFrames > 0 && st.LimitedFrames >= st.WooferLimitedFrames);
    }
}

static void test_log_exp(void)
{
    long long e, v;
    double worstExp = 0, worstLog = 0;

    printf("log2 / exp2 tables\n");
    for (e = -30ll * 65536; e <= 0; e += 37) {
        double exact = pow(2.0, (double)e / 65536.0) * 1073741824.0;
        double got = (double)T2DspExp2Q30(e);
        double rel = fabs(got - exact) / exact;
        if (exact > 1e5 && rel > worstExp) worstExp = rel;
    }
    for (v = 1000; v < (1ll << 39); v += v / 997 + 1) {
        double err = fabs((double)T2DspLog2Q16(v) / 65536.0 - log2((double)v));
        if (err > worstLog) worstLog = err;
    }
    printf("  exp2 max rel error %.2e, log2 max abs error %.2e\n", worstExp, worstLog);
    check("exp2 relative error < 2e-5 over 2^-30..2^0", worstExp < 2e-5);
    check("log2 absolute error < 1e-4 over 1e3..2^39", worstLog < 1e-4);
}

static void test_compressor(void)
{
    double lv[] = { -40, -30, -20, -15, -10, -6, -3, 0 };
    unsigned int i;
    int ok = 1;
    double gLow, gHigh;

    printf("Multiband compressor (SpeakerDsp=3), 730 Hz gain vs input level\n");
    for (i = 0; i < sizeof(lv) / sizeof(lv[0]); i++) {
        double amp = pow(10.0, lv[i] / 20.0);
        double f = comp_level(amp, 730, 1), r = comp_level(amp, 730, 0);
        printf("  in %5.1f dBFS: gain fixed %6.2f dB, model %6.2f dB\n", lv[i], f, r);
        if (fabs(f - r) > 0.1) ok = 0;
    }
    check("fixed point matches the double model (0.1 dB)", ok);
    gLow = comp_level(pow(10.0, -40 / 20.0), 730, 1);
    gHigh = comp_level(1.0, 730, 1);
    check("below threshold: input gain + makeup only (about +2 dB)", gLow > 1.5 && gLow < 2.6);
    check("at 0 dBFS the band is compressed by > 4 dB vs quiet", gLow - gHigh > 4.0);
}

int main(void)
{
    test_match_reference();
    test_shape();
    test_stability();
    test_headroom_and_limiter();
    test_virtual_bass();
    test_makeup_gain();
    test_log_exp();
    test_compressor();
    test_buzz();
    g_level = T2DSP_LEVEL_BUZZ;
    printf("--- level 4 (virtual bass + compressor + woofer buzz compressor) ---\n");
    test_match_reference();
    test_stability();
    test_headroom_and_limiter();
    test_cost();
    g_level = T2DSP_LEVEL_COMP;
    printf("--- level 3 (virtual bass + compressor) ---\n");
    test_match_reference();
    test_stability();
    test_headroom_and_limiter();
    test_cost();
    g_level = T2DSP_LEVEL_VIRTBASS;
    printf("--- level 2 (virtual bass) ---\n");
    test_match_reference();
    test_stability();
    test_headroom_and_limiter();
    test_cost();
    g_level = T2DSP_LEVEL_LINEAR;
    test_cost();
    if (g_failures != 0) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
