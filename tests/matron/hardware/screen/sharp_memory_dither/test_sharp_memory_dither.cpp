#include <doctest/doctest.h>

#include "hardware/screen/sharp_memory_dither.h"

TEST_CASE("Sharp Memory LCD luminance uses perceptual channel weights") {
    CHECK(sharp_memory_luminance(0xff000000) == 0);
    CHECK(sharp_memory_luminance(0xffffffff) == 255);
    CHECK(sharp_memory_luminance(0xffff0000) == 63);
    CHECK(sharp_memory_luminance(0xff00ff00) == 159);
    CHECK(sharp_memory_luminance(0xff0000ff) == 31);
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

TEST_CASE("Norns surface is centered without changing its aspect ratio") {
    const sharp_memory_viewport_t viewport = sharp_memory_fit_viewport(128, 64, 400, 240, false);
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
