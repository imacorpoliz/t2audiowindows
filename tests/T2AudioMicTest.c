// Host tests for src/T2AudioMicLogic.h (microphone format, DSP, capture clock).
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include "../src/T2AudioMicLogic.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_failures;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); g_failures++; } } while (0)

static unsigned int raw24(int v) { return (unsigned int)v & 0x00FFFFFFu; }

static void test_sample24(void)
{
    CHECK(T2AudioMicSample24(raw24(0)) == 0, "zero");
    CHECK(T2AudioMicSample24(raw24(0x7FFFFF)) == 0x7FFFFF, "max");
    CHECK(T2AudioMicSample24(raw24(-1)) == -1, "-1");
    CHECK(T2AudioMicSample24(raw24(-0x800000)) == -0x800000, "min");
    // Garbage in the top byte is ignored.
    CHECK(T2AudioMicSample24(0xAB000010u) == 16, "top byte");
    CHECK(T2AudioMicSample24(0x12FFFFF0u) == -16, "top byte negative");
}

// Reference: the same chain in double precision.
static double ref_gain(double freq, double amp)
{
    const double b0 = 0.9926225427561189, b1 = -1.9852450855122379, b2 = 0.9926225427561189;
    const double a1 = -1.9851906578962613, a2 = 0.9852995131282146;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0, e = 0;
    long n;

    for (n = 0; n < 96000; n++) {
        double x = amp * sin(2 * M_PI * freq * n / 48000.0);
        double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        if (n >= 48000) e += (89.12509381337455 * y) * (89.12509381337455 * y);
    }
    return sqrt(e / 48000.0);
}

static double run_gain(double freq, double amp, double mic2, int *clipped)
{
    T2AUDIO_MIC_STATE st;
    unsigned int frame[3];
    double e = 0;
    long n;

    T2AudioMicReset(&st);
    *clipped = 0;
    for (n = 0; n < 96000; n++) {
        double x = amp * sin(2 * M_PI * freq * n / 48000.0);
        int v = (int)lrint(x * 8388607.0);
        int out;

        frame[0] = raw24(v);
        frame[1] = raw24(v);
        frame[2] = raw24((int)lrint(mic2 * 8388607.0));
        out = T2AudioMicProcessFrame(&st, frame, 1);
        if (out == (int)0x7FFFFF00 || out == (int)0x80000000) (*clipped)++;
        if (n >= 48000) {
            double o = (double)out / 2147483648.0;
            e += o * o;
        }
    }
    return sqrt(e / 48000.0);
}

static void test_dsp(void)
{
    static const double freqs[] = { 50, 200, 1000, 4000, 12000 };
    unsigned i;
    int clipped;

    for (i = 0; i < sizeof(freqs) / sizeof(freqs[0]); i++) {
        double amp = 0.001;   // -60 dBFS: quiet speech level before the +39 dB
        double got = run_gain(freqs[i], amp, 0.0, &clipped);
        double want = ref_gain(freqs[i], amp);
        double db = 20 * log10(got / want);

        CHECK(fabs(db) < 0.05, "%g Hz: %.3f dB from reference (got %g want %g)", freqs[i], db, got, want);
        CHECK(clipped == 0, "%g Hz clipped", freqs[i]);
    }
    // mic 2 is not used by the 16_1 graph.
    {
        double a = run_gain(1000, 0.001, 0.0, &clipped);
        double b = run_gain(1000, 0.001, 0.3, &clipped);
        CHECK(fabs(a - b) < 1e-9, "mic2 leaks into the output");
    }
    // DC is removed.
    {
        T2AUDIO_MIC_STATE st;
        unsigned int frame[3] = { raw24(20000), raw24(20000), 0 };
        int out = 0;
        long n;

        T2AudioMicReset(&st);
        for (n = 0; n < 48000 * 2; n++) out = T2AudioMicProcessFrame(&st, frame, 1);
        CHECK(out > -2560 && out < 2560, "DC not removed: %d", out);
    }
    // Loud input saturates instead of wrapping.
    run_gain(1000, 0.5, 0.0, &clipped);
    CHECK(clipped > 0, "loud input did not saturate");
    // Dsp = 0: mic 0 passes through, MSB-aligned.
    {
        T2AUDIO_MIC_STATE st;
        unsigned int frame[3] = { raw24(-1234), raw24(999), raw24(5) };
        T2AudioMicReset(&st);
        CHECK(T2AudioMicProcessFrame(&st, frame, 0) == -1234 * 256, "passthrough");
    }
}

static void test_capture_aim(void)
{
    long long off = 0;
    unsigned long long host = 1000;
    unsigned long long write = 50000;
    int corrections = 0;
    long i;

    // First anchor: aims LAG behind.
    CHECK(T2AudioMicCaptureAim(host, &off, write, 0) == 1, "first aim");
    CHECK((long long)write - ((long long)host + off) == T2AUDIO_MIC_CAPTURE_LAG, "lag after aim");
    // Both advance together with +-100 frames of jitter: no re-aim.
    for (i = 0; i < 100000; i++) {
        long jitter = (i * 7919) % 201 - 100;
        host += 96;
        write += 96;
        corrections += T2AudioMicCaptureAim(host, &off, (unsigned long long)((long long)write + jitter), 1);
    }
    CHECK(corrections == 0, "re-aimed %d times on jitter", corrections);
    // A writer 80 ppm fast: 2300 frames in 10 minutes, so a re-aim (a jump
    // of TOLERANCE frames back to LAG) about every 100 s, ~6 in total.
    corrections = 0;
    for (i = 0; i < 48000 * 600 / 96; i++) {
        host += 96;
        write += 96;
        if (i % 130 == 0) write += 1;   // ~80 ppm
        corrections += T2AudioMicCaptureAim(host, &off, write, 1);
    }
    CHECK(corrections >= 5 && corrections <= 7, "drift: %d re-aims", corrections);
    // START_IO restarts the T2 at 0: re-aim to a negative offset, slots stay valid.
    CHECK(T2AudioMicCaptureAim(host, &off, 100, 1) == 1, "restart aim");
    CHECK(off < 0, "negative offset");
    CHECK(T2AudioMicRingSlot((long long)host + off, T2AUDIO_MIC_RING_FRAMES) == 0, "negative frame -> slot 0");
    CHECK(T2AudioMicRingSlot(T2AUDIO_MIC_RING_FRAMES + 5, T2AUDIO_MIC_RING_FRAMES) == 5, "wrap");
}

// ---------------------------------------------------------------------------
// Three-microphone noise canceller (MicDsp=2)
// ---------------------------------------------------------------------------
static unsigned int g_lcg = 12345u;

static int noise24(int amp)   // uniform-ish white noise, |x| <= amp
{
    g_lcg = g_lcg * 1103515245u + 12345u;
    return (int)(((long long)((g_lcg >> 8) & 0xFFFF) - 32768) * amp / 32768);
}

static int speech24(long n, double amp)   // two tones as a stand-in for a talker
{
    return (int)lrint(amp * 8388607.0 *
                      (0.6 * sin(2 * M_PI * 310.0 * n / 48000.0) + 0.4 * sin(2 * M_PI * 1270.0 * n / 48000.0)));
}

#define ANC_HIST 64

// Runs the canceller on a scene: a talker in front (same at all three
// mics) and a noise source whose sound reaches mic 0 / 1 / 2 with the given
// delays. Returns the output power, the power of (output - delayed speech)
// and the speech gain, over the last second.
static void run_anc(double speechAmp, int noiseAmp, int d0, int d1, int d2, int independent,
                    double *outPow, double *residualPow, double *speechGain, double *voiceNoisePow,
                    unsigned int *resets)
{
    T2AUDIO_MIC_ANC_STATE a;
    int hist[ANC_HIST] = { 0 };
    long n, total = 48000 * 4;
    double po = 0, pr = 0, ps = 0, cross = 0, pn = 0;

    T2AudioMicAncReset(&a);
    g_lcg = 777u;
    for (n = 0; n < total; n++) {
        int s = (speechAmp > 0) ? speech24(n, speechAmp) : 0;
        int mic[3], out;
        long t = n % ANC_HIST;

        hist[t] = noise24(noiseAmp);
        if (independent) {
            mic[0] = s + noise24(noiseAmp);
            mic[1] = s + hist[t];
            mic[2] = s + noise24(noiseAmp);
        } else {
            mic[0] = s + hist[(t - d0 + ANC_HIST) % ANC_HIST];
            mic[1] = s + hist[(t - d1 + ANC_HIST) % ANC_HIST];
            mic[2] = s + hist[(t - d2 + ANC_HIST) % ANC_HIST];
        }
        out = T2AudioMicAncProcess(&a, mic, 1);
        CHECK(out >= -(1 << 23) && out < (1 << 23), "out of 24-bit range: %d", out);
        if (n >= total - 48000) {
            double sd = (speechAmp > 0) ? (double)speech24(n - T2AUDIO_MIC_ANC_DELAY, speechAmp) : 0.0;
            double vn = (double)(independent ? hist[t] : hist[(t - d1 + ANC_HIST) % ANC_HIST]);

            po += (double)out * out;
            pr += ((double)out - sd) * ((double)out - sd);
            ps += sd * sd;
            cross += (double)out * sd;
            pn += vn * vn;
        }
    }
    *outPow = po / 48000.0;
    *residualPow = pr / 48000.0;
    *speechGain = (ps > 0) ? cross / ps : 0.0;
    *voiceNoisePow = pn / 48000.0;
    *resets = a.Resets;
}

static void test_anc(void)
{
    double po, pr, gain, pn;
    unsigned int resets;

    // A talker alone: the references are empty, nothing adapts, the voice
    // comes through (delayed; only the DC blocker touches it).
    run_anc(0.05, 0, 0, 0, 0, 0, &po, &pr, &gain, &pn, &resets);
    CHECK(10 * log10(pr / po) < -30.0, "voice alone changed: residual %.1f dB", 10 * log10(pr / po));
    CHECK(fabs(20 * log10(gain)) < 0.05, "voice alone gain %.3f dB", 20 * log10(gain));
    // DC offsets on the channels (as on hardware) are removed, not cancelled
    // into the voice: same scene with +-30000 offsets per mic.
    {
        T2AUDIO_MIC_ANC_STATE a;
        long n;
        double ps = 0, pe = 0;

        T2AudioMicAncReset(&a);
        for (n = 0; n < 48000 * 3; n++) {
            int s = speech24(n, 0.001);
            int mic[3] = { s + 30000, s - 25000, s + 12000 };
            int out = T2AudioMicAncProcess(&a, mic, 1);

            if (n >= 48000 * 2) {
                double sd = (double)speech24(n - T2AUDIO_MIC_ANC_DELAY, 0.001);

                ps += sd * sd;
                pe += ((double)out - sd) * ((double)out - sd);
            }
        }
        CHECK(10 * log10(pe / ps) < -20.0, "DC offsets leak: residual %.1f dB", 10 * log10(pe / ps));
    }

    // Speaker noise alone, arriving off-axis (mic 0 first, mic 2 last).
    run_anc(0, 400000, 0, 2, 4, 0, &po, &pr, &gain, &pn, &resets);
    printf("  anc: off-axis noise alone: %.1f dB\n", 10 * log10(po / pn));
    CHECK(10 * log10(po / pn) < -6.0, "off-axis noise reduced only %.1f dB", 10 * log10(po / pn));
    CHECK(resets == 0, "canceller reset %u times on plain noise", resets);

    // The same at the real raw level: the microphones sit around -75 dBFS
    // before the +39 dB output gain (hardware, 2026-11-04). Must adapt too.
    run_anc(0, 1500, 0, 2, 4, 0, &po, &pr, &gain, &pn, &resets);
    printf("  anc: off-axis noise at -75 dBFS raw: %.1f dB\n", 10 * log10(po / pn));
    CHECK(10 * log10(po / pn) < -6.0, "quiet off-axis noise reduced only %.1f dB", 10 * log10(po / pn));
    // A tone (the speakers playing music) arriving off-axis, quiet.
    {
        T2AUDIO_MIC_ANC_STATE a;
        long n;
        double pin = 0, pout = 0;

        T2AudioMicAncReset(&a);
        for (n = 0; n < 48000 * 3; n++) {
            int mic[3], out;

            mic[0] = (int)lrint(1500.0 * sin(2 * M_PI * 1000.0 * (n - 0) / 48000.0));
            mic[1] = (int)lrint(1500.0 * sin(2 * M_PI * 1000.0 * (n - 2) / 48000.0));
            mic[2] = (int)lrint(1500.0 * sin(2 * M_PI * 1000.0 * (n - 4) / 48000.0));
            out = T2AudioMicAncProcess(&a, mic, 1);
            if (n >= 48000 * 2) {
                pin += (double)mic[1] * mic[1];
                pout += (double)out * out;
            }
        }
        printf("  anc: quiet 1 kHz speaker tone: %.1f dB, adapted %.0f%%\n", 10 * log10(pout / pin),
               100.0 * (double)a.AdaptSamples / (double)a.Samples);
        CHECK(10 * log10(pout / pin) < -10.0, "quiet speaker tone reduced only %.1f dB", 10 * log10(pout / pin));
    }

    // Talker + off-axis noise: the voice keeps its level, the noise drops.
    run_anc(0.05, 400000, 0, 2, 4, 0, &po, &pr, &gain, &pn, &resets);
    printf("  anc: talker + noise: speech gain %.2f dB, noise %.1f dB\n",
           20 * log10(gain), 10 * log10(pr / pn));
    CHECK(fabs(20 * log10(gain)) < 1.0, "speech gain %.2f dB", 20 * log10(gain));
    CHECK(10 * log10(pr / pn) < -3.0, "noise under speech reduced only %.1f dB", 10 * log10(pr / pn));

    // Uncorrelated noise at each mic (each capsule's own hiss): nothing in
    // the reference relates to the voice mic, so the output keeps its level
    // - neither cancelled into the voice nor boosted.
    run_anc(0, 400000, 0, 0, 0, 1, &po, &pr, &gain, &pn, &resets);
    printf("  anc: diffuse noise: %.1f dB, resets %u\n", 10 * log10(po / pn), resets);
    CHECK(fabs(10 * log10(po / pn)) < 1.5, "diffuse noise changed by %.1f dB", 10 * log10(po / pn));

    // Capsules with different sensitivity (hardware: mic1 4.6 dB below the
    // others): mic 0 at +4 dB, quiet talker in front plus off-axis noise.
    // The level match keeps the talker out of the reference: the voice
    // keeps its level and the noise still drops.
    {
        T2AUDIO_MIC_ANC_STATE a;
        int hist[ANC_HIST] = { 0 };
        long n, total = 48000 * 6;
        double ps = 0, cross = 0, pres = 0, pnv = 0;

        T2AudioMicAncReset(&a);
        g_lcg = 99u;
        for (n = 0; n < total; n++) {
            int s = speech24(n, 0.0005);
            long t = n % ANC_HIST;
            int mic[3], out;

            hist[t] = noise24(2000);
            mic[0] = (int)lrint(1.585 * (s + hist[t]));
            mic[1] = s + hist[(t - 2 + ANC_HIST) % ANC_HIST];
            mic[2] = s + hist[(t - 4 + ANC_HIST) % ANC_HIST];
            out = T2AudioMicAncProcess(&a, mic, 1);
            if (n >= total - 48000) {
                double sd = (double)speech24(n - T2AUDIO_MIC_ANC_DELAY, 0.0005);
                double nv = (double)hist[(t - 2 - T2AUDIO_MIC_ANC_DELAY + 2 * ANC_HIST) % ANC_HIST];

                ps += sd * sd;
                cross += (double)out * sd;
                pres += ((double)out - sd) * ((double)out - sd);
                pnv += nv * nv;
            }
        }
        printf("  anc: mismatched capsules: speech %.2f dB, noise %.1f dB, match gain %.3f\n",
               20 * log10(cross / ps), 10 * log10(pres / pnv), a.MatchGain / 65536.0);
        CHECK(fabs(20 * log10(cross / ps)) < 1.0, "mismatched: speech gain %.2f dB", 20 * log10(cross / ps));
        CHECK(10 * log10(pres / pnv) < -3.0, "mismatched: noise reduced only %.1f dB", 10 * log10(pres / pnv));
        // a = mic 2, b = mic 0 (the +4 dB one): G = 1 / 1.585 = 0.631.
        CHECK(a.MatchGain > 38000 && a.MatchGain < 45000, "match gain %u, expected ~0.631 (41350)", a.MatchGain);
    }

    // Full scale, out of phase: saturates, no overflow.
    {
        T2AUDIO_MIC_ANC_STATE a;
        long n;
        int mic[3], out = 0;

        T2AudioMicAncReset(&a);
        for (n = 0; n < 48000; n++) {
            mic[0] = (n & 1) ? 0x7FFFFF : -0x800000;
            mic[1] = (n & 1) ? -0x800000 : 0x7FFFFF;
            mic[2] = (n & 2) ? 0x7FFFFF : -0x800000;
            out = T2AudioMicAncProcess(&a, mic, 1);
            CHECK(out >= -(1 << 23) && out < (1 << 23), "full scale out of range: %d", out);
        }
        (void)out;
    }

    // Through the full chain: with the talker alone MicDsp=2 has the level
    // of MicDsp=1 (delayed by T2AUDIO_MIC_ANC_DELAY frames).
    {
        T2AUDIO_MIC_STATE s1, s2;
        long n;
        double e1 = 0, e2 = 0;

        T2AudioMicReset(&s1);
        T2AudioMicReset(&s2);
        for (n = 0; n < 48000; n++) {
            unsigned int f[3];
            int v = speech24(n, 0.001);
            int o1, o2;

            f[0] = f[1] = f[2] = raw24(v);
            o1 = T2AudioMicProcessFrameEx(&s1, f, 1, 1);
            o2 = T2AudioMicProcessFrameEx(&s2, f, 2, 1);
            if (n >= 24000) {
                e1 += (double)o1 * o1;
                e2 += (double)o2 * o2;
            }
        }
        CHECK(fabs(10 * log10(e2 / e1)) < 0.1, "MicDsp=2 level differs from MicDsp=1 by %.2f dB",
              10 * log10(e2 / e1));
    }
    // The voice microphone index wraps safely.
    {
        T2AUDIO_MIC_ANC_STATE a;
        int mic[3] = { 100, 200, 300 };

        T2AudioMicAncReset(&a);
        {
            int zero[3] = { 0, 0, 0 };   // primes the DC blocker at 0

            (void)T2AudioMicAncProcess(&a, zero, 7);
        }
        (void)T2AudioMicAncProcess(&a, mic, 7);
        CHECK(a.Ref[a.Pos] == 200, "bad voice index not treated as 1 (ref %d)", a.Ref[a.Pos]);
    }
}

// Headset microphone ("Codec Input"): signed 32-bit mono passes unchanged,
// one 32-bit slot per frame, and the ring wraps like the array's.
static void test_headset_mic(void)
{
    static const int samples[] = { 0, 1, -1, 0x12345678, -0x12345678, INT_MAX, INT_MIN };
    unsigned int ring[T2AUDIO_HEADSET_MIC_RING_FRAMES];
    T2AUDIO_MIC_GATE_STATE g;
    unsigned long slot;
    size_t i;

    CHECK(T2AUDIO_HEADSET_MIC_BYTES_PER_FRAME == 4, "4 bytes a frame");
    CHECK(T2AUDIO_HEADSET_MIC_RING_BYTES == T2AUDIO_HEADSET_MIC_RING_FRAMES * 4u, "ring bytes");
    CHECK(T2AUDIO_HEADSET_MIC_RING_BYTES < T2AUDIO_MIC_RING_BYTES, "smaller than the array ring");
    for (i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        CHECK(T2AudioHeadsetMicSample((unsigned int)samples[i]) == samples[i],
              "sample %d not passed through", samples[i]);
    }
    // Gate off is bit-exact on this path too.
    T2AudioMicGateReset(&g);
    for (i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        CHECK(T2AudioMicNoiseGate(&g, T2AudioHeadsetMicSample((unsigned int)samples[i]), 0) == samples[i],
              "gate bypass changed %d", samples[i]);
    }
    // Slots stay inside the ring across a wrap and for frames before START_IO.
    for (i = 0; i < T2AUDIO_HEADSET_MIC_RING_FRAMES; i++) {
        ring[i] = (unsigned int)i;
    }
    slot = T2AudioMicRingSlot((long long)T2AUDIO_HEADSET_MIC_RING_FRAMES * 3 + 7,
                              T2AUDIO_HEADSET_MIC_RING_FRAMES);
    CHECK(slot == 7 && ring[slot] == 7, "wrap slot %lu", slot);
    CHECK(T2AudioMicRingSlot(-5, T2AUDIO_HEADSET_MIC_RING_FRAMES) == 0, "negative frame -> slot 0");
    CHECK((T2AudioMicRingSlot(0x7FFFFFFFFFFFll, T2AUDIO_HEADSET_MIC_RING_FRAMES) + 1) *
              T2AUDIO_HEADSET_MIC_BYTES_PER_FRAME <= T2AUDIO_HEADSET_MIC_RING_BYTES,
          "last byte in ring");
}

// Client containers for capture (ExtraFormats): 16 / 24 / 32 bits.
static void test_store_sample(void)
{
    unsigned char b[4];

    CHECK(T2AudioMicStoreSample(b, 0x12345678, 32) == 4 &&
          b[0] == 0x78 && b[1] == 0x56 && b[2] == 0x34 && b[3] == 0x12, "32-bit LE");
    CHECK(T2AudioMicStoreSample(b, 0x12345678, 24) == 3 &&
          b[0] == 0x56 && b[1] == 0x34 && b[2] == 0x12, "24-bit packed");
    CHECK(T2AudioMicStoreSample(b, 0x12345678, 16) == 2 && b[0] == 0x34 && b[1] == 0x12, "16-bit");
    // Rounding: 0x00008000 rounds up to 1 LSB in 16 bits.
    CHECK(T2AudioMicStoreSample(b, 0x00008000, 16) == 2 && b[0] == 1 && b[1] == 0, "16-bit rounds");
    // Negative: -1 LSB of 16 bits.
    CHECK(T2AudioMicStoreSample(b, -65536, 16) == 2 && b[0] == 0xFF && b[1] == 0xFF, "16-bit negative");
    // Full scale saturates instead of wrapping when rounding up.
    CHECK(T2AudioMicStoreSample(b, INT_MAX, 16) == 2 && b[0] == 0xFF && b[1] == 0x7F, "16-bit saturates");
    CHECK(T2AudioMicStoreSample(b, INT_MAX, 24) == 3 && b[0] == 0xFF && b[1] == 0xFF && b[2] == 0x7F,
          "24-bit saturates");
    CHECK(T2AudioMicStoreSample(b, INT_MIN, 16) == 2 && b[0] == 0x00 && b[1] == 0x80, "16-bit min");
    CHECK(T2AudioMicStoreSample(b, 5, 8) == 0, "unknown size rejected");
}

// Jack reporting: "connected" until a read succeeded once, then the jack.
static void test_jack_report(void)
{
    CHECK(T2AudioJackReportConnected(0, 0, 0) == 1, "no jack events -> connected");
    CHECK(T2AudioJackReportConnected(0, 1, 0) == 1, "no jack events, seen -> connected");
    CHECK(T2AudioJackReportConnected(1, 0, 0) == 1, "never read -> connected");
    CHECK(T2AudioJackReportConnected(1, 1, 0) == 0, "read unplugged -> unplugged");
    CHECK(T2AudioJackReportConnected(1, 1, 1) == 1, "read plugged -> plugged");
    CHECK(T2AudioJackReportConnected(1, 1, 5) == 1, "non-zero -> plugged");

    // Auto-switch: the built-in device yields while the jack device is plugged.
    CHECK(T2AudioJackYieldConnected(1, 1, 1, 0) == 1, "nothing plugged -> built-in available");
    CHECK(T2AudioJackYieldConnected(1, 1, 1, 1) == 0, "plugged -> built-in yields");
    CHECK(T2AudioJackYieldConnected(0, 1, 1, 1) == 1, "auto-switch off -> stays available");
    CHECK(T2AudioJackYieldConnected(1, 0, 1, 1) == 1, "no jack events -> stays available");
    CHECK(T2AudioJackYieldConnected(1, 1, 0, 1) == 1, "jack never read -> stays available");
    // The two sides never both read unplugged once the jack is known.
    {
        long plugged;

        for (plugged = 0; plugged <= 1; plugged++) {
            CHECK(T2AudioJackReportConnected(1, 1, plugged) + T2AudioJackYieldConnected(1, 1, 1, plugged) == 1,
                  "exactly one side available (plugged=%ld)", plugged);
        }
    }
}

static void test_geometry(void)
{
    CHECK(T2AUDIO_MIC_ARRAY_COUNT == 3, "mic count");
    CHECK(T2AUDIO_MIC0_X_MM < T2AUDIO_MIC1_X_MM, "left of center");
    CHECK(T2AUDIO_MIC1_X_MM < T2AUDIO_MIC2_X_MM, "center of right");
    CHECK(T2AUDIO_MIC_ARRAY_FREQ_LO <= 50, "low frequency response");
    CHECK(T2AUDIO_MIC_ARRAY_FREQ_HI >= 20000, "high frequency response");
    CHECK(T2AUDIO_MIC_ARRAY_WORK_ANGLE > 0, "work angle positive");
}

static void test_noise_gate(void)
{
    T2AUDIO_MIC_GATE_STATE g;
    int samples[] = { 0, 1, -1, 12345, -12345, 0x7FFFFFFF, INT_MIN, 0x12345678, -0x76543210 };
    unsigned i;
    int out, prev;
    long n;

    // Disabled: bit-exact pass-through and the gate state is not touched.
    T2AudioMicGateReset(&g);
    for (i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        CHECK(T2AudioMicNoiseGate(&g, samples[i], 0) == samples[i], "bypass sample %d", samples[i]);
    }
    CHECK(g.Gain == 0 && g.Open == 0, "bypass left the gate state untouched");

    // Enabled, from reset (closed): silence stays silent.
    T2AudioMicGateReset(&g);
    for (n = 0; n < 1000; n++) {
        CHECK(T2AudioMicNoiseGate(&g, 0, 1) == 0, "silence while closed");
    }

    // A loud signal opens the gate over ~96 samples, then passes the input
    // through exactly (the gain saturates at Q16 1.0).
    {
        int loud = 0x40000000;   // -6 dBFS, far above the open threshold

        T2AudioMicGateReset(&g);
        for (n = 0; n < 200; n++) {
            out = T2AudioMicNoiseGate(&g, loud, 1);
            if (n < 95) {
                CHECK(out != loud, "attack ramp not yet open at sample %ld", n);
            } else {
                CHECK(out == loud, "gate did not reach full gain at sample %ld", n);
            }
        }
        CHECK(g.Gain == T2AUDIO_MIC_GATE_GAIN_FULL, "gain not saturated");
    }

    // Once open, a quiet signal ramps the gain down monotonically (envelope
    // fall + 100 ms hold + ~150 ms release) and then stays silent.
    {
        int quiet = 0x200000;    // well below the close threshold

        T2AudioMicGateReset(&g);
        for (n = 0; n < 96; n++) {
            (void)T2AudioMicNoiseGate(&g, 0x40000000, 1);
        }
        CHECK(g.Gain == T2AUDIO_MIC_GATE_GAIN_FULL, "pre-open");
        prev = 0x40000000;
        for (n = 0; n < 48000; n++) {
            out = T2AudioMicNoiseGate(&g, quiet, 1);
            CHECK(abs(out) <= abs(prev), "release not monotonic at sample %ld", n);
            prev = out;
        }
        CHECK(g.Gain == 0, "gate did not fully close");
        CHECK(T2AudioMicNoiseGate(&g, quiet, 1) == 0, "gate passed noise after closing");
    }

    // Hysteresis: a mid-level sample holds whichever state the gate is in.
    {
        int mid = (int)((T2AUDIO_MIC_GATE_OPEN_THRESHOLD + T2AUDIO_MIC_GATE_CLOSE_THRESHOLD) / 2u);

        T2AudioMicGateReset(&g);
        (void)T2AudioMicNoiseGate(&g, mid, 1);
        CHECK(g.Gain == 0, "mid-level opened a closed gate");
        for (n = 0; n < 96; n++) {
            (void)T2AudioMicNoiseGate(&g, 0x40000000, 1);
        }
        (void)T2AudioMicNoiseGate(&g, mid, 1);
        CHECK(g.Gain == T2AUDIO_MIC_GATE_GAIN_FULL, "mid-level closed an open gate");
    }
}

// Continuous tones at -30 dBFS (above OPEN) must keep the gate fully open
// through every zero crossing once the attack has finished.
static void test_noise_gate_tones(void)
{
    static const double freqs[] = { 50.0, 100.0, 440.0, 1000.0, 8000.0 };
    T2AUDIO_MIC_GATE_STATE g;
    unsigned i;
    long n;

    for (i = 0; i < sizeof(freqs) / sizeof(freqs[0]); i++) {
        double amp = 2147483647.0 * pow(10.0, -30.0 / 20.0);
        int bad = 0;

        T2AudioMicGateReset(&g);
        for (n = 0; n < 48000; n++) {
            int x = (int)(amp * sin(2 * M_PI * freqs[i] * n / 48000.0));
            int y = T2AudioMicNoiseGate(&g, x, 1);
            // The tone first reaches OPEN within a quarter period; allow that
            // plus the 2 ms attack, then the output must equal the input.
            if (n > (long)(48000.0 / freqs[i] / 4.0) + 200 && y != x) {
                bad++;
            }
        }
        CHECK(bad == 0, "%.0f Hz tone: %d gated samples after attack", freqs[i], bad);
    }
}

// Noise floor at -60 dBFS never opens the gate.
static void test_noise_gate_floor(void)
{
    T2AUDIO_MIC_GATE_STATE g;
    unsigned int seed = 12345;
    long n;
    int leaked = 0;
    int amp = (int)(2147483647.0 * pow(10.0, -60.0 / 20.0));

    T2AudioMicGateReset(&g);
    for (n = 0; n < 48000; n++) {
        int x;

        seed = seed * 1103515245u + 12345u;
        x = (int)((long long)amp * (long long)((int)(seed >> 16) % 2001 - 1000) / 1000);
        if (T2AudioMicNoiseGate(&g, x, 1) != 0) {
            leaked++;
        }
    }
    CHECK(leaked == 0, "noise floor leaked %d samples", leaked);
}

// A 50 ms pause between words is bridged by the hold timer (gain stays full);
// a long silence then closes the gate completely.
static void test_noise_gate_pause(void)
{
    T2AUDIO_MIC_GATE_STATE g;
    double amp = 2147483647.0 * pow(10.0, -25.0 / 20.0);
    long n;
    unsigned int minGain = T2AUDIO_MIC_GATE_GAIN_FULL;

    T2AudioMicGateReset(&g);
    for (n = 0; n < 9600; n++) {
        (void)T2AudioMicNoiseGate(&g, (int)(amp * sin(2 * M_PI * 300.0 * n / 48000.0)), 1);
    }
    CHECK(g.Gain == T2AUDIO_MIC_GATE_GAIN_FULL, "word did not open the gate");
    for (n = 0; n < 2400; n++) {
        (void)T2AudioMicNoiseGate(&g, 0, 1);
        if (g.Gain < minGain) minGain = g.Gain;
    }
    CHECK(minGain == T2AUDIO_MIC_GATE_GAIN_FULL, "50 ms pause dipped gain to %u", minGain);
    for (n = 0; n < 48000; n++) {
        (void)T2AudioMicNoiseGate(&g, 0, 1);
    }
    CHECK(g.Gain == 0 && g.Open == 0, "1 s silence did not close the gate");
}

int main(void)
{
    test_geometry();
    test_sample24();
    test_dsp();
    test_capture_aim();
    test_anc();
    test_store_sample();
    test_headset_mic();
    test_jack_report();
    test_noise_gate();
    test_noise_gate_tones();
    test_noise_gate_floor();
    test_noise_gate_pause();
    if (g_failures) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
