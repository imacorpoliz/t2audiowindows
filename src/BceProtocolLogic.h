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

// Protocol message IDs and responses (verified against AppleAudio.sys reverse engineering)
#define T2AUDIO_MSG_START_IO                  0u
#define T2AUDIO_MSG_START_IO_RESPONSE         1u
#define T2AUDIO_MSG_UPDATE_TIMESTAMP          4u
#define T2AUDIO_MSG_PROPERTY_CHANGED          13u
#define T2AUDIO_MSG_STOP_IO                   2u
#define T2AUDIO_MSG_STOP_IO_RESPONSE          3u
#define T2AUDIO_MSG_GET_PROPERTY              7u
#define T2AUDIO_MSG_GET_PROPERTY_RESPONSE     8u
#define T2AUDIO_MSG_GET_OUTPUT_STREAM_LIST    26u
#define T2AUDIO_MSG_GET_OUTPUT_STREAM_LIST_RESPONSE 27u
#define T2AUDIO_MSG_SET_REMOTE_ACCESS         32u
#define T2AUDIO_MSG_SET_REMOTE_ACCESS_RESPONSE 33u
#define T2AUDIO_MSG_GET_DEVICE_LIST           101u
#define T2AUDIO_MSG_GET_DEVICE_LIST_RESPONSE  102u

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

// Validate a simple command response (START_IO / STOP_IO and similar), whose
// payload is just the base. Requires: ReplySize covers header+base, tag ==
// "Audt", type == RESPONSE, device_id echoes ExpectedDeviceId, the message id
// equals ExpectedMessage, and the protocol status is zero. Returns 1 on success.
static __inline int
T2AudioBceParseCommandResponse(const unsigned char *Reply,
                               unsigned long long ReplySize,
                               unsigned long ExpectedMessage,
                               unsigned long long ExpectedDeviceId)
{
    if (Reply == 0) {
        return 0;
    }
    if (ReplySize < T2AUDIO_BCE_HEADER_SIZE + T2AUDIO_BCE_BASE_SIZE) {
        return 0;
    }
    if (Reply[0] != 'A' || Reply[1] != 'u' ||
        Reply[2] != 'd' || Reply[3] != 't') {
        return 0;
    }
    if (Reply[T2AUDIO_BCE_TYPE_OFFSET] != T2AUDIO_BCE_MSG_RESPONSE) {
        return 0;
    }
    // device_id sits immediately after the 4-byte tag and the type byte.
    if (T2AudioBceReadU64(Reply + 5) != ExpectedDeviceId) {
        return 0;
    }
    if (T2AudioBceReadU32(Reply + T2AUDIO_BCE_HEADER_SIZE) != ExpectedMessage) {
        return 0;
    }
    if (T2AudioBceReadU32(Reply + T2AUDIO_BCE_HEADER_SIZE +
                          T2AUDIO_BCE_U32_SIZE) != 0) {
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

// Stream physical format ('pft ' property), the 40-byte CoreAudio-style
// descriptor bridgeOS returns for a stream object:
//   sample_rate (IEEE-754 double, u64 bits), format_id (u32, 'lpcm'),
//   format_flags, bytes_per_packet, frames_per_packet, bytes_per_frame,
//   channels_per_frame, bits_per_channel, reserved (all u32).
#define T2AUDIO_BCE_PHYS_FORMAT_SIZE      40u
#define T2AUDIO_BCE_FORMAT_LPCM           0x6c70636du  // 'lpcm'

#define T2AUDIO_BCE_FORMAT_FLAG_FLOAT           0x01u
#define T2AUDIO_BCE_FORMAT_FLAG_BIG_ENDIAN      0x02u
#define T2AUDIO_BCE_FORMAT_FLAG_SIGNED          0x04u
#define T2AUDIO_BCE_FORMAT_FLAG_PACKED          0x08u
#define T2AUDIO_BCE_FORMAT_FLAG_ALIGNED_HIGH    0x10u
#define T2AUDIO_BCE_FORMAT_FLAG_NON_INTERLEAVED 0x20u
#define T2AUDIO_BCE_FORMAT_FLAG_NON_MIXABLE     0x40u

typedef struct _T2AUDIO_BCE_PHYS_FORMAT {
    unsigned long SampleRate;        // integral Hz, decoded from the double
    unsigned long FormatId;
    unsigned long FormatFlags;
    unsigned long BytesPerPacket;
    unsigned long FramesPerPacket;
    unsigned long BytesPerFrame;
    unsigned long ChannelsPerFrame;
    unsigned long BitsPerChannel;
} T2AUDIO_BCE_PHYS_FORMAT;

// Decode a nonnegative, integral IEEE-754 double (given as its raw bits)
// without floating point, which kernel code must avoid. Returns 1 and writes
// *Out when the value is an integer in [0, 2^32), otherwise 0.
static __inline int
T2AudioBceDecodeIntegralDouble(unsigned long long Bits, unsigned long *Out)
{
    unsigned int exponent = (unsigned int)((Bits >> 52) & 0x7ffu);
    unsigned long long mantissa = Bits & ((1ull << 52) - 1);
    unsigned int shift;

    if (Out == 0) {
        return 0;
    }
    if (Bits == 0) {
        *Out = 0;
        return 1;
    }
    // Negative, below 1.0, or 2^32 and above.
    if ((Bits >> 63) != 0 || exponent < 1023 || exponent > 1023 + 31) {
        return 0;
    }
    mantissa |= 1ull << 52;
    shift = 1075 - exponent;   // 52 - (exponent - 1023), always 21..52
    if ((mantissa & ((1ull << shift) - 1)) != 0) {
        return 0;              // fractional rate
    }
    *Out = (unsigned long)(mantissa >> shift);
    return 1;
}

// Parse a 'pft ' property payload. Returns 1 on success.
static __inline int
T2AudioBceParsePhysicalFormat(const unsigned char *Data,
                              unsigned long long DataSize,
                              T2AUDIO_BCE_PHYS_FORMAT *Out)
{
    if (Data == 0 || Out == 0 || DataSize < T2AUDIO_BCE_PHYS_FORMAT_SIZE) {
        return 0;
    }
    if (!T2AudioBceDecodeIntegralDouble(T2AudioBceReadU64(Data), &Out->SampleRate)) {
        return 0;
    }
    Out->FormatId         = T2AudioBceReadU32(Data + 8);
    Out->FormatFlags      = T2AudioBceReadU32(Data + 12);
    Out->BytesPerPacket   = T2AudioBceReadU32(Data + 16);
    Out->FramesPerPacket  = T2AudioBceReadU32(Data + 20);
    Out->BytesPerFrame    = T2AudioBceReadU32(Data + 24);
    Out->ChannelsPerFrame = T2AudioBceReadU32(Data + 28);
    Out->BitsPerChannel   = T2AudioBceReadU32(Data + 32);
    return 1;
}

// Decide how a host sample (signed 32-bit, MSB-aligned, as Windows delivers
// 32-bit PCM) must be shifted to land in the device ring for the given
// physical format, matching how kaiT2en maps the descriptor to an ALSA
// format. Only the interleaved, little-endian, signed integer layouts with
// 4-byte containers and the expected channel count / rate are accepted:
//   bits 32                      -> shift 0  (S32_LE)
//   bits 24, ALIGNED_HIGH        -> shift 0  (24 bits in the high bytes)
//   bits 24, not packed          -> shift 8  (S24_LE: low 3 bytes)
// Returns the shift (0 or 8), or -1 if the format is not supported.
static __inline int
T2AudioBceDeviceSampleShift(const T2AUDIO_BCE_PHYS_FORMAT *Format,
                            unsigned long ExpectedChannels,
                            unsigned long ExpectedRate)
{
    unsigned long flags;

    if (Format == 0 || Format->FormatId != T2AUDIO_BCE_FORMAT_LPCM ||
        Format->SampleRate != ExpectedRate ||
        Format->ChannelsPerFrame != ExpectedChannels ||
        Format->FramesPerPacket != 1 ||
        Format->BytesPerFrame != ExpectedChannels * 4u ||
        Format->BytesPerPacket != Format->BytesPerFrame) {
        return -1;
    }
    flags = Format->FormatFlags;
    if ((flags & (T2AUDIO_BCE_FORMAT_FLAG_FLOAT |
                  T2AUDIO_BCE_FORMAT_FLAG_BIG_ENDIAN |
                  T2AUDIO_BCE_FORMAT_FLAG_NON_INTERLEAVED |
                  T2AUDIO_BCE_FORMAT_FLAG_PACKED)) != 0 ||
        (flags & T2AUDIO_BCE_FORMAT_FLAG_SIGNED) == 0) {
        return -1;
    }
    if (Format->BitsPerChannel == 32) {
        return 0;
    }
    if (Format->BitsPerChannel == 24) {
        return (flags & T2AUDIO_BCE_FORMAT_FLAG_ALIGNED_HIGH) ? 0 : 8;
    }
    return -1;
}

// Messages bridgeOS sends to the host on its own (UPDATE_TIMESTAMP after
// START_IO, PROPERTY_CHANGED for jacks, ...). On Windows AppleUSBVHCI
// delivers each one as IOCTL 0x222020 to the first KSCATEGORY_AUDIO interface
// whose path contains VEN_106B - i.e. to this driver once it is installed -
// with the raw message (same header/base layout) as the input buffer.

// Validate the fixed part of an incoming message and return its type, message
// id and device id. Returns 1 when MessageSize covers header + base.
static __inline int
T2AudioBceParseIncoming(const unsigned char *Message,
                        unsigned long long MessageSize,
                        unsigned long *OutType,
                        unsigned long *OutMessage,
                        unsigned long long *OutDeviceId)
{
    if (Message == 0 || OutType == 0 || OutMessage == 0 || OutDeviceId == 0 ||
        MessageSize < T2AUDIO_BCE_HEADER_SIZE + T2AUDIO_BCE_BASE_SIZE) {
        return 0;
    }
    *OutType = Message[T2AUDIO_BCE_TYPE_OFFSET];
    *OutDeviceId = T2AudioBceReadU64(Message + 5);
    *OutMessage = T2AudioBceReadU32(Message + T2AUDIO_BCE_HEADER_SIZE);
    return 1;
}

// UPDATE_TIMESTAMP payload (kaiT2en protocol.c, matches what AppleAudio.sys
// reads at message offset 0x15): timestamp u64, update_seed u64, then
// sampleTime as an IEEE-754 double (raw bits). Returns 1 on success.
static __inline int
T2AudioBceParseUpdateTimestamp(const unsigned char *Message,
                               unsigned long long MessageSize,
                               unsigned long long *OutDeviceId,
                               unsigned long long *OutTimestamp,
                               unsigned long long *OutUpdateSeed,
                               unsigned long long *OutSampleTimeBits)
{
    unsigned long long base = T2AUDIO_BCE_HEADER_SIZE + T2AUDIO_BCE_BASE_SIZE;

    if (Message == 0 || OutDeviceId == 0 || OutTimestamp == 0 ||
        OutUpdateSeed == 0 || OutSampleTimeBits == 0 ||
        MessageSize < base + 3 * T2AUDIO_BCE_U64_SIZE ||
        T2AudioBceReadU32(Message + T2AUDIO_BCE_HEADER_SIZE) !=
            T2AUDIO_MSG_UPDATE_TIMESTAMP) {
        return 0;
    }
    *OutDeviceId = T2AudioBceReadU64(Message + 5);
    *OutTimestamp = T2AudioBceReadU64(Message + base);
    *OutUpdateSeed = T2AudioBceReadU64(Message + base + 8);
    *OutSampleTimeBits = T2AudioBceReadU64(Message + base + 16);
    return 1;
}

// Decode a nonnegative, integral IEEE-754 double (raw bits) up to 2^53 into a
// 64-bit integer without floating point (sampleTime is a frame count).
static __inline int
T2AudioBceDecodeSampleTime(unsigned long long Bits, unsigned long long *OutFrames)
{
    unsigned int exponent = (unsigned int)((Bits >> 52) & 0x7ffu);
    unsigned long long mantissa = Bits & ((1ull << 52) - 1);
    unsigned int shift;

    if (OutFrames == 0) {
        return 0;
    }
    if (Bits == 0) {
        *OutFrames = 0;
        return 1;
    }
    if ((Bits >> 63) != 0 || exponent < 1023 || exponent > 1076 ||
        (exponent == 1076 && mantissa != 0)) {
        return 0;
    }
    mantissa |= 1ull << 52;
    if (exponent == 1076) {
        *OutFrames = mantissa << 1;   // exactly 2^53
        return 1;
    }
    shift = 1075 - exponent;          // 0..52
    if (shift != 0 && (mantissa & ((1ull << shift) - 1)) != 0) {
        return 0;
    }
    *OutFrames = mantissa >> shift;
    return 1;
}

#endif
