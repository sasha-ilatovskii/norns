#include "sharp_memory_protocol.h"

uint8_t sharp_memory_reverse_byte(uint8_t value) {
    value = (uint8_t)((value >> 4) | (value << 4));
    value = (uint8_t)(((value & 0xcc) >> 2) | ((value & 0x33) << 2));
    return (uint8_t)(((value & 0xaa) >> 1) | ((value & 0x55) << 1));
}

size_t sharp_memory_encoded_rows_size(uint32_t width, uint32_t row_count) {
    if (width == 0 || (width & 7) != 0 || row_count == 0)
        return 0;

    const size_t bytes_per_row = width / 8;
    return 1 + (size_t)row_count * (bytes_per_row + 2) + 1;
}

size_t sharp_memory_encode_rows(uint8_t *output,
                                size_t output_size,
                                const uint8_t *frame,
                                uint32_t width,
                                uint32_t first_row,
                                uint32_t row_count,
                                bool vcom) {
    const size_t encoded_size = sharp_memory_encoded_rows_size(width, row_count);
    if (output == NULL || frame == NULL || encoded_size == 0 || output_size < encoded_size ||
        first_row >= 255 || row_count > 255 - first_row)
        return 0;

    const size_t bytes_per_row = width / 8;
    size_t offset = 0;
    output[offset++] = sharp_memory_reverse_byte(
        (uint8_t)(SHARP_MEMORY_WRITE_COMMAND | (vcom ? SHARP_MEMORY_VCOM_BIT : 0)));

    for (uint32_t row = first_row; row < first_row + row_count; ++row) {
        output[offset++] = sharp_memory_reverse_byte((uint8_t)(row + 1));
        const uint8_t *source = frame + (size_t)row * bytes_per_row;
        for (size_t column = 0; column < bytes_per_row; ++column)
            output[offset++] = sharp_memory_reverse_byte(source[column]);
        output[offset++] = 0;
    }

    output[offset++] = 0;
    return offset;
}

size_t sharp_memory_encode_command(uint8_t *output,
                                   size_t output_size,
                                   uint8_t command,
                                   bool vcom) {
    if (output == NULL || output_size < 2)
        return 0;

    output[0] = sharp_memory_reverse_byte(
        (uint8_t)(command | (vcom ? SHARP_MEMORY_VCOM_BIT : 0)));
    output[1] = 0;
    return 2;
}
