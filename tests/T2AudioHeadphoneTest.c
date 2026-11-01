#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../src/T2AudioHeadphoneDsp.h"

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("[FAIL] %s\n", msg); \
        g_failures++; \
    } else { \
        printf("  [PASS] %s\n", msg); \
    } \
} while (0)

int main(void)
{
    T2AUDIO_HEADPHONE_DSP_STATE state;
    int outL = 0, outR = 0;
    int i;

    printf("Headphone Crossfeed DSP tests\n");

    // 1. Test disabled passthrough
    T2AudioHeadphoneDspReset(&state);
    T2AudioHeadphoneProcessStereo(&state, 123456, -654321, 0, &outL, &outR);
    CHECK(outL == 123456 && outR == -654321, "Crossfeed disabled gives exact bit-for-bit passthrough");

    // 2. Test crossfeed active: Left channel pulse produces delayed low-pass bleed into Right
    T2AudioHeadphoneDspReset(&state);
    for (i = 0; i < 50; i++) {
        int inL = (i == 0) ? 1000000 : 0;
        int inR = 0;
        T2AudioHeadphoneProcessStereo(&state, inL, inR, 1, &outL, &outR);
        if (i == (int)T2AUDIO_HP_CROSSFEED_DELAY + 1) {
            CHECK(outR > 0, "Right channel receives crossfeed signal after delay");
        }
    }

    // 3. Test mono input: Left == Right gives balanced output
    T2AudioHeadphoneDspReset(&state);
    for (i = 0; i < 200; i++) {
        T2AudioHeadphoneProcessStereo(&state, 500000, 500000, 1, &outL, &outR);
    }
    CHECK(abs(outL - outR) < 10, "In-phase mono input maintains perfect left/right balance");

    // 4. Test saturation safety on full-scale input
    T2AudioHeadphoneDspReset(&state);
    for (i = 0; i < 100; i++) {
        T2AudioHeadphoneProcessStereo(&state, 8388607, 8388607, 1, &outL, &outR);
        CHECK(outL <= 8388607 && outL >= -8388608, "Left does not overflow 24-bit range");
        CHECK(outR <= 8388607 && outR >= -8388608, "Right does not overflow 24-bit range");
    }

    if (g_failures == 0) {
        printf("ALL HEADPHONE TESTS PASSED\n");
        return 0;
    } else {
        printf("%d FAILURES\n", g_failures);
        return 1;
    }
}
