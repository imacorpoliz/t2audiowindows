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

// out[6]: ring channels, full scale 1.0 (no limiter, no attenuation).
static void ref_frame(REF *r, double l, double rr, double *out)
{
    double in[2] = { l, rr }, mid, side, md, acc = 0;
    unsigned int s, i, idx;

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
    for (s = 0; s < 2; s++) {
        double x = rcas(g_T2DspRefTone2, r->t2[s], T2DSP_TONE2_STAGES, in[s]);
        double w = rcas(g_T2DspRefWoofer, r->w[s], T2DSP_WOOFER_STAGES, x);
        double t = rcas(g_T2DspRefTweeter, r->tw[s], T2DSP_TWEETER_STAGES, x);
        out[s * 3 + 0] = w;
        out[s * 3 + 1] = w;
        out[s * 3 + 2] = t;
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
    for (n = 0; n < total; n++) {
        double ph = 2.0 * 3.14159265358979323846 * hz * n / 48000.0;
        double l = amp * sin(ph), r = amp * sin(ph + rphase);
        src[0] = host_word(l);
        src[1] = host_word(r);
        T2DspRenderStereoToSpeakers(dst, src, 1, 8, 0, &st);
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
        T2DspRenderStereoToSpeakers(dst, src, 1, 8, 0, &st);
        if (dst[0] | dst[2] | dst[3] | dst[5]) nonzero++;
    }
    check("silence in -> exact silence out", nonzero == 0);

    // Impulse, then 2 s of silence: the output must decay to (near) zero.
    T2DspReset(&st);
    src[0] = src[1] = 0x7FFFFF00u;
    T2DspRenderStereoToSpeakers(dst, src, 1, 8, 0, &st);
    src[0] = src[1] = 0;
    for (n = 0; n < 96000; n++) {
        T2DspRenderStereoToSpeakers(dst, src, 1, 8, 0, &st);
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
        T2DspRenderStereoToSpeakers(dst, src, 1, 8, 0, &st);
        for (c = 0; c < 6; c++) if (fabs(ring_value(dst[c])) > peak) peak = fabs(ring_value(dst[c]));
    }
    printf("  full-scale 100 Hz: output peak %.4f, limited frames %lu\n", peak, st.LimitedFrames);
    check("output never exceeds the -1 dBFS threshold (+2 LSB)", peak <= 0.8912509 + 2.0 / 8388608.0);
    check("limiter engaged on the full-scale 100 Hz sine", st.LimitedFrames > 0);

    // Full-scale square at 80 Hz: no overflow anywhere.
    T2DspReset(&st);
    peak = 0;
    for (n = 0; n < 48000; n++) {
        unsigned int v = ((n / 300) & 1) ? 0x80000000u : 0x7FFFFFFFu;
        int c;
        src[0] = src[1] = v;
        T2DspRenderStereoToSpeakers(dst, src, 1, 8, 0, &st);
        for (c = 0; c < 6; c++) if (fabs(ring_value(dst[c])) > peak) peak = fabs(ring_value(dst[c]));
    }
    check("full-scale square: output bounded by the threshold", peak <= 0.8912509 + 2.0 / 8388608.0);

    // Quiet signal: limiter idle.
    T2DspReset(&st);
    for (n = 0; n < 48000; n++) {
        double s = 0.05 * sin(2.0 * 3.14159265358979323846 * 440.0 * n / 48000.0);
        src[0] = src[1] = host_word(s);
        T2DspRenderStereoToSpeakers(dst, src, 1, 8, 0, &st);
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
                T2DspRenderStereoToSpeakers(dst, src, 1, 8, 1, &st);
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
    for (i = 0; i < 5000; i++) T2DspRenderStereoToSpeakers(dst, src, 96, 8, 0, &st);
    t1 = clock();
    us = (double)(t1 - t0) / CLOCKS_PER_SEC * 1e6 / 5000.0;
    printf("Cost: %.1f us per 2 ms DPC tick (96 frames) on this host\n", us);
}

int main(void)
{
    test_match_reference();
    test_shape();
    test_stability();
    test_headroom_and_limiter();
    test_cost();
    if (g_failures != 0) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
