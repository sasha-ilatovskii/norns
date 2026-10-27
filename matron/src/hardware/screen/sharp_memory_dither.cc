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

// Ten hand-tuned 3x3 masks ordered by population count. Unlike a thresholded
// Bayer matrix, every mask is a deliberately connected or balanced little
// glyph. This keeps a single 128x64 Cairo pixel legible after exact 3x scaling.
// Five neighboring logical pixels distribute the fractional part of a norns
// level, giving all sixteen levels distinct average coverage despite a 3x3
// tile having only ten integral densities.
static const uint16_t tile3x3_masks[10] = {
    0x000, // 000 / 000 / 000
    0x010, // 000 / 010 / 000
    0x101, // 100 / 000 / 001
    0x054, // 001 / 010 / 100
    0x145, // 101 / 000 / 101
    0x0ba, // 010 / 111 / 010
    0x1ab, // 110 / 101 / 011
    0x0fe, // 011 / 111 / 110
    0x1ef, // 111 / 101 / 111
    0x1ff, // 111 / 111 / 111
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

uint8_t sharp_memory_level(uint8_t luminance) {
    // Norns emits exact multiples of 17. Rounding also gives images and
    // antialiased Cairo output a stable nearest-level conversion.
    const uint16_t rounded = (uint16_t)luminance + 8;
    const uint8_t level = (uint8_t)(rounded / 17);
    return level > 15 ? 15 : level;
}

bool sharp_memory_dither_is_white(uint8_t luminance, uint32_t x, uint32_t y, bool invert) {
    const uint8_t threshold = (uint8_t)(bayer8[(y & 7) * 8 + (x & 7)] * 4 + 2);
    const bool white = luminance > threshold;
    return invert ? !white : white;
}

bool sharp_memory_tile3x3_is_white(uint8_t luminance,
                                   uint32_t x,
                                   uint32_t y,
                                   uint8_t density_boost,
                                   bool invert) {
    const uint8_t level = sharp_memory_level(luminance);

    // A 3x3 tile contains nine pixels. Across five tiles, level N therefore
    // targets 3*N lit pixels. A small boost makes the useful middle levels a
    // little denser without changing the black and white endpoints.
    uint8_t fill_numerator = (uint8_t)(level * 3);
    if (level > 0 && level < 15)
        fill_numerator = (uint8_t)(fill_numerator + density_boost);
    if (fill_numerator > 45)
        fill_numerator = 45;

    const uint32_t tile_x = x / 3;
    const uint32_t tile_y = y / 3;
    const uint8_t phase = (uint8_t)((tile_x + tile_y * 3) % 5);
    uint8_t fill = (uint8_t)(fill_numerator / 5);
    if (phase < fill_numerator % 5)
        ++fill;

    const uint8_t bit = (uint8_t)((y % 3) * 3 + (x % 3));
    const bool white = (tile3x3_masks[fill] & (1U << bit)) != 0;
    return invert ? !white : white;
}

uint8_t sharp_memory_text_shadow_count(uint8_t luminance, uint8_t second_shadow_level) {
    const uint8_t level = sharp_memory_level(luminance);
    if (level == 0 || level == 15)
        return 0;
    return level >= second_shadow_level ? 2 : 1;
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
