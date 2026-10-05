#include "DataStructures.h"

#include <string.h>

enum {
    T2AUDIO_OK = 0,
    T2AUDIO_PARSE_ERROR = -1
};

static int DeviceNameEquals(const char name[128], const char *target)
{
    size_t target_length = strlen(target);

    return target_length < 128 &&
           memcmp(name, target, target_length) == 0 &&
           (name[target_length] == '\0' ||
            (target_length == 127 && name[target_length] != '\0'));
}

int ParseBufferStruct(const void *buffer_struct_base,
                      const char *target_device_name,
                      size_t *out_buffer_offset,
                      size_t *out_buffer_size)
{
    const T2AUDIO_BUFFER_STRUCT *bs;

    if (buffer_struct_base == NULL || target_device_name == NULL ||
        out_buffer_offset == NULL || out_buffer_size == NULL) {
        return T2AUDIO_PARSE_ERROR;
    }

    bs = (const T2AUDIO_BUFFER_STRUCT *)buffer_struct_base;
    if (bs->signature != T2AUDIO_SIG || bs->version < 2 ||
        bs->num_devices > T2AUDIO_MAX_DEVICES) {
        return T2AUDIO_PARSE_ERROR;
    }

    for (uint32_t i = 0; i < bs->num_devices; ++i) {
        const T2AUDIO_DEVICE *device =
            (const T2AUDIO_DEVICE *)((const uint8_t *)bs + 0x10 +
                                     ((size_t)i * T2AUDIO_DEVICE_STRIDE));

        if (!DeviceNameEquals(device->name, target_device_name) ||
            device->num_output_streams == 0 ||
            device->num_output_streams > T2AUDIO_MAX_STREAMS) {
            continue;
        }

        {
            const T2AUDIO_STREAM *stream = &device->output_streams[0];
            if (stream->num_buffers == 0 || stream->num_buffers > T2AUDIO_MAX_BUFFERS) {
                continue;
            }

            *out_buffer_offset = stream->buffers[0].address;
            *out_buffer_size = stream->buffers[0].size;
            return T2AUDIO_OK;
        }
    }

    return T2AUDIO_PARSE_ERROR;
}
