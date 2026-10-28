#ifndef T2_AUDIO_PHASE1_DATA_STRUCTURES_H
#define T2_AUDIO_PHASE1_DATA_STRUCTURES_H

#include <stddef.h>
#include <stdint.h>

#define T2AUDIO_SIG UINT32_C(0x19870423)
#define T2AUDIO_DEVICE_STRIDE ((size_t)0xBDEC)
#define T2AUDIO_MAX_DEVICES 20u
#define T2AUDIO_MAX_STREAMS 5u
#define T2AUDIO_MAX_BUFFERS 100u

/* The Linux ABI uses four-byte alignment for the complete wire structure. */
#if defined(_MSC_VER)
#pragma pack(push, 4)
#endif
typedef struct t2audio_buffer_struct_buffer {
    size_t address;
    size_t size;
    size_t pad[4];
} T2AUDIO_BUFFER;
typedef struct t2audio_buffer_struct_stream {
    uint8_t num_buffers;
    T2AUDIO_BUFFER buffers[T2AUDIO_MAX_BUFFERS];
    char filler[32];
} T2AUDIO_STREAM;

typedef struct t2audio_buffer_struct_device {
    char name[128];
    uint8_t num_input_streams;
    uint8_t num_output_streams;
    T2AUDIO_STREAM input_streams[T2AUDIO_MAX_STREAMS];
    T2AUDIO_STREAM output_streams[T2AUDIO_MAX_STREAMS];
    char filler[128];
} T2AUDIO_DEVICE;

typedef struct t2audio_buffer_struct {
    uint32_t version;
    uint32_t signature;
    uint32_t flags;
    uint8_t num_devices;
    T2AUDIO_DEVICE devices[T2AUDIO_MAX_DEVICES];
} T2AUDIO_BUFFER_STRUCT;

#if defined(_MSC_VER)
#pragma pack(pop)
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(T2AUDIO_BUFFER) == 0x30, "T2AUDIO_BUFFER ABI mismatch");
_Static_assert(offsetof(T2AUDIO_BUFFER_STRUCT, devices) == 0x10,
               "BufferStruct device offset mismatch");
#if SIZE_MAX == UINT64_MAX
_Static_assert(sizeof(size_t) == 8, "Phase 1 requires a 64-bit target");
_Static_assert(sizeof(T2AUDIO_STREAM) == 0x12E4, "T2AUDIO_STREAM ABI mismatch");
_Static_assert(sizeof(T2AUDIO_DEVICE) == T2AUDIO_DEVICE_STRIDE,
               "T2AUDIO_DEVICE ABI mismatch");
#endif
#endif

#endif
