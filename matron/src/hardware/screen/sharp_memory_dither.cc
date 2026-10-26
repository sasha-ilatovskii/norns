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

// Below this contrast, ordered dithering is left unchanged. Above the high
// point, the Bayer threshold collapses to 50%, producing a solid logical edge.
// Between the two points the amount of threshold modulation decreases
// linearly. The wide transition keeps low-contrast graphics from acquiring a
// bright or dark outline while cleaning up antialiased black/white contours.
static const int edge_contrast_low = 64;
static const int edge_contrast_high = 192;
static const int dither_scale = 256;

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

static uint8_t difference(uint8_t first, uint8_t second) {
    return first > second ? first - second : second - first;
}

uint8_t sharp_memory_edge_contrast(uint8_t center,
                                   uint8_t left,
                                   uint8_t right,
                                   uint8_t above,
                                   uint8_t below) {
    uint8_t contrast = difference(center, left);
    const uint8_t right_contrast = difference(center, right);
    const uint8_t above_contrast = difference(center, above);
    const uint8_t below_contrast = difference(center, below);
    if (right_contrast > contrast)
        contrast = right_contrast;
    if (above_contrast > contrast)
        contrast = above_contrast;
    if (below_contrast > contrast)
        contrast = below_contrast;
    return contrast;
}

bool sharp_memory_edge_aware_dither_is_white(uint8_t luminance,
                                             uint8_t edge_contrast,
                                             uint32_t x,
                                             uint32_t y,
                                             bool invert) {
    const int bayer_threshold = bayer8[(y & 7) * 8 + (x & 7)] * 4 + 2;
    int strength = dither_scale;
    if (edge_contrast >= edge_contrast_high) {
        strength = 0;
    } else if (edge_contrast > edge_contrast_low) {
        strength = (edge_contrast_high - edge_contrast) * dither_scale /
                   (edge_contrast_high - edge_contrast_low);
    }

    // Pull the ordered-dither threshold towards 50% as local contrast rises.
    // Signed division intentionally rounds towards zero; the result stays in
    // the original 2..254 threshold range.
    const int threshold = 128 + (bayer_threshold - 128) * strength / dither_scale;
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

    const uint32_t integer_scale_x = target_width / source_width;
    const uint32_t integer_scale_y = target_height / source_height;
    const uint32_t integer_scale =
        integer_scale_x < integer_scale_y ? integer_scale_x : integer_scale_y;

    if (integer_scale > 0) {
        // Keep every source pixel the same size. For the 128x64 norns surface
        // on a 400x240 panel this produces a centered 384x192 image, with each
        // Cairo pixel occupying exactly 3x3 physical pixels.
        viewport.width = source_width * integer_scale;
        viewport.height = source_height * integer_scale;
    } else if ((uint64_t)target_width * source_height <=
               (uint64_t)target_height * source_width) {
        // Fall back to proportional downscaling if the source is larger than
        // the target and no positive integer scale can fit.
        viewport.height =
            (uint32_t)(((uint64_t)target_width * source_height) / source_width);
    } else {
        viewport.width =
            (uint32_t)(((uint64_t)target_height * source_width) / source_height);
    }

    if (viewport.width == 0)
        viewport.width = 1;
    if (viewport.height == 0)
        viewport.height = 1;

    viewport.x = (target_width - viewport.width) / 2;
    viewport.y = (target_height - viewport.height) / 2;
    return viewport;
}
