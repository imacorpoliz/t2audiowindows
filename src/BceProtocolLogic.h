#ifndef T2AUDIO_BCE_PROTOCOL_LOGIC_H
#define T2AUDIO_BCE_PROTOCOL_LOGIC_H

// Pure, dependency-free helpers for the T2 BCE control protocol. Kept free of
// kernel APIs (only unsigned char / unsigned long / unsigned long long, which
// match UCHAR / ULONG / ULONG64 on Windows) so the same code can be compiled
// and exercised by a host-side test (tests/BceProtocolLogicTest.c).
//
// Wire layout (little-endian, from kaiT2en modules/t2bce_audio/protocol.h):
//
//   header : tag[4] (char), type (u8), device_id (u64)   -> 13 bytes
//   base   : msg (u32), status (u32)                     ->  8 bytes
//
//   GET_DEVICE_LIST response payload:
//     count (u64) then count * device_id (u64)
//
//   GET_PROPERTY response payload:
//     obj (u64), element (u32), scope (u32), selector (u32),
//     data_size (u64), then data_size bytes

#define T2AUDIO_BCE_HEADER_SIZE      13u
#define T2AUDIO_BCE_BASE_SIZE        8u
#define T2AUDIO_BCE_U64_SIZE         8u
#define T2AUDIO_BCE_U32_SIZE         4u

// GET_PROPERTY response prefix after the base: obj(8)+element(4)+scope(4)+
// selector(4)+data_size(8).
#define T2AUDIO_BCE_PROP_FIXED_SIZE  28u

// Offset of the type byte within the header, and the two type values.
#define T2AUDIO_BCE_TYPE_OFFSET      4u
#define T2AUDIO_BCE_MSG_COMMAND      1u
#define T2AUDIO_BCE_MSG_RESPONSE     2u

// Alignment-free little-endian scalar accessors.
static __inline unsigned long long
T2AudioBceReadU64(const unsigned char *p)
{
    unsigned long long v = 0;
    int i;
    for (i = 7; i >= 0; i--) {
        v = (v << 8) | (unsigned long long)p[i];
    }
    return v;
}

static __inline unsigned long
T2AudioBceReadU32(const unsigned char *p)
{
    return (unsigned long)p[0] |
           ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) |
           ((unsigned long)p[3] << 24);
}

static __inline void
T2AudioBceWriteU64(unsigned char *p, unsigned long long v)
{
    int i;
    for (i = 0; i < 8; i++) {
        p[i] = (unsigned char)(v >> (8 * i));
    }
}

static __inline void
T2AudioBceWriteU32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

// Validate a GET_DEVICE_LIST response and compute how many device ids may be
// copied. ReplySize is the number of bytes actually returned. Returns 1 and
// writes *OutCount (clamped to MaxDevices) when the count and array fit within
// ReplySize, otherwise 0 (truncated or inconsistent reply).
static __inline int
T2AudioBceDeviceListCount(unsigned long long ReplySize,
                          unsigned long long Count,
                          unsigned long MaxDevices,
                          unsigned long *OutCount)
{
    unsigned long long fixed =
        T2AUDIO_BCE_HEADER_SIZE + T2AUDIO_BCE_BASE_SIZE + T2AUDIO_BCE_U64_SIZE;
    unsigned long long maxBySize;

    if (OutCount == 0) {
        return 0;
    }
    if (ReplySize < fixed) {
        return 0;
    }

    maxBySize = (ReplySize - fixed) / T2AUDIO_BCE_U64_SIZE;
    if (Count > maxBySize) {
        return 0;
    }
    if (Count > MaxDevices) {
        Count = MaxDevices;
    }

    *OutCount = (unsigned long)Count;
    return 1;
}

// Parse and validate a complete GET_DEVICE_LIST response. Requires the
// response type byte to be RESPONSE, the protocol status to be zero, the
// message id to equal ExpectedMessage, and the advertised count to fit within
// ReplySize. On success copies up to MaxDevices ids into OutIds (which may be
// 0 only when the count is 0) and writes the copied count to *OutCount.
static __inline int
T2AudioBceParseDeviceListResponse(const unsigned char *Reply,
                                  unsigned long long ReplySize,
                                  unsigned long ExpectedMessage,
                                  unsigned long long *OutIds,
                                  unsigned long MaxDevices,
                                  unsigned long *OutCount)
{
    unsigned long long base =
        T2AUDIO_BCE_HEADER_SIZE + T2AUDIO_BCE_BASE_SIZE;
    unsigned long long count;
    unsigned long out;
    unsigned long i;

    if (Reply == 0 || OutCount == 0) {
        return 0;
    }
    if (ReplySize < base + T2AUDIO_BCE_U64_SIZE) {
        return 0;
    }
    if (Reply[T2AUDIO_BCE_TYPE_OFFSET] != T2AUDIO_BCE_MSG_RESPONSE) {
        return 0;
    }
    if (T2AudioBceReadU32(Reply + T2AUDIO_BCE_HEADER_SIZE) != ExpectedMessage) {
        return 0;
    }
    if (T2AudioBceReadU32(Reply + T2AUDIO_BCE_HEADER_SIZE +
                          T2AUDIO_BCE_U32_SIZE) != 0) {
        return 0;
    }

    count = T2AudioBceReadU64(Reply + base);
    if (!T2AudioBceDeviceListCount(ReplySize, count, MaxDevices, &out)) {
        return 0;
    }
    if (out != 0 && OutIds == 0) {
        return 0;
    }

    for (i = 0; i < out; i++) {
        OutIds[i] = T2AudioBceReadU64(
            Reply + base + T2AUDIO_BCE_U64_SIZE +
            (unsigned long long)i * T2AUDIO_BCE_U64_SIZE);
    }

    *OutCount = out;
    return 1;
}

// Validate a GET_PROPERTY response and locate its data. Returns 1 and writes
// the byte offset of the data (and its size via *OutDataSize) when the fixed
// prefix plus DataSize fits within ReplySize, otherwise 0.
static __inline int
T2AudioBcePropertyDataOffset(unsigned long long ReplySize,
                             unsigned long long DataSize,
                             unsigned long long *OutOffset,
                             unsigned long long *OutDataSize)
{
    unsigned long long fixed = T2AUDIO_BCE_HEADER_SIZE +
                               T2AUDIO_BCE_BASE_SIZE +
                               T2AUDIO_BCE_PROP_FIXED_SIZE;

    if (OutOffset == 0 || OutDataSize == 0) {
        return 0;
    }
    if (ReplySize < fixed) {
        return 0;
    }
    if (DataSize > ReplySize - fixed) {
        return 0;
    }

    *OutOffset = fixed;
    *OutDataSize = DataSize;
    return 1;
}

// Parse and validate a complete GET_PROPERTY response. Requires the response
// type byte to be RESPONSE, the protocol status to be zero, the message id to
// equal ExpectedMessage, and the fixed prefix (obj/element/scope/selector/
// data_size) plus the advertised data to fit within ReplySize. On success
// writes every parsed field. The caller is expected to verify that the echoed
// obj/scope/selector match what it requested.
static __inline int
T2AudioBceParsePropertyResponse(const unsigned char *Reply,
                                unsigned long long ReplySize,
                                unsigned long ExpectedMessage,
                                unsigned long long *OutObj,
                                unsigned long *OutElement,
                                unsigned long *OutScope,
                                unsigned long *OutSelector,
                                unsigned long long *OutDataOffset,
                                unsigned long long *OutDataSize)
{
    unsigned long long base =
        T2AUDIO_BCE_HEADER_SIZE + T2AUDIO_BCE_BASE_SIZE;
    unsigned long long fixed = base + T2AUDIO_BCE_PROP_FIXED_SIZE;
    unsigned long long dataSize;

    if (Reply == 0 || OutObj == 0 || OutElement == 0 || OutScope == 0 ||
        OutSelector == 0 || OutDataOffset == 0 || OutDataSize == 0) {
        return 0;
    }
    if (ReplySize < fixed) {
        return 0;
    }
    if (Reply[T2AUDIO_BCE_TYPE_OFFSET] != T2AUDIO_BCE_MSG_RESPONSE) {
        return 0;
    }
    if (T2AudioBceReadU32(Reply + T2AUDIO_BCE_HEADER_SIZE) != ExpectedMessage) {
        return 0;
    }
    if (T2AudioBceReadU32(Reply + T2AUDIO_BCE_HEADER_SIZE +
                          T2AUDIO_BCE_U32_SIZE) != 0) {
        return 0;
    }

    *OutObj = T2AudioBceReadU64(Reply + base);
    *OutElement = T2AudioBceReadU32(Reply + base + 8);
    *OutScope = T2AudioBceReadU32(Reply + base + 12);
    *OutSelector = T2AudioBceReadU32(Reply + base + 16);
    dataSize = T2AudioBceReadU64(Reply + base + 20);

    if (!T2AudioBcePropertyDataOffset(ReplySize, dataSize,
                                      OutDataOffset, OutDataSize)) {
        return 0;
    }

    return 1;
}

// Exact, case-sensitive match of a device UID against "Speaker", as kaiT2en
// does with strcmp() against the BufferStruct device name.
static __inline int
T2AudioBceUidIsSpeaker(const char *Uid, unsigned long long UidLength)
{
    static const char kSpeaker[] = "Speaker";
    unsigned long long i;

    if (Uid == 0 || UidLength != sizeof(kSpeaker) - 1) {
        return 0;
    }
    for (i = 0; i < UidLength; i++) {
        if (Uid[i] != kSpeaker[i]) {
            return 0;
        }
    }
    return 1;
}

#endif
