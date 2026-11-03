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
