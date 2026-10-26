#include "sharp_memory_dither.h"

#include <stddef.h>

// 8x8 Bayer matrix. Ordered dithering is deterministic, so static norns UI
// elements do not shimmer from one display update to the next.
static const uint8_t bayer8[64] = {
    0,
    48,
    12,
    60,
    3,
    51,
    15,
    63,
    32,
    16,
    44,
    28,
    35,
    19,
    47,
    31,
    8,
    56,
    4,
    52,
    11,
    59,
    7,
    55,
    40,
    24,
    36,
    20,
    43,
    27,
    39,
    23,
    2,
    50,
    14,
    62,
    1,
    49,
    13,
    61,
    34,
    18,
    46,
    30,
    33,
    17,
    45,
    29,
    10,
    58,
    6,
    54,
    9,
    57,
    5,
    53,
    42,
    26,
    38,
    22,
    41,
    25,
    37,
    21,
};

uint8_t sharp_memory_luminance(uint32_t argb) {
    const uint32_t red = (argb >> 16) & 0xff;
    const uint32_t green = (argb >> 8) & 0xff;
    const uint32_t blue = argb & 0xff;

    // The weights sum to 256 and closely match the conversion used by the
    // stock SSD1322 backend.
    return (uint8_t)((red * 64 + green * 160 + blue * 32) >> 8);
}

bool sharp_memory_dither_is_white(uint8_t luminance, uint32_t x, uint32_t y, bool invert) {
    const uint8_t threshold = (uint8_t)(bayer8[(y & 7) * 8 + (x & 7)] * 4 + 2);
    const bool white = luminance > threshold;
    return invert ? !white : white;
}

sharp_memory_viewport_t sharp_memory_fit_viewport(uint32_t source_width,
                                                  uint32_t source_height,
                                                  uint32_t target_width,
                                                  uint32_t target_height,
                                                  bool stretch) {
    sharp_memory_viewport_t viewport = {0, 0, target_width, target_height};
    if (stretch || source_width == 0 || source_height == 0 || target_width == 0 || target_height == 0) {
        return viewport;
    }

    if ((uint64_t)target_width * source_height <= (uint64_t)target_height * source_width) {
        viewport.height = (uint32_t)(((uint64_t)target_width * source_height) / source_width);
    } else {
        viewport.width = (uint32_t)(((uint64_t)target_height * source_width) / source_height);
    }

    if (viewport.width == 0)
        viewport.width = 1;
    if (viewport.height == 0)
        viewport.height = 1;

    viewport.x = (target_width - viewport.width) / 2;
    viewport.y = (target_height - viewport.height) / 2;
    return viewport;
}
