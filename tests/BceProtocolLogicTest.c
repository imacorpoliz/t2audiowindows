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
    printf("T2AudioBceUidEquals\n");
    check("\"Codec Output\" (12) equals", T2AudioBceUidEquals("Codec Output", 12, "Codec Output"));
    check("prefix only -> no", !T2AudioBceUidEquals("Codec Out", 9, "Codec Output"));
    check("longer -> no", !T2AudioBceUidEquals("Codec Output2", 13, "Codec Output"));
    check("empty vs name -> no", !T2AudioBceUidEquals("", 0, "Codec Output"));
    check("NULL -> no", !T2AudioBceUidEquals(0, 3, "abc"));

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
    unsigned char b[256] = {0};

    printf("T2AudioBceParseCommandResponse\n");

    build_reply(b, T2AUDIO_BCE_MSG_RESPONSE, T2AUDIO_MSG_START_IO_RESPONSE, 0);
    T2AudioBceWriteU64(b + 5, 0x39);
    check("valid START_IO ack -> ok",
          T2AudioBceParseCommandResponse(b, 21, T2AUDIO_MSG_START_IO_RESPONSE));

    build_reply(b, T2AUDIO_BCE_MSG_RESPONSE, T2AUDIO_MSG_STOP_IO_RESPONSE, 0);
    T2AudioBceWriteU64(b + 5, 0x39);
    check("valid STOP_IO ack -> ok",
          T2AudioBceParseCommandResponse(b, 21, T2AUDIO_MSG_STOP_IO_RESPONSE));

    check("short reply -> fail",
          !T2AudioBceParseCommandResponse(b, 20, T2AUDIO_MSG_START_IO_RESPONSE));

    build_reply(b, T2AUDIO_BCE_MSG_RESPONSE, 2, 0);
    T2AudioBceWriteU64(b + 5, 0x39);
    check("wrong message id -> fail",
          !T2AudioBceParseCommandResponse(b, 21, T2AUDIO_MSG_START_IO_RESPONSE));

    build_reply(b, T2AUDIO_BCE_MSG_RESPONSE, T2AUDIO_MSG_START_IO_RESPONSE, 0);
    T2AudioBceWriteU64(b + 5, 0x3A);
    check("other echoed device id is accepted (AppleAudio does not check it)",
          T2AudioBceParseCommandResponse(b, 21, T2AUDIO_MSG_START_IO_RESPONSE));

    build_reply(b, T2AUDIO_BCE_MSG_RESPONSE, T2AUDIO_MSG_START_IO_RESPONSE, 0x80000000ul);
    T2AudioBceWriteU64(b + 5, 0x39);
    check("non-zero status -> fail",
          !T2AudioBceParseCommandResponse(b, 21, T2AUDIO_MSG_START_IO_RESPONSE));

    build_reply(b, T2AUDIO_BCE_MSG_COMMAND, T2AUDIO_MSG_START_IO_RESPONSE, 0);
    T2AudioBceWriteU64(b + 5, 0x39);
    check("type byte is not checked (AppleAudio does not check it)",
          T2AudioBceParseCommandResponse(b, 21, T2AUDIO_MSG_START_IO_RESPONSE));

    build_reply(b, T2AUDIO_BCE_MSG_RESPONSE, T2AUDIO_MSG_START_IO_RESPONSE, 0);
    check("256-byte reply buffer (as VHCI returns) -> ok",
          T2AudioBceParseCommandResponse(b, 256, T2AUDIO_MSG_START_IO_RESPONSE));

    build_reply(b, T2AUDIO_BCE_MSG_RESPONSE, T2AUDIO_MSG_START_IO_RESPONSE, 0);
    T2AudioBceWriteU64(b + 5, 0x39);
    T2AudioBceWriteU32(b, 42);
    check("tag is not checked here (transport checks it)",
          T2AudioBceParseCommandResponse(b, 21, T2AUDIO_MSG_START_IO_RESPONSE));

    check("NULL reply -> fail",
          !T2AudioBceParseCommandResponse(NULL, 21, T2AUDIO_MSG_START_IO_RESPONSE));
}

static void test_physical_format(void)
{
    // Speaker stream 0x3A 'pft ' payload captured on MacBookPro16,1
    // (docs/planning/NOTES.md, base64 AAAAAABw50Bt...).
    static const unsigned char kSpeaker[40] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0xE7, 0x40, 0x6D, 0x63,
        0x70, 0x6C, 0x04, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x06, 0x00,
        0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    T2AUDIO_BCE_PHYS_FORMAT f;
    unsigned long v = 0;
    unsigned char bad[40];

    printf("T2AudioBceParsePhysicalFormat / DeviceSampleShift\n");

    check("hardware descriptor parses",
          T2AudioBceParsePhysicalFormat(kSpeaker, sizeof(kSpeaker), &f));
    check("rate 48000", f.SampleRate == 48000);
    check("format lpcm", f.FormatId == T2AUDIO_BCE_FORMAT_LPCM);
    check("flags SIGNED only", f.FormatFlags == T2AUDIO_BCE_FORMAT_FLAG_SIGNED);
    check("6 ch, 24 B/frame, 24 bits",
          f.ChannelsPerFrame == 6 && f.BytesPerFrame == 24 &&
          f.BytesPerPacket == 24 && f.FramesPerPacket == 1 &&
          f.BitsPerChannel == 24);
    check("speaker ring is S24_LE -> shift 8",
          T2AudioBceDeviceSampleShift(&f, 6, 48000) == 8);
    check("too short -> fail",
          !T2AudioBceParsePhysicalFormat(kSpeaker, 39, &f));

    T2AudioBceParsePhysicalFormat(kSpeaker, sizeof(kSpeaker), &f);
    f.FormatFlags |= T2AUDIO_BCE_FORMAT_FLAG_ALIGNED_HIGH;
    check("24-bit aligned high -> shift 0", T2AudioBceDeviceSampleShift(&f, 6, 48000) == 0);
    f.FormatFlags = T2AUDIO_BCE_FORMAT_FLAG_SIGNED;
    f.BitsPerChannel = 32;
    check("32-bit -> shift 0", T2AudioBceDeviceSampleShift(&f, 6, 48000) == 0);
    f.FormatFlags = T2AUDIO_BCE_FORMAT_FLAG_SIGNED | T2AUDIO_BCE_FORMAT_FLAG_FLOAT;
    check("float -> unsupported", T2AudioBceDeviceSampleShift(&f, 6, 48000) == -1);
    f.FormatFlags = T2AUDIO_BCE_FORMAT_FLAG_SIGNED;
    check("wrong channel count -> unsupported", T2AudioBceDeviceSampleShift(&f, 2, 48000) == -1);
    check("wrong rate -> unsupported", T2AudioBceDeviceSampleShift(&f, 6, 44100) == -1);
    f.FormatFlags = 0;
    check("unsigned -> unsupported", T2AudioBceDeviceSampleShift(&f, 6, 48000) == -1);
    check("NULL format -> unsupported", T2AudioBceDeviceSampleShift(0, 6, 48000) == -1);

    // 44100.0 = 0x40E5888000000000, 1.5 = 0x3FF8000000000000.
    check("double 44100 -> 44100",
          T2AudioBceDecodeIntegralDouble(0x40E5888000000000ull, &v) && v == 44100);
    check("double 1.5 (fractional) -> fail",
          !T2AudioBceDecodeIntegralDouble(0x3FF8000000000000ull, &v));
    check("double -48000 -> fail",
          !T2AudioBceDecodeIntegralDouble(0xC0E7700000000000ull, &v));
    check("double 2^32 -> fail",
          !T2AudioBceDecodeIntegralDouble(0x41F0000000000000ull, &v));
    check("double 0 -> 0",
          T2AudioBceDecodeIntegralDouble(0, &v) && v == 0);

    memcpy(bad, kSpeaker, sizeof(bad));
    bad[7] = 0xBF;  // negative sample rate
    check("negative rate descriptor -> fail",
          !T2AudioBceParsePhysicalFormat(bad, sizeof(bad), &f));
}

static void test_incoming(void)
{
    unsigned char m[45];
    unsigned long type = 0, msg = 0;
    unsigned long long dev = 0, ts = 0, seed = 0, bits = 0, frames = 0;

    printf("incoming messages (IOCTL 0x222020)\n");

    memset(m, 0, sizeof(m));
    memcpy(m, "S001", 4);
    m[4] = 1;                                   // COMMAND from bridgeOS
    T2AudioBceWriteU64(m + 5, 0x39);
    T2AudioBceWriteU32(m + 13, T2AUDIO_MSG_UPDATE_TIMESTAMP);
    T2AudioBceWriteU64(m + 21, 123456789ull);   // timestamp (AppleAudio: +0x15)
    T2AudioBceWriteU64(m + 29, 1);              // update seed
    T2AudioBceWriteU64(m + 37, 0x40E7700000000000ull); // 48000

    check("incoming header parses",
          T2AudioBceParseIncoming(m, sizeof(m), &type, &msg, &dev) &&
          type == 1 && msg == T2AUDIO_MSG_UPDATE_TIMESTAMP && dev == 0x39);
    check("incoming too short -> fail",
          !T2AudioBceParseIncoming(m, 20, &type, &msg, &dev));
    check("UPDATE_TIMESTAMP parses",
          T2AudioBceParseUpdateTimestamp(m, sizeof(m), &dev, &ts, &seed, &bits) &&
          dev == 0x39 && ts == 123456789ull && seed == 1 &&
          bits == 0x40E7700000000000ull);
    check("UPDATE_TIMESTAMP truncated -> fail",
          !T2AudioBceParseUpdateTimestamp(m, sizeof(m) - 1, &dev, &ts, &seed, &bits));
    T2AudioBceWriteU32(m + 13, T2AUDIO_MSG_PROPERTY_CHANGED);
    check("other message id is not a timestamp",
          !T2AudioBceParseUpdateTimestamp(m, sizeof(m), &dev, &ts, &seed, &bits));

    check("sampleTime 48000 -> 48000",
          T2AudioBceDecodeSampleTime(0x40E7700000000000ull, &frames) && frames == 48000);
    check("sampleTime 0 -> 0",
          T2AudioBceDecodeSampleTime(0, &frames) && frames == 0);
    check("sampleTime 1.0 -> 1",
          T2AudioBceDecodeSampleTime(0x3FF0000000000000ull, &frames) && frames == 1);
    check("sampleTime 2^40 -> 2^40",
          T2AudioBceDecodeSampleTime(0x4270000000000000ull, &frames) && frames == (1ull << 40));
    check("sampleTime 2^53 -> 2^53",
          T2AudioBceDecodeSampleTime(0x4340000000000000ull, &frames) && frames == (1ull << 53));
    check("sampleTime above 2^53 -> fail",
          !T2AudioBceDecodeSampleTime(0x4340000000000001ull, &frames));
    check("sampleTime 0.5 -> fail",
          !T2AudioBceDecodeSampleTime(0x3FE0000000000000ull, &frames));
    check("sampleTime negative -> fail",
          !T2AudioBceDecodeSampleTime(0xBFF0000000000000ull, &frames));
}

static void test_tags(void)
{
    unsigned long c = 0;
    unsigned long t;
    unsigned char r[8];
    int i, ok = 1;

    printf("T2AudioBceNextTag / ReplyTagMatches\n");

    check("first tag from 0 is 1", T2AudioBceNextTag(&c) == 1);
    check("then 2", T2AudioBceNextTag(&c) == 2);
    c = 126;
    check("126 -> 127", T2AudioBceNextTag(&c) == 127);
    check("127 wraps to 1 (never 0)", T2AudioBceNextTag(&c) == 1);
    c = 0x74647541ul; // "Audt" left over from an old build
    t = T2AudioBceNextTag(&c);
    check("any stale counter lands in 1..127", t >= 1 && t <= 127);
    for (i = 0, c = 0; i < 1000; i++) {
        t = T2AudioBceNextTag(&c);
        if (t < 1 || t > 127) {
            ok = 0;
        }
    }
    check("1000 tags all in 1..127", ok);
    check("NULL counter -> 1", T2AudioBceNextTag(0) == 1);

    T2AudioBceWriteU32(r, 17);
    check("reply tag matches", T2AudioBceReplyTagMatches(r, sizeof(r), 17));
    check("reply tag differs", !T2AudioBceReplyTagMatches(r, sizeof(r), 18));
    check("reply too short", !T2AudioBceReplyTagMatches(r, 3, 17));
    check("NULL reply", !T2AudioBceReplyTagMatches(0, 8, 17));
}

static void test_property_events(void)
{
    unsigned char m[64];
    unsigned char r[64];
    unsigned long long dev = 0, obj = 0;
    unsigned long el = 1, scope = 0, sel = 0;
    unsigned long n;
    unsigned long i;

    printf("Property listener / PROPERTY_CHANGED\n");
    // PROPERTY_CHANGED for 'outp'/'jack' on device 0x25 (a NOTIFICATION).
    for (i = 0; i < sizeof(m); i++) m[i] = 0;
    m[4] = 3;
    T2AudioBceWriteU64(m + 5, 0x25);
    T2AudioBceWriteU32(m + 13, T2AUDIO_MSG_PROPERTY_CHANGED);
    T2AudioBceWriteU64(m + 21, 0x25);
    T2AudioBceWriteU32(m + 29, 0);
    T2AudioBceWriteU32(m + 33, 0x6f757470u);
    T2AudioBceWriteU32(m + 37, 0x6a61636bu);
    check("PROPERTY_CHANGED parsed",
          T2AudioBceParsePropertyChanged(m, 41, &dev, &obj, &el, &scope, &sel) &&
          dev == 0x25 && obj == 0x25 && el == 0 &&
          scope == 0x6f757470u && sel == 0x6a61636bu);
    check("scope at message offset 0x21 (as AppleAudio reads it)",
          T2AudioBceReadU32(m + 0x21) == 0x6f757470u);
    check("PROPERTY_CHANGED truncated -> fail",
          !T2AudioBceParsePropertyChanged(m, 40, &dev, &obj, &el, &scope, &sel));
    T2AudioBceWriteU32(m + 13, T2AUDIO_MSG_UPDATE_TIMESTAMP);
    check("other message id -> fail",
          !T2AudioBceParsePropertyChanged(m, 41, &dev, &obj, &el, &scope, &sel));
    check("NULL output -> fail",
          !T2AudioBceParsePropertyChanged(m, 41, 0, &obj, &el, &scope, &sel));

    n = T2AudioBceBuildPropertyListener(m, 0x25, 0x25, 0x6f757470u, 0x6a61636bu);
    check("listener size 41", n == 41 && T2AUDIO_BCE_PROPERTY_LISTENER_SIZE == 41u);
    check("listener layout: type 1, dev, msg 11, obj, element 0, scope, selector",
          m[4] == 1 && T2AudioBceReadU64(m + 5) == 0x25 &&
          T2AudioBceReadU32(m + 13) == 11 && T2AudioBceReadU32(m + 17) == 0 &&
          T2AudioBceReadU64(m + 21) == 0x25 && T2AudioBceReadU32(m + 29) == 0 &&
          T2AudioBceReadU32(m + 33) == 0x6f757470u && T2AudioBceReadU32(m + 37) == 0x6a61636bu &&
          T2AudioBceReadU32(m) == 0);
    check("listener NULL buffer -> 0", T2AudioBceBuildPropertyListener(0, 1, 1, 1, 1) == 0);

    for (i = 0; i < sizeof(r); i++) r[i] = 0;
    r[4] = 2;
    T2AudioBceWriteU32(r + 13, T2AUDIO_MSG_PROPERTY_LISTENER_RESPONSE);
    T2AudioBceWriteU64(r + 21, 0x25);
    T2AudioBceWriteU32(r + 33, 0x6f757470u);
    T2AudioBceWriteU32(r + 37, 0x6a61636bu);
    check("listener reply ok",
          T2AudioBceParsePropertyListenerResponse(r, 41, 0x25, 0x6f757470u, 0x6a61636bu));
    check("listener reply wrong selector -> fail",
          !T2AudioBceParsePropertyListenerResponse(r, 41, 0x25, 0x6f757470u, 0x75696420u));
    check("listener reply short -> fail",
          !T2AudioBceParsePropertyListenerResponse(r, 40, 0x25, 0x6f757470u, 0x6a61636bu));
    T2AudioBceWriteU32(r + 17, 5);
    check("listener reply status != 0 -> fail",
          !T2AudioBceParsePropertyListenerResponse(r, 41, 0x25, 0x6f757470u, 0x6a61636bu));
}

static void test_set_property_nominal_sample_rate(void)
{
    unsigned char buf[128];
    unsigned long n;
    unsigned long i;
    int rest_zero = 1;
    union {
        double d;
        unsigned long long u;
    } rate;

    printf("T2AudioBceBuildSetPropertyNominalSampleRate\n");
    for (i = 0; i < sizeof(buf); i++) buf[i] = 0xAA;
    n = T2AudioBceBuildSetPropertyNominalSampleRate(buf, sizeof(buf), 0x21, 48000);
    check("size 0x42 (AppleAudio)", n == 0x42);
    check("type COMMAND", buf[4] == 1);
    check("device id 0x21", T2AudioBceReadU64(buf + 5) == 0x21);
    check("msg 9 (SET_PROPERTY)", T2AudioBceReadU32(buf + 13) == 9);
    check("status 0", T2AudioBceReadU32(buf + 17) == 0);
    check("object 0x21 at +0x15", T2AudioBceReadU64(buf + 0x15) == 0x21);
    check("element 0 at +0x1D", T2AudioBceReadU32(buf + 0x1D) == 0);
    check("scope 'glob' at +0x21", T2AudioBceReadU32(buf + 0x21) == 0x676c6f62u);
    check("selector 'nsrt' at +0x25", T2AudioBceReadU32(buf + 0x25) == 0x6e737274u);
    check("datasize 8 at +0x29", T2AudioBceReadU64(buf + 0x29) == 8);
    rate.u = T2AudioBceReadU64(buf + 0x31);
    check("rate 48000 at +0x31", rate.d == 48000);
    for (i = 0x39; i < 0x42; i++) if (buf[i] != 0) rest_zero = 0;
    check("reserved zero at +0x39..+0x41", rest_zero);
    check("nothing past 0x42", buf[0x42] == 0xAA);
    check("too small refused", T2AudioBceBuildSetPropertyNominalSampleRate(buf, 0x41, 0x21, 48000) == 0);
    check("device 0 refused", T2AudioBceBuildSetPropertyNominalSampleRate(buf, sizeof(buf), 0, 48000) == 0);
}

static void test_set_input_ranges(void)
{
    unsigned char buf[T2AUDIO_BCE_SET_INPUT_RANGES_SIZE + 4];
    unsigned long n, i;
    int rest_zero = 1;

    printf("T2AudioBceBuildSetInputRanges\n");
    for (i = 0; i < sizeof(buf); i++) buf[i] = 0xEE;
    n = T2AudioBceBuildSetInputRanges(buf, sizeof(buf), 0x21, 0x22);
    check("size 0x345 (AppleAudio)", n == 0x345);
    check("type COMMAND", buf[4] == 1);
    check("device id 0x21", T2AudioBceReadU64(buf + 5) == 0x21);
    check("msg 18", T2AudioBceReadU32(buf + 13) == 18);
    check("status 0", T2AudioBceReadU32(buf + 17) == 0);
    check("stream id at +0x15", T2AudioBceReadU64(buf + 0x15) == 0x22);
    check("count 1 at +0x1D", T2AudioBceReadU64(buf + 0x1D) == 1);
    check("1 at +0x25", T2AudioBceReadU64(buf + 0x25) == 1);
    check("1 at +0x2D", T2AudioBceReadU64(buf + 0x2D) == 1);
    for (i = 0x35; i < 0x345; i++) if (buf[i] != 0) rest_zero = 0;
    check("rest zero", rest_zero);
    check("nothing past 0x345", buf[0x345] == 0xEE);
    check("too small refused", T2AudioBceBuildSetInputRanges(buf, 0x344, 0x21, 0x22) == 0);
    check("stream 0 refused", T2AudioBceBuildSetInputRanges(buf, sizeof(buf), 0x21, 0) == 0);
}

int main(void)
{
    test_set_input_ranges();
    test_set_property_nominal_sample_rate();
    test_scalar_roundtrip();
    test_device_list();
    test_property_offset();
    test_uid_match();
    test_parse_device_list_response();
    test_parse_property_response();
    test_parse_command_response();
    test_physical_format();
    test_incoming();
    test_tags();
    test_property_events();

    printf("\n%s (%d failure%s)\n",
           g_failures ? "FAILED" : "ALL PASSED",
           g_failures,
           g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
