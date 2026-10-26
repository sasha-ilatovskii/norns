#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sharp_memory_dither.h"

bool sharp_memory_map_target_to_source(const sharp_memory_viewport_t *viewport,
                                       uint32_t source_width,
                                       uint32_t source_height,
                                       int target_x,
                                       int target_y,
                                       uint32_t *source_x,
                                       uint32_t *source_y);

bool sharp_memory_text_outline_is_white(const uint8_t *source_data,
                                        int source_stride,
                                        const uint8_t *mask_data,
                                        int mask_stride,
                                        const sharp_memory_viewport_t *viewport,
                                        uint32_t source_width,
                                        uint32_t source_height,
                                        int target_x,
                                        int target_y,
                                        uint8_t text_luminance,
                                        uint8_t threshold,
                                        bool *is_outline);
