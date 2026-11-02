#ifndef T2AUDIO_HEADPHONE_DSP_H
#define T2AUDIO_HEADPHONE_DSP_H

// Pure, dependency-free headphone DSP (ROADMAP section 9.4):
// Bauer Stereophonic-to-Binaural (bs2b) crossfeed algorithm to reduce listener
// fatigue from extreme channel separation in headphones.
//
// Crossfeed feeds a low-passed and delayed cross-channel signal:
//   L_out = g_direct * L[n] + g_cross * R_filt[n - delay]
//   R_out = g_direct * R[n] + g_cross * L_filt[n - delay]
//
// Constants at 48 kHz:
//   Cutoff: ~700 Hz (1st order IIR, alpha = 0.0875)
//   Delay: 13 samples (~270 microseconds, typical human head ITD)
//   g_direct: ~ -2.85 dB (0.72)
//   g_cross:  ~ -11.0 dB (0.28)
//   g_direct + g_cross <= 1.20; normalized with 6 dB headroom.

#define T2AUDIO_HP_CROSSFEED_DELAY 13u
#define T2AUDIO_HP_DELAY_BUF_SIZE  16u
#define T2AUDIO_HP_DELAY_MASK      (T2AUDIO_HP_DELAY_BUF_SIZE - 1u)

// IIR alpha in Q16: 0.0875 * 65536 = 5734
#define T2AUDIO_HP_IIR_ALPHA_Q16   5734
// Direct gain in Q16: 0.72 * 65536 = 47186
#define T2AUDIO_HP_DIRECT_GAIN_Q16 47186
// Cross gain in Q16: 0.28 * 65536 = 18350
#define T2AUDIO_HP_CROSS_GAIN_Q16  18350

typedef struct _T2AUDIO_HEADPHONE_DSP_STATE {
    int LeftDelay[T2AUDIO_HP_DELAY_BUF_SIZE];
    int RightDelay[T2AUDIO_HP_DELAY_BUF_SIZE];
    unsigned int DelayIndex;
    long long LeftIirState;   // 64-bit Q16 accumulator (prevents 24-bit audio overflow)
    long long RightIirState;  // 64-bit Q16 accumulator
} T2AUDIO_HEADPHONE_DSP_STATE;

static __inline void
T2AudioHeadphoneDspReset(T2AUDIO_HEADPHONE_DSP_STATE *State)
{
    unsigned int i;
    if (State == (void *)0) return;
    for (i = 0; i < T2AUDIO_HP_DELAY_BUF_SIZE; i++) {
        State->LeftDelay[i] = 0;
        State->RightDelay[i] = 0;
    }
    State->DelayIndex = 0;
    State->LeftIirState = 0;
    State->RightIirState = 0;
}

// Process one stereo frame (L, R) of 24-bit PCM samples with crossfeed.
// Input: inLeft, inRight (signed 24-bit integer, [-8388608, 8388607]).
// Output: *outLeft, *outRight (signed 24-bit integer).
static __inline void
T2AudioHeadphoneProcessStereo(
    T2AUDIO_HEADPHONE_DSP_STATE *State,
    int InLeft,
    int InRight,
    int EnableCrossfeed,
    int *OutLeft,
    int *OutRight)
{
    unsigned int writeIdx;
    unsigned int readIdx;
    int delayedL;
    int delayedR;
    long long filtL;
    long long filtR;
    long long mixedL;
    long long mixedR;

    if (!EnableCrossfeed || State == (void *)0) {
        *OutLeft = InLeft;
        *OutRight = InRight;
        return;
    }

    writeIdx = State->DelayIndex;
    readIdx = (writeIdx - T2AUDIO_HP_CROSSFEED_DELAY) & T2AUDIO_HP_DELAY_MASK;

    State->LeftDelay[writeIdx] = InLeft;
    State->RightDelay[writeIdx] = InRight;
    State->DelayIndex = (writeIdx + 1u) & T2AUDIO_HP_DELAY_MASK;

    delayedL = State->LeftDelay[readIdx];
    delayedR = State->RightDelay[readIdx];

    // 1-pole lowpass filter in Q16: y[n] = y[n-1] + alpha * (x[n] - y[n-1])
    State->LeftIirState += (long long)((((long long)delayedL << 16) - State->LeftIirState) * T2AUDIO_HP_IIR_ALPHA_Q16 >> 16);
    State->RightIirState += (long long)((((long long)delayedR << 16) - State->RightIirState) * T2AUDIO_HP_IIR_ALPHA_Q16 >> 16);

    filtL = State->LeftIirState >> 16;
    filtR = State->RightIirState >> 16;

    // Cross mix: L = direct * L + cross * R_filtered
    mixedL = ((long long)InLeft * T2AUDIO_HP_DIRECT_GAIN_Q16 + filtR * T2AUDIO_HP_CROSS_GAIN_Q16) >> 16;
    mixedR = ((long long)InRight * T2AUDIO_HP_DIRECT_GAIN_Q16 + filtL * T2AUDIO_HP_CROSS_GAIN_Q16) >> 16;

    // Saturate to 24-bit range
    if (mixedL > 8388607) mixedL = 8388607;
    else if (mixedL < -8388608) mixedL = -8388608;

    if (mixedR > 8388607) mixedR = 8388607;
    else if (mixedR < -8388608) mixedR = -8388608;

    *OutLeft = (int)mixedL;
    *OutRight = (int)mixedR;
}

#endif // T2AUDIO_HEADPHONE_DSP_H
