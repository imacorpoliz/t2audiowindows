#include <stddef.h>
#include <stdint.h>
#include <windows.h>

size_t RingWrapOffset(size_t base_offset, size_t write_offset, size_t buffer_size)
{
    if (buffer_size == 0) {
        return 0;
    }
    return (base_offset + write_offset) % buffer_size;
}

uint64_t InterpolatePosition(uint64_t anchor_qpc,
                             uint64_t current_qpc,
                             uint64_t anchor_frames,
                             uint32_t sample_rate)
{
    LARGE_INTEGER frequency;
    uint64_t qpc_delta;

    if (current_qpc <= anchor_qpc || sample_rate == 0 ||
        !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        return anchor_frames;
    }

    qpc_delta = current_qpc - anchor_qpc;
    return anchor_frames +
           (uint64_t)((long double)qpc_delta * sample_rate /
                      (long double)frequency.QuadPart);
}
