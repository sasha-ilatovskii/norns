#include <doctest/doctest.h>

#include <stdint.h>

#include "hardware/screen/sharp_memory_render.h"

TEST_CASE("Sharp text outline follows the foreground polarity") {
    uint32_t source[25];
    uint8_t mask[25] = {};
    const sharp_memory_viewport_t viewport = {0, 0, 5, 5};

    for (int i = 0; i < 25; ++i)
        source[i] = 0xff000000;
    for (int y = 1; y <= 3; ++y) {
        for (int x = 1; x <= 3; ++x) {
            source[y * 5 + x] = 0xffffffff;
            mask[y * 5 + x] = 255;
        }
    }

    bool is_outline = false;
    CHECK(sharp_memory_text_outline_is_white(
        reinterpret_cast<const uint8_t *>(source), 5 * 4, mask, 5,
        &viewport, 5, 5, 1, 2, 255, 96, &is_outline));
    CHECK(is_outline);

    is_outline = true;
    CHECK_FALSE(sharp_memory_text_outline_is_white(
        reinterpret_cast<const uint8_t *>(source), 5 * 4, mask, 5,
        &viewport, 5, 5, 2, 2, 255, 96, &is_outline));
    CHECK_FALSE(is_outline);

    for (int i = 0; i < 25; ++i)
        source[i] = 0xffffffff;
    for (int y = 1; y <= 3; ++y) {
        for (int x = 1; x <= 3; ++x)
            source[y * 5 + x] = 0xff000000;
    }

    CHECK_FALSE(sharp_memory_text_outline_is_white(
        reinterpret_cast<const uint8_t *>(source), 5 * 4, mask, 5,
        &viewport, 5, 5, 1, 2, 0, 96, &is_outline));
    CHECK(is_outline);
}

TEST_CASE("Upscaled text keeps a one-target-pixel inner outline") {
    uint32_t source[9] = {};
    uint8_t mask[9] = {};
    const sharp_memory_viewport_t viewport = {0, 0, 9, 9};
    source[4] = 0xffffffff;
    mask[4] = 255;

    bool is_outline = false;
    CHECK(sharp_memory_text_outline_is_white(
        reinterpret_cast<const uint8_t *>(source), 3 * 4, mask, 3,
        &viewport, 3, 3, 3, 4, 255, 96, &is_outline));
    CHECK(is_outline);

    is_outline = true;
    CHECK_FALSE(sharp_memory_text_outline_is_white(
        reinterpret_cast<const uint8_t *>(source), 3 * 4, mask, 3,
        &viewport, 3, 3, 4, 4, 255, 96, &is_outline));
    CHECK_FALSE(is_outline);

    is_outline = false;
    CHECK(sharp_memory_text_outline_is_white(
        reinterpret_cast<const uint8_t *>(source), 3 * 4, mask, 3,
        &viewport, 3, 3, 5, 4, 255, 96, &is_outline));
    CHECK(is_outline);
}
