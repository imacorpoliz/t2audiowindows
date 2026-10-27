// Host-side test for the speaker layout, tweeter filter and T2 clock helpers in
// ../src/T2AudioSpeakerLogic.h (the exact code compiled into the driver).
//
//   cl /nologo /W4 /Fe:tests\T2AudioSpeakerLogicTest.exe tests\T2AudioSpeakerLogicTest.c
//   gcc -Wall -Wextra -Werror -o T2AudioSpeakerLogicTest tests/T2AudioSpeakerLogicTest.c -lm

#include <stdio.h>
#include <math.h>
#include "../src/T2AudioSpeakerLogic.h"

static int g_failures = 0;

static void check(const char *name, int condition)
{
    printf("  [%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) {
        g_failures++;
    }
}

static long long ring_sample(unsigned int word)
{
    // Ring words are S24_LE: 24 bits sign-extended into 32.
    return (word & 0x80000000u) ? (long long)word - 0x100000000ll : (long long)word;
}

static void test_layout(void)
{
    unsigned int src[4] = { 0x12345600u, 0xFEDCBA00u, 0x7FFFFF00u, 0x80000000u };
    unsigned int dst[12];
    T2AUDIO_SPEAKER_STATE state;

    printf("T2AudioRenderStereoToSpeakers layout\n");

    T2AudioSpeakerStateReset(&state);
    T2AudioRenderStereoToSpeakers(dst, src, 2, 8, T2AUDIO_TWEETER_MUTE, &state);
    check("L -> woofers 0,1", dst[0] == 0x00123456u && dst[1] == 0x00123456u);
    check("R -> woofers 3,4 (sign-extended)",
          dst[3] == 0xFFFEDCBAu && dst[4] == 0xFFFEDCBAu);
    check("muted tweeters 2,5", dst[2] == 0 && dst[5] == 0);
    check("frame 2 full scale", dst[6] == 0x007FFFFFu && dst[9] == 0xFF800000u);

    T2AudioRenderStereoToSpeakers(dst, src, 1, 0, T2AUDIO_TWEETER_MUTE, &state);
    check("shift 0 keeps the 32-bit sample", dst[0] == 0x12345600u);
}

// Steady-state gain of the tweeter path for a sine at Hz, in dB.
static double tweeter_gain_db(double hz)
{
    T2AUDIO_SPEAKER_STATE state;
    unsigned int src[2];
    unsigned int dst[6];
    double peakIn = 0x7FFFFF * 0.5;
    double peakOut = 0.0;
    int n;

    T2AudioSpeakerStateReset(&state);
    for (n = 0; n < 48000; n++) {
        double x = peakIn * sin(2.0 * 3.14159265358979323846 * hz * n / 48000.0);
        long long s = (long long)x;
        unsigned int w = (unsigned int)((s * 256) & 0xFFFFFFFFll);

        src[0] = w;
        src[1] = w;
        T2AudioRenderStereoToSpeakers(dst, src, 1, 8, T2AUDIO_TWEETER_HIGHPASS, &state);
        if (n > 24000) {
            double y = fabs((double)ring_sample(dst[T2AUDIO_SPK_LEFT_TWEETER]));
            if (y > peakOut) {
                peakOut = y;
            }
        }
    }
    if (peakOut < 1.0) {
        peakOut = 1.0;
    }
    return 20.0 * log10(peakOut / peakIn);
}

static void test_tweeter_filter(void)
{
    double g250 = tweeter_gain_db(250);
    double g1k = tweeter_gain_db(1000);
    double g3k = tweeter_gain_db(3000);
    double g10k = tweeter_gain_db(10000);

    printf("Tweeter high-pass (dB): 250 Hz %.1f, 1 kHz %.1f, 3 kHz %.1f, 10 kHz %.1f\n",
           g250, g1k, g3k, g10k);
    check("250 Hz below -70 dB", g250 < -70.0);
    check("1 kHz near -38 dB", g1k < -34.0 && g1k > -42.0);
    check("3 kHz near -3 dB", g3k < -2.0 && g3k > -4.0);
    check("10 kHz passes (> -0.5 dB)", g10k > -0.5 && g10k < 0.5);
}

static void test_clipping(void)
{
    T2AUDIO_SPEAKER_STATE state;
    unsigned int src[2];
    unsigned int dst[6];
    int n;
    int ok = 1;

    printf("Tweeter path stays in S24 range on full-scale square wave\n");
    T2AudioSpeakerStateReset(&state);
    for (n = 0; n < 4800; n++) {
        unsigned int w = ((n / 3) & 1) ? 0x7FFFFF00u : 0x80000000u;
        long long y;

        src[0] = w;
        src[1] = w;
        T2AudioRenderStereoToSpeakers(dst, src, 1, 8, T2AUDIO_TWEETER_HIGHPASS, &state);
        y = ring_sample(dst[T2AUDIO_SPK_RIGHT_TWEETER]);
        if (y > 0x7FFFFF || y < -0x800000) {
            ok = 0;
        }
    }
    check("no overflow", ok);
}

static void test_clock(void)
{
    unsigned long long off = 0;
    long long jump = 0;
    const unsigned long ring = 16640;

    printf("T2 clock helpers\n");

    check("QpcToNs 10 MHz", T2AudioQpcToNs(15000000ull, 10000000ull) == 1500000000ull);
    check("QpcToNs no overflow at 30 days",
          T2AudioQpcToNs(10000000ull * 86400ull * 30ull, 10000000ull) ==
              1000000000ull * 86400ull * 30ull);
    check("reception: 2 ms delivery adds 96 frames",
          T2AudioClockFramesAtReception(1000, 5000000, 7000000, 48000) == 1096);
    check("reception: implausible delta ignored",
          T2AudioClockFramesAtReception(1000, 5000000, 5000000 + 60000000ull, 48000) == 1000);
    check("reception: hostTime in the future ignored",
          T2AudioClockFramesAtReception(1000, 9000000, 5000000, 48000) == 1000);

    check("plausible: 2 ms old", T2AudioClockHostTimePlausible(5000000, 7000000));
    check("implausible: future", !T2AudioClockHostTimePlausible(9000000, 7000000));
    check("anchor ns: plausible uses hostTime", T2AudioClockAnchorNs(5000000, 7000000) == 5000000);
    check("anchor ns: implausible uses reception",
          T2AudioClockAnchorNs(1000, 1000 + 60000000ull) == 1000 + 60000000ull);

    check("distance simple", T2AudioRingDistance(1500, 500, ring) == 1000);
    check("distance across wrap", T2AudioRingDistance(ring + 200, ring - 800, ring) == 1000);
    check("distance negative", T2AudioRingDistance(100, 600, ring) == -500);

    check("lead 1000: no correction",
          !T2AudioClockCorrection(500, 1000, 500, ring, &off, &jump));
    check("lead 1400: within tolerance",
          !T2AudioClockCorrection(500, 1400, 500, ring, &off, &jump));
    check("lead 1600: pulled back by 600",
          T2AudioClockCorrection(500, 1600, 500, ring, &off, &jump) &&
              off == 1000 && jump == -600);
    check("lead 300: pushed ahead by 700",
          T2AudioClockCorrection(10000, 1000, 10700, ring, &off, &jump) &&
              off == 1700 && jump == 700);
    check("behind the reader: re-aimed to 1000 ahead",
          T2AudioClockCorrection(0, 0, 2000, ring, &off, &jump) &&
              T2AudioRingDistance(0 + off, 2000, ring) == 1000);
    check("offset normalized into the ring",
          T2AudioClockCorrection(0, 5, 1000, ring, &off, &jump) &&
              off < ring && T2AudioRingDistance(off, 1000, ring) == 1000);
}

int main(void)
{
    test_layout();
    test_tweeter_filter();
    test_clipping();
    test_clock();
    if (g_failures != 0) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
