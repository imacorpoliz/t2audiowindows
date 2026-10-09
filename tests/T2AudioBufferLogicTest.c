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
}

int main(void)
{
    test_align();
    test_hardware_satisfies();
    test_release_decision();
    test_hardware_io_decision();
    test_device_table_fits();

    printf("\n%s (%d failure%s)\n",
           g_failures ? "FAILED" : "ALL PASSED",
           g_failures,
           g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
