#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct _sharp_memory_viewport {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} sharp_memory_viewport_t;

uint8_t sharp_memory_luminance(uint32_t argb);
bool sharp_memory_dither_is_white(uint8_t luminance, uint32_t x, uint32_t y, bool invert);
sharp_memory_viewport_t sharp_memory_fit_viewport(uint32_t source_width,
                                                  uint32_t source_height,
                                                  uint32_t target_width,
                                                  uint32_t target_height,
                                                  bool stretch);
