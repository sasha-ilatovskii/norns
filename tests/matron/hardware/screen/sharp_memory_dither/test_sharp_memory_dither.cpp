#include <doctest/doctest.h>

#include "hardware/screen/sharp_memory_dither.h"

TEST_CASE("Sharp Memory LCD luminance uses perceptual channel weights") {
    CHECK(sharp_memory_luminance(0xff000000) == 0);
    CHECK(sharp_memory_luminance(0xffffffff) == 255);
    CHECK(sharp_memory_luminance(0xffff0000) == 63);
    CHECK(sharp_memory_luminance(0xff00ff00) == 159);
    CHECK(sharp_memory_luminance(0xff0000ff) == 31);
}

TEST_CASE("Sharp Memory luminance maps to the nearest norns level") {
    for (uint8_t level = 0; level < 16; ++level)
        CHECK(sharp_memory_level((uint8_t)(level * 17)) == level);

    CHECK(sharp_memory_level(8) == 0);
    CHECK(sharp_memory_level(9) == 1);
    CHECK(sharp_memory_level(246) == 14);
    CHECK(sharp_memory_level(247) == 15);
}

TEST_CASE("Bayer dithering preserves black, white, and half intensity") {
    int half_white = 0;
    int half_white_inverted = 0;
    for (uint32_t y = 0; y < 8; ++y) {
        for (uint32_t x = 0; x < 8; ++x) {
            CHECK_FALSE(sharp_memory_dither_is_white(0, x, y, false));
            CHECK(sharp_memory_dither_is_white(255, x, y, false));
            half_white += sharp_memory_dither_is_white(128, x, y, false) ? 1 : 0;
            half_white_inverted += sharp_memory_dither_is_white(128, x, y, true) ? 1 : 0;
        }
    }
    CHECK(half_white == 32);
    CHECK(half_white_inverted == 32);
}

TEST_CASE("All 16 norns levels produce distinct ordered-dither coverage") {
    int previous_white = -1;
    for (int level = 0; level < 16; ++level) {
        const uint8_t luminance = (uint8_t)(level * 17);
        int white = 0;
        for (uint32_t y = 0; y < 8; ++y) {
            for (uint32_t x = 0; x < 8; ++x)
                white += sharp_memory_dither_is_white(luminance, x, y, false) ? 1 : 0;
        }
        CHECK(white > previous_white);
        previous_white = white;
    }
    CHECK(previous_white == 64);
}

TEST_CASE("3x3 tiles give all 16 norns levels distinct average coverage") {
    int previous_white = -1;
    for (int level = 0; level < 16; ++level) {
        const uint8_t luminance = (uint8_t)(level * 17);
        int white = 0;
        // Five horizontal logical pixels exercise all five density phases.
        for (uint32_t y = 0; y < 3; ++y) {
            for (uint32_t x = 0; x < 15; ++x)
                white += sharp_memory_tile3x3_is_white(luminance, x, y, 1, false) ? 1 : 0;
        }
        CHECK(white > previous_white);
        CHECK(white == (level == 0 ? 0 : (level == 15 ? 45 : level * 3 + 1)));
        previous_white = white;
    }
}

TEST_CASE("3x3 tiles are stable, anchored, and invertible") {
    for (uint32_t y = 0; y < 15; ++y) {
        for (uint32_t x = 0; x < 15; ++x) {
            const bool normal = sharp_memory_tile3x3_is_white(119, x, y, 1, false);
            CHECK(sharp_memory_tile3x3_is_white(119, x, y, 1, false) == normal);
            CHECK(sharp_memory_tile3x3_is_white(119, x, y, 1, true) != normal);
        }
    }
}

TEST_CASE("Text shadows adapt to norns level") {
    CHECK(sharp_memory_text_shadow_count(0, 8) == 0);
    CHECK(sharp_memory_text_shadow_count(17, 8) == 1);
    CHECK(sharp_memory_text_shadow_count(7 * 17, 8) == 1);
    CHECK(sharp_memory_text_shadow_count(8 * 17, 8) == 2);
    CHECK(sharp_memory_text_shadow_count(14 * 17, 8) == 2);
    CHECK(sharp_memory_text_shadow_count(255, 8) == 0);
}

TEST_CASE("Edge contrast is the largest four-neighbor luminance difference") {
    CHECK(sharp_memory_edge_contrast(100, 90, 120, 60, 101) == 40);
    CHECK(sharp_memory_edge_contrast(0, 0, 0, 0, 255) == 255);
    CHECK(sharp_memory_edge_contrast(255, 0, 255, 255, 255) == 255);
}

TEST_CASE("Edge-aware dithering preserves Bayer in low-contrast regions") {
    for (uint32_t y = 0; y < 8; ++y) {
        for (uint32_t x = 0; x < 8; ++x) {
            for (int luminance = 0; luminance <= 255; ++luminance) {
                CHECK(sharp_memory_edge_aware_dither_is_white(
                          (uint8_t)luminance, 64, x, y, false) ==
                      sharp_memory_dither_is_white((uint8_t)luminance, x, y, false));
            }
        }
    }
}

TEST_CASE("Edge-aware dithering hardens high-contrast contours") {
    for (uint32_t y = 0; y < 8; ++y) {
        for (uint32_t x = 0; x < 8; ++x) {
            CHECK_FALSE(sharp_memory_edge_aware_dither_is_white(128, 192, x, y, false));
            CHECK(sharp_memory_edge_aware_dither_is_white(129, 192, x, y, false));
            CHECK(sharp_memory_edge_aware_dither_is_white(128, 255, x, y, true));
            CHECK_FALSE(sharp_memory_edge_aware_dither_is_white(129, 255, x, y, true));
        }
    }
}

TEST_CASE("Edge-aware dithering progressively reduces threshold variation") {
    int low_contrast_white = 0;
    int medium_contrast_white = 0;
    int high_contrast_white = 0;
    for (uint32_t y = 0; y < 8; ++y) {
        for (uint32_t x = 0; x < 8; ++x) {
            low_contrast_white +=
                sharp_memory_edge_aware_dither_is_white(96, 64, x, y, false) ? 1 : 0;
            medium_contrast_white +=
                sharp_memory_edge_aware_dither_is_white(96, 128, x, y, false) ? 1 : 0;
            high_contrast_white +=
                sharp_memory_edge_aware_dither_is_white(96, 192, x, y, false) ? 1 : 0;
        }
    }
    CHECK(low_contrast_white > medium_contrast_white);
    CHECK(medium_contrast_white > high_contrast_white);
    CHECK(high_contrast_white == 0);
}

TEST_CASE("Norns surface uses a centered integer scale") {
    const sharp_memory_viewport_t viewport = sharp_memory_fit_viewport(128, 64, 400, 240, false);
    CHECK(viewport.x == 8);
    CHECK(viewport.y == 24);
    CHECK(viewport.width == 384);
    CHECK(viewport.height == 192);
    CHECK(viewport.width / 128 == 3);
    CHECK(viewport.height / 64 == 3);
}

TEST_CASE("Oversized surfaces retain proportional downscaling") {
    const sharp_memory_viewport_t viewport = sharp_memory_fit_viewport(800, 400, 400, 240, false);
    CHECK(viewport.x == 0);
    CHECK(viewport.y == 20);
    CHECK(viewport.width == 400);
    CHECK(viewport.height == 200);
}

TEST_CASE("Stretch mode occupies the complete panel") {
    const sharp_memory_viewport_t viewport = sharp_memory_fit_viewport(128, 64, 400, 240, true);
    CHECK(viewport.x == 0);
    CHECK(viewport.y == 0);
    CHECK(viewport.width == 400);
    CHECK(viewport.height == 240);
}
