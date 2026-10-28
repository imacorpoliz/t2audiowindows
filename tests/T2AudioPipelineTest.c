// Host-side simulation of the whole speaker path: client ring -> copy DPC
// (every 2 ms) -> BAR0 speaker ring -> T2 reader on its own clock, with
// UPDATE_TIMESTAMP anchors once per ring lap. Uses the exact helpers compiled
// into the driver (T2AudioBufferLogic.h, T2AudioSpeakerLogic.h); the DPC
// steps mirror T2AudioStreamAdvanceLocked / T2AudioStreamFollowT2ClockLocked
// in src/WaveRTStream.c and must be kept in sync with them.
//
// Every client frame carries its own index as a marker, so the T2 side can
// count discontinuities (a jump in the marker, or silence after audio).
//
//   cl /nologo /W4 /Fe:tests\T2AudioPipelineTest.exe tests\T2AudioPipelineTest.c
//   gcc -Wall -Wextra -Werror -o T2AudioPipelineTest tests/T2AudioPipelineTest.c -lm

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/T2AudioBufferLogic.h"
#include "../src/T2AudioSpeakerLogic.h"

#define RATE          48000u
#define QPC_FREQ      10000000ull               // 10 MHz, 100 ns ticks
#define HW_FRAMES     16640u                    // MacBookPro16,1 speaker ring
#define SRC_FRAMES    1440u                     // 30 ms client buffer
#define SRC_BPF       8u                        // stereo, 2 x 32-bit
#define TICK_NS       2000000ull                // copy DPC period
#define T2_START_NS   40000000ull               // T2 starts ~40 ms after START_IO
#define DELIVERY_NS   2000000ull                // timestamp delivery delay
#define MARKER_MASK   0x3FFFFFu
#define UPTIME_NS     179280000000000ull        // ~49.8 h, as in the 2026-10-28 Stage E run

// Scenario knobs for the hostTime path (see simulate()).
#define HOST_TRUE     0   // hostTime correct (uptime < 25.6 h)
#define HOST_GARBAGE  1   // hostTime unusable (far off)
#define HOST_WRAPPED  2   // AppleUSBVHCI's qpc*1e7 wrap at ~49.8 h uptime
static unsigned long long g_firstLateNs = 0;   // extra delay of the first timestamp
static int g_unwrap = 1;                       // driver unwraps hostTime (BceTransport.c)

static int g_failures = 0;

static void check(const char *name, int condition)
{
    printf("  [%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) {
        g_failures++;
    }
}

typedef struct _SIM_RESULT {
    unsigned int ReAims;
    unsigned int Discontinuities;   // after the first audio frame
    unsigned int OutOfBounds;
    long long FinalPpb;
    long long MinLead, MaxLead;     // lead seen at anchors after settling
} SIM_RESULT;

static unsigned int marker(unsigned long long frame)
{
    return (unsigned int)((frame & MARKER_MASK) + 1u);
}

// T2 read position (frames since its start) at host time ns.
static unsigned long long t2_read(unsigned long long ns, long long t2Ppb)
{
    unsigned long long dt;

    if (ns <= T2_START_NS) {
        return 0;
    }
    dt = ns - T2_START_NS;
    return T2AudioTicksToFramesPpb(dt, RATE, 1000000000ull, t2Ppb);
}

// Host time at which the T2 reaches read frame f (inverse of t2_read).
static unsigned long long t2_time_of(unsigned long long f, long long t2Ppb)
{
    double ns = (double)f * 1e9 / ((double)RATE * (1.0 + (double)t2Ppb / 1e9));
    return T2_START_NS + (unsigned long long)ns + 1u;
}

static SIM_RESULT simulate(long long t2Ppb, unsigned long long seconds,
                           int rateTracking, int hostMode)
{
    static unsigned int ring[HW_FRAMES * 6u];
    static unsigned int src[SRC_FRAMES * 2u];
    T2AUDIO_SPEAKER_STATE spk;
    SIM_RESULT r;
    unsigned long long now;
    unsigned long long endNs = seconds * 1000000000ull;
    // Stream state (instance->...)
    unsigned long long anchorQpc = 0, anchorFrames = 0, lastCopied = 0;
    unsigned long long destOffset = T2AUDIO_CLOCK_TARGET_LEAD;
    unsigned long long clientWritten = 0;
    unsigned int genSeen = 0;
    long long ppb = 0;
    int driftBaseValid = 0;
    unsigned long long driftBaseFrames = 0, driftBaseNs = 0;
    // Device clock anchor (context->Clock...)
    unsigned int gen = 0;
    unsigned long long clkFrames = 0, clkQpc = 0;
    // T2 side
    unsigned long long nextLap = HW_FRAMES;     // next timestamp sample
    unsigned long long t2Consumed = 0;          // frames read so far
    unsigned int prev = 0;
    int heardAudio = 0;
    unsigned long long settleNs = 30000000000ull;

    memset(&r, 0, sizeof(r));
    r.MinLead = 1ll << 40;
    r.MaxLead = -(1ll << 40);
    memset(ring, 0, sizeof(ring));
    T2AudioSpeakerStateReset(&spk);

    for (now = TICK_NS; now <= endNs; now += TICK_NS) {
        unsigned long long qpc = now / 100u;
        unsigned long long playFrame, frame, pending, readNow;

        // bridgeOS: one UPDATE_TIMESTAMP per lap, delivered DELIVERY_NS later.
        for (;;) {
            unsigned long long late = (nextLap == HW_FRAMES) ? g_firstLateNs : 0;
            unsigned long long hostNs = t2_time_of(nextLap, t2Ppb);
            unsigned long long recvNs = hostNs + DELIVERY_NS + late;
            // Absolute host times (the driver sees QPC since boot).
            unsigned long long absHost = UPTIME_NS + hostNs;
            unsigned long long absRecv = UPTIME_NS + recvNs;
            unsigned long long sentNs, usedNs;

            if (recvNs > now) {
                break;
            }
            if (hostMode == HOST_GARBAGE) {
                sentNs = 18446701622728035531ull;
            } else if (hostMode == HOST_WRAPPED) {
                sentNs = absHost - T2AudioClockHostTimeWrapNs(QPC_FREQ);
            } else {
                sentNs = absHost;
            }
            // BceTransport.c: unwrap, then advance the anchor to reception.
            usedNs = g_unwrap ? T2AudioClockUnwrapHostTime(sentNs, absRecv, QPC_FREQ) : sentNs;
            clkFrames = T2AudioClockFramesAtReception(nextLap, usedNs, absRecv, RATE);
            clkQpc = recvNs / 100u;
            gen++;
            nextLap += HW_FRAMES;
        }

        // T2AudioStreamAdvanceLocked
        playFrame = anchorFrames +
                    T2AudioTicksToFramesPpb(qpc - anchorQpc, RATE, QPC_FREQ, ppb);
        // Client (audio engine) keeps the buffer filled ahead of the position.
        while (clientWritten < playFrame + SRC_FRAMES / 3u) {
            unsigned int m = marker(clientWritten) << 8;
            src[(clientWritten % SRC_FRAMES) * 2u] = m;
            src[(clientWritten % SRC_FRAMES) * 2u + 1u] = m;
            clientWritten++;
        }
        frame = lastCopied;
        if (playFrame > frame) {
            unsigned long remaining;

            pending = playFrame - frame;
            if (pending > SRC_FRAMES) {
                frame = playFrame - SRC_FRAMES;
                pending = SRC_FRAMES;
            }

            // T2AudioStreamFollowT2ClockLocked
            if (gen != genSeen) {
                unsigned long long readFrame, newOff;
                long long jump, lead;

                genSeen = gen;
                readFrame = clkFrames + T2AudioTicksToFrames(qpc - clkQpc, RATE, QPC_FREQ);
                lead = T2AudioRingDistance(frame + destOffset, readFrame, HW_FRAMES);
                if (now > settleNs) {
                    if (lead < r.MinLead) r.MinLead = lead;
                    if (lead > r.MaxLead) r.MaxLead = lead;
                }
                if (T2AudioClockCorrection(frame, destOffset, readFrame, HW_FRAMES,
                                           &newOff, &jump)) {
                    if (jump > 0) {
                        unsigned long long z;
                        for (z = 0; z < (unsigned long long)jump && z < HW_FRAMES; z++) {
                            memset(&ring[((frame + destOffset + z) % HW_FRAMES) * 6u], 0,
                                   6u * sizeof(unsigned int));
                        }
                    }
                    destOffset = newOff;
                    r.ReAims++;
                    lead = T2AUDIO_CLOCK_TARGET_LEAD;
                }
                if (rateTracking) {
                    long long drift;

                    if (!driftBaseValid) {
                        driftBaseFrames = clkFrames;
                        driftBaseNs = T2AudioQpcToNs(clkQpc, QPC_FREQ);
                        driftBaseValid = 1;
                    }
                    drift = T2AudioClockDriftPpb(driftBaseFrames, driftBaseNs, clkFrames,
                                                 T2AudioQpcToNs(clkQpc, QPC_FREQ), RATE);
                    // Rebase the source cursor at the old rate, then switch.
                    anchorFrames = playFrame;
                    anchorQpc = qpc;
                    ppb = T2AudioClockRatePpb(drift, lead);
                }
            }

            remaining = (unsigned long)pending;
            while (remaining > 0) {
                unsigned long so = 0, dOff = 0, len = 0;
                unsigned long n = T2AudioCalculateCopyChunk(
                    frame, frame + destOffset, remaining, SRC_BPF,
                    T2AUDIO_RING_BYTES_PER_FRAME, SRC_FRAMES * SRC_BPF,
                    HW_FRAMES * T2AUDIO_RING_BYTES_PER_FRAME, &so, &dOff, &len);

                if (n == 0) {
                    break;
                }
                if (dOff + n * T2AUDIO_RING_BYTES_PER_FRAME > HW_FRAMES * T2AUDIO_RING_BYTES_PER_FRAME ||
                    so + len > SRC_FRAMES * SRC_BPF) {
                    r.OutOfBounds++;
                    break;
                }
                T2AudioRenderStereoToSpeakers(&ring[dOff / 4u], &src[so / 4u], n, 8,
                                              T2AUDIO_TWEETER_MUTE, 0, &spk);
                frame += n;
                remaining -= n;
            }
            lastCopied = playFrame;
        }

        // T2 plays everything up to its current read position.
        readNow = t2_read(now, t2Ppb);
        while (t2Consumed < readNow) {
            unsigned int v = ring[(t2Consumed % HW_FRAMES) * 6u + T2AUDIO_SPK_LEFT_WOOFER_A];

            if (v != 0) {
                if (heardAudio && v != ((prev & MARKER_MASK) + 1u)) {
                    r.Discontinuities++;
                }
                heardAudio = 1;
            } else if (heardAudio && prev != 0) {
                r.Discontinuities++;            // silence after audio
            }
            prev = v;
            t2Consumed++;
        }
    }
    r.FinalPpb = ppb;
    return r;
}

static void report(const char *name, const SIM_RESULT *r)
{
    printf("  %-34s re-aims=%u discontinuities=%u lead=[%lld..%lld] ppb=%lld\n",
           name, r->ReAims, r->Discontinuities, r->MinLead, r->MaxLead, r->FinalPpb);
}

int main(void)
{
    SIM_RESULT a;

    printf("Pipeline, current driver behavior (rate tracking off)\n");
    a = simulate(0, 120, 0, 0);
    report("0 ppm, 2 min", &a);
    check("no out-of-bounds copy", a.OutOfBounds == 0);
    check("0 ppm: lead settles, at most 2 re-aims", a.ReAims <= 2);
    check("0 ppm: continuous after the start", a.Discontinuities <= a.ReAims);

    a = simulate(80000, 600, 0, 0);
    report("+80 ppm, 10 min", &a);
    check("+80 ppm: no out-of-bounds copy", a.OutOfBounds == 0);
    check("+80 ppm: drift forces periodic re-aims (known defect)", a.ReAims >= 3);

    a = simulate(-80000, 600, 0, 0);
    report("-80 ppm, 10 min", &a);
    check("-80 ppm: drift forces periodic re-aims (known defect)", a.ReAims >= 3);

    printf("Pipeline with Parameters\\ClockRateTracking=1\n");
    a = simulate(0, 120, 1, 0);
    report("0 ppm, 2 min", &a);
    check("0 ppm: at most 2 re-aims", a.ReAims <= 2);
    check("0 ppm: rate stays near 0 (|ppb| < 100 ppm)", llabs(a.FinalPpb) < 100000);

    a = simulate(80000, 600, 1, 0);
    report("+80 ppm, 10 min", &a);
    check("+80 ppm: at most 2 re-aims (start only)", a.ReAims <= 2);
    check("+80 ppm: lead stays within tolerance",
          a.MinLead >= (long long)(T2AUDIO_CLOCK_TARGET_LEAD - T2AUDIO_CLOCK_TOLERANCE) &&
              a.MaxLead <= (long long)(T2AUDIO_CLOCK_TARGET_LEAD + T2AUDIO_CLOCK_TOLERANCE));
    check("+80 ppm: discontinuities only at the start", a.Discontinuities <= a.ReAims);

    a = simulate(-80000, 600, 1, 0);
    report("-80 ppm, 10 min", &a);
    check("-80 ppm: at most 2 re-aims (start only)", a.ReAims <= 2);
    check("-80 ppm: discontinuities only at the start", a.Discontinuities <= a.ReAims);

    a = simulate(300000, 600, 1, 0);
    report("+300 ppm, 10 min", &a);
    check("+300 ppm: at most 2 re-aims", a.ReAims <= 2);

    a = simulate(80000, 600, 1, HOST_GARBAGE);
    report("+80 ppm, unusable hostTime, 10 min", &a);
    check("wrapped hostTime: at most 2 re-aims", a.ReAims <= 2);
    check("wrapped hostTime: no out-of-bounds copy", a.OutOfBounds == 0);

    // Reproduces the 2026-10-28 Stage E log: ~50 h uptime (wrapped hostTime)
    // and the first timestamp ~13 ms late -> re-aims -17xx then +6xx.
    printf("Stream start with wrapped hostTime and a late first timestamp\n");
    g_firstLateNs = 13000000ull;
    g_unwrap = 0;
    a = simulate(10000, 20, 0, HOST_WRAPPED);
    report("old driver (no unwrap), 10 ppm", &a);
    check("old driver: second re-aim at start (as on hardware)", a.ReAims == 2);
    g_unwrap = 1;
    a = simulate(10000, 20, 0, HOST_WRAPPED);
    report("with unwrap, 10 ppm", &a);
    check("unwrap: a single re-aim at start", a.ReAims == 1);
    check("unwrap: no gap after the start", a.Discontinuities <= 1);
    a = simulate(10000, 20, 0, HOST_TRUE);
    report("correct hostTime, 10 ppm", &a);
    check("correct hostTime: a single re-aim at start", a.ReAims == 1);
    g_firstLateNs = 0;

    printf("T2AudioClockUnwrapHostTime\n");
    {
        unsigned long long w = T2AudioClockHostTimeWrapNs(QPC_FREQ);
        unsigned long long raw = 18446738975408907977ull;   // logged t2 at ~49.8 h
        unsigned long long recv = 179280000000000ull;
        unsigned long long u = T2AudioClockUnwrapHostTime(raw - 0, w - 5098300643639ull + 2000000ull, QPC_FREQ);

        check("wrap period at 10 MHz = 2^64 * 1e-5 ns", w == 184467440737095ull);
        check("logged wrapped value unwraps next to reception",
              u == raw + w && T2AudioClockHostTimePlausible(u, w - 5098300643639ull + 2000000ull));
        check("plausible hostTime unchanged",
              T2AudioClockUnwrapHostTime(recv - 2000000ull, recv, QPC_FREQ) == recv - 2000000ull);
        check("garbage stays garbage (caller falls back)",
              T2AudioClockUnwrapHostTime(18446701622728035531ull, 1000000000ull, QPC_FREQ) ==
                  18446701622728035531ull);
        check("future hostTime not moved",
              T2AudioClockUnwrapHostTime(recv + 5000000ull, recv, QPC_FREQ) == recv + 5000000ull);
    }

    printf("T2AudioTicksToFramesPpb / T2AudioClockDriftPpb\n");
    check("ppb 0 matches T2AudioTicksToFrames",
          T2AudioTicksToFramesPpb(123456789ull, RATE, QPC_FREQ, 0) ==
              T2AudioTicksToFrames(123456789ull, RATE, QPC_FREQ));
    check("+1000 ppm adds 0.1%",
          T2AudioTicksToFramesPpb(10ull * QPC_FREQ, RATE, QPC_FREQ, 1000000) == 480480ull);
    check("-1000 ppm removes 0.1%",
          T2AudioTicksToFramesPpb(10ull * QPC_FREQ, RATE, QPC_FREQ, -1000000) == 479520ull);
    check("drift: 48012 frames in 1 s = +250 ppm",
          T2AudioClockDriftPpb(0, 0, 48012, 1000000000ull, RATE) == 250000);
    check("drift: 47988 frames in 1 s = -250 ppm",
          T2AudioClockDriftPpb(0, 0, 47988, 1000000000ull, RATE) == -250000);
    check("drift: baseline under 1 s ignored",
          T2AudioClockDriftPpb(0, 0, 48000, 999999999ull, RATE) == 0);
    check("drift: clamped to +-500 ppm",
          T2AudioClockDriftPpb(0, 0, 60000, 1000000000ull, RATE) == T2AUDIO_RATE_MAX_DRIFT_PPB);
    check("rate: lead above target slows the source",
          T2AudioClockRatePpb(0, T2AUDIO_CLOCK_TARGET_LEAD + 100) == -100000);
    check("rate: clamped",
          T2AudioClockRatePpb(T2AUDIO_RATE_MAX_PPB, 0) == T2AUDIO_RATE_MAX_PPB);

    if (g_failures != 0) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
