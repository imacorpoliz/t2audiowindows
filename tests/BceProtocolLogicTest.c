// Host-side test for the pure T2 BCE protocol helpers in
// ../src/BceProtocolLogic.h. Build and run with a normal MSVC environment:
//
//   cl /nologo /W4 /Fe:tests\BceProtocolLogicTest.exe tests\BceProtocolLogicTest.c
//   tests\BceProtocolLogicTest.exe
//
// The helpers are the exact code compiled into the kernel driver, so this
// exercises the real length/offset/UID logic, not a copy.

#include <stdio.h>
#include <string.h>
#include "../src/BceProtocolLogic.h"

static int g_failures = 0;

static void check(const char *name, int condition)
{
    printf("  [%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) {
        g_failures++;
    }
}

static void test_scalar_roundtrip(void)
{
    unsigned char buf[8];
    unsigned long long v64 = 0x0102030405060708ull;
    unsigned long v32 = 0xDEADBEEFul;

    printf("scalar little-endian accessors\n");

    T2AudioBceWriteU64(buf, v64);
    check("u64 round-trip", T2AudioBceReadU64(buf) == v64);
    check("u64 byte 0 == 0x08", buf[0] == 0x08);
    check("u64 byte 7 == 0x01", buf[7] == 0x01);

    T2AudioBceWriteU32(buf, v32);
    check("u32 round-trip", T2AudioBceReadU32(buf) == v32);
    check("u32 byte 0 == 0xEF", buf[0] == 0xEF);
}

static void test_device_list(void)
{
    unsigned long out = 0;

    printf("T2AudioBceDeviceListCount\n");

    // header(13)+base(8)+count(8) = 29, plus 2 ids = 45.
    check("exact fit 2 ids -> ok",
          T2AudioBceDeviceListCount(45, 2, 32, &out) && out == 2);
    check("count 0 with header only -> ok",
          T2AudioBceDeviceListCount(29, 0, 32, &out) && out == 0);
    check("reply too small -> fail",
          !T2AudioBceDeviceListCount(28, 0, 32, &out));
    check("count exceeds returned array -> fail",
          !T2AudioBceDeviceListCount(45, 3, 32, &out));
    check("count clamped to MaxDevices",
          T2AudioBceDeviceListCount(29 + 8 * 10, 10, 4, &out) && out == 4);
    check("huge count -> fail",
          !T2AudioBceDeviceListCount(45, 0xFFFFFFFFFFFFFFFFull, 32, &out));
    check("NULL output -> fail",
          !T2AudioBceDeviceListCount(45, 2, 32, NULL));
}

static void test_property_offset(void)
{
    unsigned long long offset = 0;
    unsigned long long size = 0;

    printf("T2AudioBcePropertyDataOffset\n");

    // fixed = 13+8+28 = 49.
    check("empty property -> ok at 49",
          T2AudioBcePropertyDataOffset(49, 0, &offset, &size) &&
          offset == 49 && size == 0);
    check("7-byte UID -> ok at 49",
          T2AudioBcePropertyDataOffset(56, 7, &offset, &size) &&
          offset == 49 && size == 7);
    check("data_size exceeds reply -> fail",
          !T2AudioBcePropertyDataOffset(50, 2, &offset, &size));
    check("reply shorter than fixed -> fail",
          !T2AudioBcePropertyDataOffset(48, 0, &offset, &size));
    check("NULL offset -> fail",
          !T2AudioBcePropertyDataOffset(56, 7, NULL, &size));
    check("NULL size -> fail",
          !T2AudioBcePropertyDataOffset(56, 7, &offset, NULL));
}

static void test_uid_match(void)
{
    printf("T2AudioBceUidIsSpeaker\n");

    check("\"Speaker\" (7) -> match",
          T2AudioBceUidIsSpeaker("Speaker", 7));
    check("\"SpeakerX\" (8) -> no match",
          !T2AudioBceUidIsSpeaker("SpeakerX", 8));
    check("\"Speaker\" as 8 incl NUL -> no match",
          !T2AudioBceUidIsSpeaker("Speaker\0", 8));
    check("lowercase \"speaker\" -> no match",
          !T2AudioBceUidIsSpeaker("speaker", 7));
    check("\"Codec Output\" -> no match",
          !T2AudioBceUidIsSpeaker("Codec Output", 12));
    check("empty -> no match",
          !T2AudioBceUidIsSpeaker("", 0));
    check("NULL -> no match",
          !T2AudioBceUidIsSpeaker(NULL, 7));
}

int main(void)
{
    test_scalar_roundtrip();
    test_device_list();
    test_property_offset();
    test_uid_match();

    printf("\n%s (%d failure%s)\n",
           g_failures ? "FAILED" : "ALL PASSED",
           g_failures,
           g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
