// Host-side test for the pure WaveRT buffer helpers in
// ../src/T2AudioBufferLogic.h. Build and run with a normal MSVC environment:
//
//   cl /nologo /W4 /Fe:tests\T2AudioBufferLogicTest.exe tests\T2AudioBufferLogicTest.c
//   tests\T2AudioBufferLogicTest.exe
//
// The helpers are the exact code compiled into the kernel driver, so this
// exercises the real arithmetic and release-decision logic, not a copy.

#include <stdio.h>
#include "../src/T2AudioBufferLogic.h"

static int g_failures = 0;

static void check(const char *name, int condition)
{
    printf("  [%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) {
        g_failures++;
    }
}

static void test_align(void)
{
    unsigned long a = 0;

    printf("T2AudioAlignSizeUpToFrame\n");

    check("100/24 -> 120",
          T2AudioAlignSizeUpToFrame(100, 24, &a) && a == 120);
    check("24/24 -> 24 (already aligned)",
          T2AudioAlignSizeUpToFrame(24, 24, &a) && a == 24);
    check("1/24 -> 24",
          T2AudioAlignSizeUpToFrame(1, 24, &a) && a == 24);
    check("25/24 -> 48",
          T2AudioAlignSizeUpToFrame(25, 24, &a) && a == 48);
    check("0xFFFFFFF0/24 -> unchanged",
          T2AudioAlignSizeUpToFrame(0xFFFFFFF0ul, 24, &a) && a == 0xFFFFFFF0ul);
    check("0 requested -> fail",
          !T2AudioAlignSizeUpToFrame(0, 24, &a));
    check("BytesPerFrame 0 -> fail",
          !T2AudioAlignSizeUpToFrame(100, 0, &a));
    check("NULL output -> fail",
          !T2AudioAlignSizeUpToFrame(100, 24, NULL));
    check("0xFFFFFFFF/24 overflow -> fail",
          !T2AudioAlignSizeUpToFrame(0xFFFFFFFFul, 24, &a));
}

static void test_hardware_satisfies(void)
{
    printf("T2AudioHardwareBufferSatisfies\n");

    check("1000<=1200 aligned -> ok",
          T2AudioHardwareBufferSatisfies(1000, 1200, 24));
    check("equal size -> ok",
          T2AudioHardwareBufferSatisfies(1200, 1200, 24));
    check("buffer smaller than request -> fail",
          !T2AudioHardwareBufferSatisfies(1200, 1000, 24));
    check("buffer not frame-aligned -> fail",
          !T2AudioHardwareBufferSatisfies(1000, 1001, 24));
    check("BytesPerFrame 0 -> fail",
          !T2AudioHardwareBufferSatisfies(1000, 1200, 0));
}

static void test_release_decision(void)
{
    printf("T2AudioDecideBufferRelease\n");

    check("system buffer, matching MDL -> SYSTEM",
          T2AudioDecideBufferRelease(1, 0, 1) == T2AUDIO_RELEASE_SYSTEM);
    check("hardware buffer, matching MDL -> HARDWARE",
          T2AudioDecideBufferRelease(1, 1, 1) == T2AUDIO_RELEASE_HARDWARE);
    check("foreign MDL -> NONE",
          T2AudioDecideBufferRelease(1, 0, 0) == T2AUDIO_RELEASE_NONE);
    check("foreign hardware MDL -> NONE",
          T2AudioDecideBufferRelease(1, 1, 0) == T2AUDIO_RELEASE_NONE);
    check("nothing held (double free) -> NONE",
          T2AudioDecideBufferRelease(0, 0, 1) == T2AUDIO_RELEASE_NONE);
    check("nothing held, foreign -> NONE",
          T2AudioDecideBufferRelease(0, 0, 0) == T2AUDIO_RELEASE_NONE);
}

static void test_hardware_io_decision(void)
{
    printf("T2AudioDecideHardwareIo\n");

    check("device path, not started -> start",
          T2AudioDecideHardwareIo(0, 1, 0) == 1);
    check("ForceSystemBuffer -> never start",
          T2AudioDecideHardwareIo(1, 1, 0) == 0);
    check("no speaker wired -> never start",
          T2AudioDecideHardwareIo(0, 0, 0) == 0);
    check("already started -> no double start",
          T2AudioDecideHardwareIo(0, 1, 1) == 0);
    check("forced + already started -> no start",
          T2AudioDecideHardwareIo(1, 1, 1) == 0);

    printf("T2AudioDecideStopHardwareIo\n");
    check("started -> stop",
          T2AudioDecideStopHardwareIo(1) == 1);
    check("never started -> no stop (no unmatched STOP_IO)",
          T2AudioDecideStopHardwareIo(0) == 0);

    printf("T2AudioShouldStartCopyTimer\n");
    check("buffer + size, idle -> start",
          T2AudioShouldStartCopyTimer(1, 4096, 0) == 1);
    check("already active -> no restart",
          T2AudioShouldStartCopyTimer(1, 4096, 1) == 0);
    check("no system buffer -> no timer",
          T2AudioShouldStartCopyTimer(0, 4096, 0) == 0);
    check("zero copy size -> no timer",
          T2AudioShouldStartCopyTimer(1, 0, 0) == 0);
}

static void test_device_table_fits(void)
{
    // Mirrors the kernel metadata layout: 0x10-byte header, 0xBDEC-byte stride,
    // max 20 devices, mapped BAR up to 4 MB.
    const unsigned long long hdr = 0x10ull;
    const unsigned long long stride = 0xBDECull;
    const unsigned long long maxd = 20ull;
    const unsigned long long one = hdr + stride;

    printf("T2AudioDeviceTableFits\n");

    check("0 devices, exactly header -> fits",
          T2AudioDeviceTableFits(0, maxd, hdr, stride, hdr));
    check("1 device, exactly one entry -> fits",
          T2AudioDeviceTableFits(1, maxd, hdr, stride, one));
    check("1 device, one byte short -> fail",
          !T2AudioDeviceTableFits(1, maxd, hdr, stride, one - 1));
    check("20 devices in 4 MB -> fits",
          T2AudioDeviceTableFits(20, maxd, hdr, stride, 0x400000ull));
    check("21 devices exceeds max -> fail",
          !T2AudioDeviceTableFits(21, maxd, hdr, stride, 0x400000ull));
    check("corrupt count (0xFFFFFFFF) -> fail",
          !T2AudioDeviceTableFits(0xFFFFFFFFull, maxd, hdr, stride, 0x400000ull));
    check("huge count cannot overflow into fit",
          !T2AudioDeviceTableFits(0xFFFFFFFFFFFFFFFFull, maxd, hdr, stride,
                                  0xFFFFFFFFFFFFFFFFull));
    check("entry multiplication cannot overflow into fit",
          !T2AudioDeviceTableFits(0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull,
                                  0, 2, 16));
    check("header addition cannot overflow into fit",
          !T2AudioDeviceTableFits(1, 1, 0xFFFFFFFFFFFFFFFFull, 1, 16));
}

static void test_calculate_copy_chunk(void)
{
    unsigned long srcOff = 0, dstOff = 0, byteLen = 0, frames = 0;
    // Source: 480 frames * 24 = 11520 bytes (10ms @ 48kHz)
    // Dest: 16640 frames * 24 = 399360 bytes (346.6ms hardware ring)
    const unsigned long srcBuf = 11520;
    const unsigned long dstBuf = 399360;
    const unsigned long bpf = 24;

    printf("T2AudioCalculateCopyChunk\n");

    // 1. Initial 96 frames from frame 0
    frames = T2AudioCalculateCopyChunk(0, 0, 96, bpf, srcBuf, dstBuf, &srcOff, &dstOff, &byteLen);
    check("initial 96 frames -> frames==96", frames == 96);
    check("initial 96 frames -> srcOff==0", srcOff == 0);
    check("initial 96 frames -> dstOff==0", dstOff == 0);
    check("initial 96 frames -> byteLen==2304", byteLen == 2304);

    // 2. Near end of source buffer (frame 470, request 20 frames)
    // Source buffer has only 10 frames remaining (470..479). First chunk should copy 10 frames.
    frames = T2AudioCalculateCopyChunk(470, 470, 20, bpf, srcBuf, dstBuf, &srcOff, &dstOff, &byteLen);
    check("source wrap chunk 1 -> frames==10", frames == 10);
    check("source wrap chunk 1 -> srcOff==11280", srcOff == 470 * 24);
    check("source wrap chunk 1 -> byteLen==240", byteLen == 240);

    // Following chunk starts at frame 480, should wrap source offset to 0
    frames = T2AudioCalculateCopyChunk(480, 480, 10, bpf, srcBuf, dstBuf, &srcOff, &dstOff, &byteLen);
    check("source wrap chunk 2 -> frames==10", frames == 10);
    check("source wrap chunk 2 -> srcOff==0", srcOff == 0);
    check("source wrap chunk 2 -> dstOff==11520", dstOff == 480 * 24);

    // 3. Near end of destination buffer (frame 16630, request 20 frames)
    // Dest has only 10 frames remaining (16630..16639). First chunk should copy 10 frames.
    frames = T2AudioCalculateCopyChunk(16630, 16630, 20, bpf, srcBuf, dstBuf, &srcOff, &dstOff, &byteLen);
    check("dest wrap chunk 1 -> frames==10", frames == 10);
    check("dest wrap chunk 1 -> dstOff==399120", dstOff == 16630 * 24);
    check("dest wrap chunk 1 -> byteLen==240", byteLen == 240);

    // Following chunk starts at frame 16640, should wrap dest offset to 0
    frames = T2AudioCalculateCopyChunk(16640, 16640, 10, bpf, srcBuf, dstBuf, &srcOff, &dstOff, &byteLen);
    check("dest wrap chunk 2 -> frames==10", frames == 10);
    check("dest wrap chunk 2 -> dstOff==0", dstOff == 0);

    // 4. Boundary and invalid conditions
    check("0 remaining frames -> 0",
          T2AudioCalculateCopyChunk(0, 0, 0, bpf, srcBuf, dstBuf, &srcOff, &dstOff, &byteLen) == 0);
    check("0 bpf -> 0",
          T2AudioCalculateCopyChunk(0, 0, 96, 0, srcBuf, dstBuf, &srcOff, &dstOff, &byteLen) == 0);
    check("0 srcBuf -> 0",
          T2AudioCalculateCopyChunk(0, 0, 96, bpf, 0, dstBuf, &srcOff, &dstOff, &byteLen) == 0);
    check("NULL output -> 0",
          T2AudioCalculateCopyChunk(0, 0, 96, bpf, srcBuf, dstBuf, NULL, &dstOff, &byteLen) == 0);
    frames = T2AudioCalculateCopyChunk(0, 0, 0x20000000ul, bpf, srcBuf, dstBuf,
                                       &srcOff, &dstOff, &byteLen);
    check("large frame count copies up to ring boundary without overflow",
          frames == 480 && srcOff == 0 && dstOff == 0 && byteLen == srcBuf);
    check("unaligned source ring -> 0",
          T2AudioCalculateCopyChunk(0, 0, 1, bpf, 25, dstBuf,
                                    &srcOff, &dstOff, &byteLen) == 0);
    check("unaligned destination ring -> 0",
          T2AudioCalculateCopyChunk(0, 0, 1, bpf, srcBuf, 25,
                                    &srcOff, &dstOff, &byteLen) == 0);
    frames = T2AudioCalculateCopyChunk(0x100000000ull, 0x100000000ull, 1, bpf, srcBuf, dstBuf,
                                       &srcOff, &dstOff, &byteLen);
    check("frame cursor beyond 32 bits uses the full ring position",
          frames == 1 && srcOff == (0x100000000ull % 480) * bpf &&
          dstOff == (0x100000000ull % 16640) * bpf && byteLen == bpf);

    // 5. Destination runs a fixed lead (720 frames) ahead of the source.
    frames = T2AudioCalculateCopyChunk(100, 100 + 720, 96, bpf, srcBuf, dstBuf,
                                       &srcOff, &dstOff, &byteLen);
    check("lead: src at frame 100, dst at frame 820",
          frames == 96 && srcOff == 100 * bpf && dstOff == 820 * bpf &&
          byteLen == 96 * bpf);
    frames = T2AudioCalculateCopyChunk(16000, 16000 + 720, 96, bpf, srcBuf, dstBuf,
                                       &srcOff, &dstOff, &byteLen);
    check("lead: destination index wraps independently of the source",
          frames == 96 && dstOff == (16720 % 16640) * bpf &&
          srcOff == (16000 % 480) * bpf);
    frames = T2AudioCalculateCopyChunk(0, 16630, 96, bpf, srcBuf, dstBuf,
                                       &srcOff, &dstOff, &byteLen);
    check("lead: chunk stops at the destination ring end",
          frames == 10 && srcOff == 0 && dstOff == 16630 * bpf);
}

static void test_ticks_to_frames(void)
{
    printf("T2AudioTicksToFrames\n");

    check("1 s at 10 MHz -> 48000 frames",
          T2AudioTicksToFrames(10000000ull, 48000, 10000000ull) == 48000ull);
    check("0.5 ms at 10 MHz -> 24 frames",
          T2AudioTicksToFrames(5000ull, 48000, 10000000ull) == 24ull);
    check("zero frequency -> 0",
          T2AudioTicksToFrames(12345ull, 48000, 0) == 0ull);
    // Naive ticks * rate overflows 64 bits for this interval.
    check("huge interval does not overflow",
          T2AudioTicksToFrames(0x0010000000000000ull, 48000, 3000000000ull) ==
              (0x0010000000000000ull / 3000000000ull) * 48000ull +
              ((0x0010000000000000ull % 3000000000ull) * 48000ull) / 3000000000ull);
}

int main(void)
{
    test_align();
    test_hardware_satisfies();
    test_release_decision();
    test_hardware_io_decision();
    test_device_table_fits();
    test_calculate_copy_chunk();
    test_ticks_to_frames();

    printf("\n%s (%d failure%s)\n",
           g_failures ? "FAILED" : "ALL PASSED",
           g_failures,
           g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
