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

// Build a response header + base with the given type/message/status.
static void build_reply(unsigned char *b, unsigned char type,
                        unsigned long message, unsigned long status)
{
    memset(b, 0, 128);
    b[0] = 'A'; b[1] = 'u'; b[2] = 'd'; b[3] = 't';
    b[T2AUDIO_BCE_TYPE_OFFSET] = type;
    T2AudioBceWriteU32(b + T2AUDIO_BCE_HEADER_SIZE, message);
    T2AudioBceWriteU32(b + T2AUDIO_BCE_HEADER_SIZE + T2AUDIO_BCE_U32_SIZE,
                       status);
}

static void test_parse_device_list_response(void)
{
    unsigned char b[128];
    unsigned long long ids[8];
    unsigned long count = 0;
    int ok;

    printf("T2AudioBceParseDeviceListResponse\n");

    // header(13)+base(8)+count(8)+2 ids(16) = 45.
    build_reply(b, T2AUDIO_BCE_MSG_RESPONSE, 102, 0);
    T2AudioBceWriteU64(b + 21, 2);
    T2AudioBceWriteU64(b + 29, 0x39);
    T2AudioBceWriteU64(b + 37, 0x3A);
    ok = T2AudioBceParseDeviceListResponse(b, 45, 102, ids, 8, &count);
    check("valid 2 ids -> ok, ids copied",
          ok && count == 2 && ids[0] == 0x39 && ids[1] == 0x3A);

    check("truncated (count=2, 44 bytes) -> fail",
          !T2AudioBceParseDeviceListResponse(b, 44, 102, ids, 8, &count));

    build_reply(b, T2AUDIO_BCE_MSG_RESPONSE, 103, 0);
    T2AudioBceWriteU64(b + 21, 0);
    check("wrong message id -> fail",
          !T2AudioBceParseDeviceListResponse(b, 29, 102, ids, 8, &count));

    build_reply(b, T2AUDIO_BCE_MSG_RESPONSE, 102, 0x80000000ul);
    T2AudioBceWriteU64(b + 21, 0);
    check("non-zero status -> fail",
          !T2AudioBceParseDeviceListResponse(b, 29, 102, ids, 8, &count));

    build_reply(b, T2AUDIO_BCE_MSG_COMMAND, 102, 0);
    T2AudioBceWriteU64(b + 21, 0);
    check("command type (not response) -> fail",
          !T2AudioBceParseDeviceListResponse(b, 29, 102, ids, 8, &count));

    build_reply(b, T2AUDIO_BCE_MSG_RESPONSE, 102, 0);
    T2AudioBceWriteU64(b + 21, 3);
    T2AudioBceWriteU64(b + 29, 1);
    T2AudioBceWriteU64(b + 37, 2);
    T2AudioBceWriteU64(b + 45, 3);
    ok = T2AudioBceParseDeviceListResponse(b, 53, 102, ids, 2, &count);
    check("count clamped to MaxDevices -> ok, count=2",
          ok && count == 2 && ids[0] == 1 && ids[1] == 2);

    check("NULL ids with count>0 -> fail",
          !T2AudioBceParseDeviceListResponse(b, 53, 102, NULL, 2, &count));
}

static void test_parse_property_response(void)
{
    unsigned char b[128];
    unsigned long long obj;
    unsigned long element;
    unsigned long scope;
    unsigned long selector;
    unsigned long long offset;
    unsigned long long size;
    int ok;

    printf("T2AudioBceParsePropertyResponse\n");

    // fixed prefix = 13+8+28 = 49; data = 7 -> 56 total.
    build_reply(b, T2AUDIO_BCE_MSG_RESPONSE, 8, 0);
    T2AudioBceWriteU64(b + 21, 0x39);
    T2AudioBceWriteU32(b + 29, 0);
    T2AudioBceWriteU32(b + 33, 0x676c6f62ul);
    T2AudioBceWriteU32(b + 37, 0x75696420ul);
    T2AudioBceWriteU64(b + 41, 7);
    memcpy(b + 49, "Speaker", 7);
    ok = T2AudioBceParsePropertyResponse(b, 56, 8, &obj, &element, &scope,
                                         &selector, &offset, &size);
    check("valid UID reply -> ok, fields parsed",
          ok && obj == 0x39 && element == 0 && scope == 0x676c6f62ul &&
          selector == 0x75696420ul && offset == 49 && size == 7);

    check("truncated (48 bytes) -> fail",
          !T2AudioBceParsePropertyResponse(b, 48, 8, &obj, &element, &scope,
                                           &selector, &offset, &size));

    // Advertise 8 data bytes but only 7 present (56 total).
    T2AudioBceWriteU64(b + 41, 8);
    check("data_size exceeds reply -> fail",
          !T2AudioBceParsePropertyResponse(b, 56, 8, &obj, &element, &scope,
                                           &selector, &offset, &size));
    T2AudioBceWriteU64(b + 41, 7);

    build_reply(b, T2AUDIO_BCE_MSG_RESPONSE, 8, 0x1234);
    T2AudioBceWriteU64(b + 41, 0);
    check("non-zero status -> fail",
          !T2AudioBceParsePropertyResponse(b, 49, 8, &obj, &element, &scope,
                                           &selector, &offset, &size));

    build_reply(b, T2AUDIO_BCE_MSG_RESPONSE, 7, 0);
    T2AudioBceWriteU64(b + 41, 0);
    check("wrong message id -> fail",
          !T2AudioBceParsePropertyResponse(b, 49, 8, &obj, &element, &scope,
                                           &selector, &offset, &size));

    build_reply(b, T2AUDIO_BCE_MSG_COMMAND, 8, 0);
    T2AudioBceWriteU64(b + 41, 0);
    check("command type (not response) -> fail",
          !T2AudioBceParsePropertyResponse(b, 49, 8, &obj, &element, &scope,
                                           &selector, &offset, &size));
}

static void test_parse_command_response(void)
{
    unsigned char b[128];

    printf("T2AudioBceParseCommandResponse\n");

    build_reply(b, T2AUDIO_BCE_MSG_RESPONSE, 0, 0);
    T2AudioBceWriteU64(b + 5, 0x39);
    check("valid START_IO ack -> ok",
          T2AudioBceParseCommandResponse(b, 21, 0, 0x39));

    check("short reply -> fail",
          !T2AudioBceParseCommandResponse(b, 20, 0, 0x39));

    build_reply(b, T2AUDIO_BCE_MSG_RESPONSE, 2, 0);
    T2AudioBceWriteU64(b + 5, 0x39);
    check("wrong message id -> fail",
          !T2AudioBceParseCommandResponse(b, 21, 0, 0x39));

    build_reply(b, T2AUDIO_BCE_MSG_RESPONSE, 0, 0);
    T2AudioBceWriteU64(b + 5, 0x3A);
    check("wrong echoed device id -> fail",
          !T2AudioBceParseCommandResponse(b, 21, 0, 0x39));

    build_reply(b, T2AUDIO_BCE_MSG_RESPONSE, 0, 0x80000000ul);
    T2AudioBceWriteU64(b + 5, 0x39);
    check("non-zero status -> fail",
          !T2AudioBceParseCommandResponse(b, 21, 0, 0x39));

    build_reply(b, T2AUDIO_BCE_MSG_COMMAND, 0, 0);
    T2AudioBceWriteU64(b + 5, 0x39);
    check("command type (not response) -> fail",
          !T2AudioBceParseCommandResponse(b, 21, 0, 0x39));

    build_reply(b, T2AUDIO_BCE_MSG_RESPONSE, 0, 0);
    T2AudioBceWriteU64(b + 5, 0x39);
    b[0] = 'X';
    check("bad tag -> fail",
          !T2AudioBceParseCommandResponse(b, 21, 0, 0x39));

    check("NULL reply -> fail",
          !T2AudioBceParseCommandResponse(NULL, 21, 0, 0x39));
}

int main(void)
{
    test_scalar_roundtrip();
    test_device_list();
    test_property_offset();
    test_uid_match();
    test_parse_device_list_response();
    test_parse_property_response();
    test_parse_command_response();

    printf("\n%s (%d failure%s)\n",
           g_failures ? "FAILED" : "ALL PASSED",
           g_failures,
           g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
