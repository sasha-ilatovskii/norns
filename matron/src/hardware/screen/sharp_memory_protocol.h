#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SHARP_MEMORY_WRITE_COMMAND 0x01
#define SHARP_MEMORY_VCOM_BIT 0x02
#define SHARP_MEMORY_CLEAR_COMMAND 0x04

uint8_t sharp_memory_reverse_byte(uint8_t value);

size_t sharp_memory_encoded_rows_size(uint32_t width, uint32_t row_count);

size_t sharp_memory_encode_rows(uint8_t *output,
                                size_t output_size,
                                const uint8_t *frame,
                                uint32_t width,
                                uint32_t first_row,
                                uint32_t row_count,
                                bool vcom);

size_t sharp_memory_encode_command(uint8_t *output,
                                   size_t output_size,
                                   uint8_t command,
                                   bool vcom);
