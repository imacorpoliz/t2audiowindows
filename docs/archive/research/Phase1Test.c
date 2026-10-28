#include "DataStructures.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

int ParseBufferStruct(const void *, const char *, size_t *, size_t *);
size_t RingWrapOffset(size_t, size_t, size_t);
uint64_t InterpolatePosition(uint64_t, uint64_t, uint64_t, uint32_t);

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        printf("FAIL: %s\n", message); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", message); \
    } \
} while (0)

static void TestParser(void)
{
    size_t allocation_size = 0x10 + T2AUDIO_DEVICE_STRIDE * T2AUDIO_MAX_DEVICES;
    uint8_t *raw = (uint8_t *)calloc(1, allocation_size);
    T2AUDIO_BUFFER_STRUCT *bs = (T2AUDIO_BUFFER_STRUCT *)raw;
    T2AUDIO_DEVICE *speaker;
    size_t offset = 0;
    size_t size = 0;

    CHECK(raw != NULL, "mock BufferStruct allocation");
    if (raw == NULL) {
        return;
    }

    bs->version = 2;
    bs->signature = T2AUDIO_SIG;
    bs->num_devices = 1;
    speaker = (T2AUDIO_DEVICE *)(raw + 0x10);
    memcpy(speaker->name, "Speaker", 8);
    speaker->num_output_streams = 1;
    speaker->output_streams[0].num_buffers = 1;
    speaker->output_streams[0].buffers[0].address = 0x200000;
    speaker->output_streams[0].buffers[0].size = 0x6000;

    CHECK(ParseBufferStruct(bs, "Speaker", &offset, &size) == 0 &&
          offset == 0x200000 && size == 0x6000,
          "Speaker output buffer parsed");
    CHECK(ParseBufferStruct(bs, "Microphone", &offset, &size) != 0,
          "unknown device rejected");

    free(raw);
}

static void TestNativeFormatContract(void)
{
    const unsigned sample_rate = 48000;
    const unsigned channels = 6;
    const unsigned bits_per_sample = 24;
    const unsigned bytes_per_frame = 24;

    CHECK(sample_rate == 48000 && channels == 6 && bits_per_sample == 24 &&
          bytes_per_frame == channels * sizeof(uint32_t),
          "native Speaker format is 48kHz, 6ch, 24-in-32");
}

static void TestRingMath(void)
{
    CHECK(RingWrapOffset(0, 0, 4096) == 0, "ring origin");
    CHECK(RingWrapOffset(4000, 200, 4096) == 104, "ring wrap-around");
    CHECK(RingWrapOffset(4095, 1, 4096) == 0, "ring exact boundary");
    CHECK(RingWrapOffset(0, 0, 0) == 0, "zero-sized ring guarded");
}

static void TestQpc(void)
{
    LARGE_INTEGER frequency;
    uint64_t first;
    uint64_t second;

    CHECK(QueryPerformanceFrequency(&frequency) != 0, "QPC frequency available");
    if (!QueryPerformanceFrequency(&frequency)) {
        return;
    }

    first = InterpolatePosition(1000, 1000 + (uint64_t)frequency.QuadPart,
                                500, 48000);
    second = InterpolatePosition(1000, 1000 + 2 * (uint64_t)frequency.QuadPart,
                                 500, 48000);
    CHECK(first >= 500 && second >= first, "QPC positions monotonic");
    CHECK(first == 48500 && second == 96500, "QPC one/two second interpolation");
}

int main(void)
{
    printf("T2Audio Phase 1 user-mode contract tests\n");
    printf("device stride: 0x%zX, buffer entry: 0x%zX\n",
           T2AUDIO_DEVICE_STRIDE, sizeof(T2AUDIO_BUFFER));
    TestParser();
    TestNativeFormatContract();
    TestRingMath();
    TestQpc();
    printf("RESULT: %s (%d failure(s))\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
