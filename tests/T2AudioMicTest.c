// Host tests for src/T2AudioMicLogic.h (microphone format, DSP, capture clock).
#include <stdio.h>
#include <math.h>
#include <string.h>
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

int main(void)
{
    test_sample24();
    test_dsp();
    test_capture_aim();
    if (g_failures) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
