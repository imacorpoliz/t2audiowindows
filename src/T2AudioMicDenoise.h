// Spectral noise suppressor for the microphone outputs
// (Parameters\MicNoiseSuppression=2). Integer-only and free of kernel APIs,
// so tests/T2AudioMicTest.c runs the same code on the host.
//
// Short-time Fourier processing, 256-point frames, 50 % overlap (hop 128 =
// 2.7 ms, 187.5 Hz bins), sqrt-Hann analysis and synthesis windows (their
// product sums to 1, so a gain of 1 everywhere reconstructs the input
// exactly, delayed by 256 samples = 5.3 ms). Per bin:
//
//   Ps    = smoothed power (fast, ~11 ms)
//   N     = noise estimate: follows Ps down at once, rises only ~1.6 dB/s,
//           so it settles on the noise floor between syllables and ignores
//           speech (minimum tracking)
//   G     = max(Gmin, 1 - OVER * N / Ps)   (spectral subtraction, OVER = 3
//           makes up for the minimum's low bias)
//   Gs    = G smoothed over time (rises fast, falls slowly) against
//           "musical noise"
//
// Gmin = -20 dB: noise is pushed down by up to 20 dB, never to silence, so
// the background stays natural. Everything is 64-bit fixed point; one hop
// costs two 256-point FFTs (~2 x 1000 complex multiplies) - fine at
// DISPATCH_LEVEL. The 7.7 KB state lives in the stream object, not on the
// stack.

#ifndef T2AUDIO_MIC_DENOISE_H
#define T2AUDIO_MIC_DENOISE_H

#define T2AUDIO_DN_N            256
#define T2AUDIO_DN_HOP          128
#define T2AUDIO_DN_BINS         (T2AUDIO_DN_N / 2 + 1)
#define T2AUDIO_DN_Q            20                  // window / twiddle fraction bits
#define T2AUDIO_DN_GAIN_ONE     32768               // gains in Q15
#define T2AUDIO_DN_GAIN_MIN     3277                // -20 dB
#define T2AUDIO_DN_OVER         3                   // over-subtraction of the noise estimate
#define T2AUDIO_DN_RISE_SHIFT   10                  // noise rise: x(1 + 1/1024) a hop, ~1.6 dB/s
#define T2AUDIO_DN_POWER_SHIFT  2                   // power smoothing: 1/4 a hop
#define T2AUDIO_DN_SLOW_SHIFT   4                   // noise tracker input: 1/16 a hop (~45 ms)
#define T2AUDIO_DN_WARMUP_HOPS  40                  // ~0.1 s: estimate before attenuating

// sqrt-Hann window sin(pi n / 256), Q20 (generated).
static const int g_T2AudioDnWindow[256] = {
    0, 12868, 25733, 38595, 51451, 64299, 77138, 89965,
    102778, 115576, 128357, 141118, 153858, 166575, 179267, 191931,
    204567, 217172, 229744, 242282, 254783, 267246, 279669, 292049,
    304386, 316676, 328919, 341113, 353255, 365343, 377377, 389354,
    401273, 413131, 424926, 436658, 448324, 459922, 471452, 482910,
    494295, 505606, 516841, 527998, 539076, 550072, 560986, 571815,
    582558, 593213, 603779, 614254, 624636, 634924, 645117, 655213,
    665210, 675106, 684901, 694593, 704181, 713662, 723036, 732301,
    741455, 750498, 759428, 768244, 776944, 785526, 793991, 802336,
    810560, 818662, 826641, 834495, 842224, 849826, 857300, 864645,
    871859, 878942, 885893, 892711, 899394, 905941, 912352, 918626,
    924761, 930758, 936614, 942328, 947901, 953332, 958618, 963761,
    968758, 973609, 978314, 982871, 987281, 991541, 995652, 999614,
    1003425, 1007084, 1010592, 1013948, 1017151, 1020201, 1023098, 1025840,
    1028428, 1030861, 1033138, 1035261, 1037227, 1039037, 1040690, 1042187,
    1043527, 1044709, 1045735, 1046603, 1047313, 1047865, 1048260, 1048497,
    1048576, 1048497, 1048260, 1047865, 1047313, 1046603, 1045735, 1044709,
    1043527, 1042187, 1040690, 1039037, 1037227, 1035261, 1033138, 1030861,
    1028428, 1025840, 1023098, 1020201, 1017151, 1013948, 1010592, 1007084,
    1003425, 999614, 995652, 991541, 987281, 982871, 978314, 973609,
    968758, 963761, 958618, 953332, 947901, 942328, 936614, 930758,
    924761, 918626, 912352, 905941, 899394, 892711, 885893, 878942,
    871859, 864645, 857300, 849826, 842224, 834495, 826641, 818662,
    810560, 802336, 793991, 785526, 776944, 768244, 759428, 750498,
    741455, 732301, 723036, 713662, 704181, 694593, 684901, 675106,
    665210, 655213, 645117, 634924, 624636, 614254, 603779, 593213,
    582558, 571815, 560986, 550072, 539076, 527998, 516841, 505606,
    494295, 482910, 471452, 459922, 448324, 436658, 424926, 413131,
    401273, 389354, 377377, 365343, 353255, 341113, 328919, 316676,
    304386, 292049, 279669, 267246, 254783, 242282, 229744, 217172,
    204567, 191931, 179267, 166575, 153858, 141118, 128357, 115576,
    102778, 89965, 77138, 64299, 51451, 38595, 25733, 12868,
};
// cos / sin(2 pi k / 256), Q20 (generated).
static const int g_T2AudioDnCos[128] = {
    1048576, 1048260, 1047313, 1045735, 1043527, 1040690, 1037227, 1033138,
    1028428, 1023098, 1017151, 1010592, 1003425, 995652, 987281, 978314,
    968758, 958618, 947901, 936614, 924761, 912352, 899394, 885893,
    871859, 857300, 842224, 826641, 810560, 793991, 776944, 759428,
    741455, 723036, 704181, 684901, 665210, 645117, 624636, 603779,
    582558, 560986, 539076, 516841, 494295, 471452, 448324, 424926,
    401273, 377377, 353255, 328919, 304386, 279669, 254783, 229744,
    204567, 179267, 153858, 128357, 102778, 77138, 51451, 25733,
    0, -25733, -51451, -77138, -102778, -128357, -153858, -179267,
    -204567, -229744, -254783, -279669, -304386, -328919, -353255, -377377,
    -401273, -424926, -448324, -471452, -494295, -516841, -539076, -560986,
    -582558, -603779, -624636, -645117, -665210, -684901, -704181, -723036,
    -741455, -759428, -776944, -793991, -810560, -826641, -842224, -857300,
    -871859, -885893, -899394, -912352, -924761, -936614, -947901, -958618,
    -968758, -978314, -987281, -995652, -1003425, -1010592, -1017151, -1023098,
    -1028428, -1033138, -1037227, -1040690, -1043527, -1045735, -1047313, -1048260,
};
static const int g_T2AudioDnSin[128] = {
    0, 25733, 51451, 77138, 102778, 128357, 153858, 179267,
    204567, 229744, 254783, 279669, 304386, 328919, 353255, 377377,
    401273, 424926, 448324, 471452, 494295, 516841, 539076, 560986,
    582558, 603779, 624636, 645117, 665210, 684901, 704181, 723036,
    741455, 759428, 776944, 793991, 810560, 826641, 842224, 857300,
    871859, 885893, 899394, 912352, 924761, 936614, 947901, 958618,
    968758, 978314, 987281, 995652, 1003425, 1010592, 1017151, 1023098,
    1028428, 1033138, 1037227, 1040690, 1043527, 1045735, 1047313, 1048260,
    1048576, 1048260, 1047313, 1045735, 1043527, 1040690, 1037227, 1033138,
    1028428, 1023098, 1017151, 1010592, 1003425, 995652, 987281, 978314,
    968758, 958618, 947901, 936614, 924761, 912352, 899394, 885893,
    871859, 857300, 842224, 826641, 810560, 793991, 776944, 759428,
    741455, 723036, 704181, 684901, 665210, 645117, 624636, 603779,
    582558, 560986, 539076, 516841, 494295, 471452, 448324, 424926,
    401273, 377377, 353255, 328919, 304386, 279669, 254783, 229744,
    204567, 179267, 153858, 128357, 102778, 77138, 51451, 25733,
};

typedef struct _T2AUDIO_MIC_DENOISE_STATE {
    int In[T2AUDIO_DN_N];                   // last N input samples (24-bit)
    int Out[T2AUDIO_DN_HOP];                // finished output of the current hop
    long long Overlap[T2AUDIO_DN_HOP];      // second half of the previous frame
    long long Re[T2AUDIO_DN_N];             // FFT work buffers
    long long Im[T2AUDIO_DN_N];
    long long Ps[T2AUDIO_DN_BINS];          // smoothed power (fast: gain)
    long long PsSlow[T2AUDIO_DN_BINS];      // smoothed power (slow: noise tracking)
    long long Noise[T2AUDIO_DN_BINS];       // noise estimate
    int Gs[T2AUDIO_DN_BINS];                // smoothed gain, Q15
    unsigned int Pos;                       // samples collected in this hop
    unsigned int Hops;
    int FixedGain;                          // tests: 1 = gain 1 everywhere
} T2AUDIO_MIC_DENOISE_STATE;

static __inline void
T2AudioMicDenoiseReset(T2AUDIO_MIC_DENOISE_STATE *D)
{
    unsigned int k;

    for (k = 0; k < T2AUDIO_DN_N; k++) {
        D->In[k] = 0;
        D->Re[k] = D->Im[k] = 0;
    }
    for (k = 0; k < T2AUDIO_DN_HOP; k++) {
        D->Out[k] = 0;
        D->Overlap[k] = 0;
    }
    for (k = 0; k < T2AUDIO_DN_BINS; k++) {
        D->Ps[k] = 0;
        D->PsSlow[k] = 0;
        D->Noise[k] = 0;
        D->Gs[k] = T2AUDIO_DN_GAIN_ONE;
    }
    D->Pos = 0;
    D->Hops = 0;
    D->FixedGain = 0;
}

static __inline long long
T2AudioDnSar(long long v, unsigned int s)
{
    return v >= 0 ? (v >> s) : -((-v - 1) >> s) - 1;
}

static __inline unsigned int
T2AudioDnBitReverse8(unsigned int v)
{
    v = ((v & 0xF0u) >> 4) | ((v & 0x0Fu) << 4);
    v = ((v & 0xCCu) >> 2) | ((v & 0x33u) << 2);
    v = ((v & 0xAAu) >> 1) | ((v & 0x55u) << 1);
    return v & 0xFFu;
}

// In-place radix-2 FFT of Re/Im (forward, e^{-j}); Inverse uses e^{+j} and
// is not scaled (the caller divides by N).
static __inline void
T2AudioDnFft(long long *Re, long long *Im, int Inverse)
{
    unsigned int i, j, len, half, step, k;

    for (i = 0; i < T2AUDIO_DN_N; i++) {
        j = T2AudioDnBitReverse8(i);
        if (j > i) {
            long long t = Re[i]; Re[i] = Re[j]; Re[j] = t;
            t = Im[i]; Im[i] = Im[j]; Im[j] = t;
        }
    }
    for (len = 2; len <= T2AUDIO_DN_N; len <<= 1) {
        half = len >> 1;
        step = T2AUDIO_DN_N / len;
        for (i = 0; i < T2AUDIO_DN_N; i += len) {
            for (k = 0; k < half; k++) {
                long long wr = g_T2AudioDnCos[k * step];
                long long wi = Inverse ? g_T2AudioDnSin[k * step] : -g_T2AudioDnSin[k * step];
                unsigned int a = i + k, b = i + k + half;
                long long tr = T2AudioDnSar(Re[b] * wr - Im[b] * wi, T2AUDIO_DN_Q);
                long long ti = T2AudioDnSar(Re[b] * wi + Im[b] * wr, T2AUDIO_DN_Q);

                Re[b] = Re[a] - tr;
                Im[b] = Im[a] - ti;
                Re[a] += tr;
                Im[a] += ti;
            }
        }
    }
}

// One hop: window the last N inputs, attenuate each bin, overlap-add into
// Out[] (the next HOP output samples).
static __inline void
T2AudioDnProcessHop(T2AUDIO_MIC_DENOISE_STATE *D)
{
    unsigned int n, k;

    for (n = 0; n < T2AUDIO_DN_N; n++) {
        D->Re[n] = T2AudioDnSar((long long)D->In[n] * g_T2AudioDnWindow[n], T2AUDIO_DN_Q);
        D->Im[n] = 0;
    }
    T2AudioDnFft(D->Re, D->Im, 0);

    D->Hops++;
    for (k = 0; k < T2AUDIO_DN_BINS; k++) {
        // Bins reach 2^31 at full scale; >> 8 keeps the power below 2^48.
        long long re = T2AudioDnSar(D->Re[k], 8), im = T2AudioDnSar(D->Im[k], 8);
        long long p = re * re + im * im;
        long long g;

        if (D->Hops == 1) {
            D->Ps[k] = p;
            D->PsSlow[k] = p;
            D->Noise[k] = p;
        } else {
            D->Ps[k] += T2AudioDnSar(p - D->Ps[k], T2AUDIO_DN_POWER_SHIFT);
            D->PsSlow[k] += T2AudioDnSar(p - D->PsSlow[k], T2AUDIO_DN_SLOW_SHIFT);
        }
        // Minimum tracking on the slow power: the fast one dips far below
        // the mean noise power between its random peaks.
        if (D->PsSlow[k] < D->Noise[k]) {
            D->Noise[k] = D->PsSlow[k];
        } else {
            D->Noise[k] += (D->Noise[k] >> T2AUDIO_DN_RISE_SHIFT) + 1;
        }
        if (D->FixedGain || D->Hops <= T2AUDIO_DN_WARMUP_HOPS) {
            g = T2AUDIO_DN_GAIN_ONE;
        } else if (D->Ps[k] <= 0) {
            g = T2AUDIO_DN_GAIN_MIN;
        } else {
            long long noise = D->Noise[k] * T2AUDIO_DN_OVER;
            long long num = D->Ps[k] - noise;

            // g = 1 - OVER*N/Ps in Q15 (both below 2^49: scale before the shift).
            if (num <= 0) {
                g = 0;
            } else {
                long long ps = D->Ps[k];

                while (ps > (1ll << 46)) {
                    ps >>= 1;
                    num >>= 1;
                }
                g = (num << 15) / (ps > 0 ? ps : 1);
            }
            if (g < T2AUDIO_DN_GAIN_MIN) {
                g = T2AUDIO_DN_GAIN_MIN;
            } else if (g > T2AUDIO_DN_GAIN_ONE) {
                g = T2AUDIO_DN_GAIN_ONE;
            }
        }
        // Rise fast (speech onsets), fall slowly (no fluttering residue).
        if (g > D->Gs[k]) {
            D->Gs[k] += (int)T2AudioDnSar(g - D->Gs[k], 1);
        } else {
            D->Gs[k] += (int)T2AudioDnSar(g - D->Gs[k], 3);
        }
        if (D->FixedGain) {
            D->Gs[k] = T2AUDIO_DN_GAIN_ONE;
        }
        D->Re[k] = T2AudioDnSar(D->Re[k] * D->Gs[k], 15);
        D->Im[k] = T2AudioDnSar(D->Im[k] * D->Gs[k], 15);
        if (k != 0 && k != T2AUDIO_DN_N / 2) {
            D->Re[T2AUDIO_DN_N - k] = T2AudioDnSar(D->Re[T2AUDIO_DN_N - k] * D->Gs[k], 15);
            D->Im[T2AUDIO_DN_N - k] = T2AudioDnSar(D->Im[T2AUDIO_DN_N - k] * D->Gs[k], 15);
        }
    }

    T2AudioDnFft(D->Re, D->Im, 1);
    for (n = 0; n < T2AUDIO_DN_N; n++) {
        long long y = T2AudioDnSar(T2AudioDnSar(D->Re[n], 8) * g_T2AudioDnWindow[n], T2AUDIO_DN_Q);

        if (n < T2AUDIO_DN_HOP) {
            long long v = D->Overlap[n] + y;

            if (v > 0x7FFFFF) {
                v = 0x7FFFFF;
            } else if (v < -0x800000) {
                v = -0x800000;
            }
            D->Out[n] = (int)v;
        } else {
            D->Overlap[n - T2AUDIO_DN_HOP] = y;
        }
    }
    // Slide the input by one hop.
    for (n = 0; n < T2AUDIO_DN_HOP; n++) {
        D->In[n] = D->In[n + T2AUDIO_DN_HOP];
    }
}

// One sample through the suppressor: Sample is a 32-bit MSB-aligned PCM
// sample, the result is the same format, T2AUDIO_DN_N samples later.
static __inline int
T2AudioMicDenoise(T2AUDIO_MIC_DENOISE_STATE *D, int Sample)
{
    int out;

    if (D == 0) {
        return Sample;
    }
    D->In[T2AUDIO_DN_HOP + D->Pos] = (int)T2AudioDnSar((long long)Sample, 8);
    out = D->Out[D->Pos];
    if (++D->Pos >= T2AUDIO_DN_HOP) {
        D->Pos = 0;
        T2AudioDnProcessHop(D);
    }
    return (int)((unsigned int)out << 8);
}

#endif
