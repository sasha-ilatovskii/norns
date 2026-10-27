#include <doctest/doctest.h>

#include "hardware/screen/sharp_memory_outline_atlas.h"

TEST_CASE("outline atlas preserves the supplied periods and track fill order") {
    const uint8_t expected_periods[16] = {
        1,
        17,
        25,
        41,
        65,
        80,
        80,
        80,
        80,
        80,
        80,
        80,
        80,
        80,
        80,
        80,
    };
    const uint8_t expected_dashes[16][SHARP_MEMORY_OUTLINE_TRACK_COUNT] = {
        {0, 0, 0},
        {0, 16, 0},
        {0, 24, 0},
        {0, 40, 0},
        {0, 64, 0},
        {0, 80, 0},
        {16, 80, 0},
        {16, 80, 16},
        {32, 80, 16},
        {32, 80, 32},
        {48, 80, 32},
        {48, 80, 48},
        {64, 80, 48},
        {64, 80, 64},
        {80, 80, 64},
        {80, 80, 80},
    };

    for (uint8_t level = 0; level < 16; ++level) {
        const sharp_memory_outline_style_t style = sharp_memory_outline_style(level);
        CHECK(style.period_px == expected_periods[level]);
        for (int track = 0; track < SHARP_MEMORY_OUTLINE_TRACK_COUNT; ++track)
            CHECK(style.dash_px[track] == expected_dashes[level][track]);
        CHECK(style.phase_px[SHARP_MEMORY_OUTLINE_UPPER] == 9);
        CHECK(style.phase_px[SHARP_MEMORY_OUTLINE_CENTER] == 0);
        CHECK(style.phase_px[SHARP_MEMORY_OUTLINE_LOWER] == 49);
    }
}

TEST_CASE("outline atlas brightness has strictly increasing mean coverage") {
    double previous = -1.0;
    for (uint8_t level = 0; level < 16; ++level) {
        const sharp_memory_outline_style_t style = sharp_memory_outline_style(level);
        double coverage = 0.0;
        for (int track = 0; track < SHARP_MEMORY_OUTLINE_TRACK_COUNT; ++track)
            coverage += static_cast<double>(style.dash_px[track]) / style.period_px;
        coverage /= SHARP_MEMORY_OUTLINE_TRACK_COUNT;
        CHECK(coverage > previous);
        previous = coverage;
    }
    CHECK(previous == doctest::Approx(1.0));
}

TEST_CASE("three atlas tracks occupy exactly three physical pixels across a path") {
    cairo_surface_t *path_surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 64, 24);
    cairo_t *path_context = cairo_create(path_surface);
    cairo_move_to(path_context, 4.5, 10.5);
    cairo_line_to(path_context, 59.5, 10.5);
    cairo_path_t *flat_path = cairo_copy_path_flat(path_context);

    cairo_surface_t *target = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 64, 24);
    sharp_memory_outline_render(path_context, flat_path, target, 15, 1.0);
    cairo_surface_flush(target);

    const uint32_t *pixels =
        reinterpret_cast<const uint32_t *>(cairo_image_surface_get_data(target));
    const int stride = cairo_image_surface_get_stride(target) / sizeof(uint32_t);
    CHECK(pixels[8 * stride + 32] == 0);
    CHECK(pixels[9 * stride + 32] != 0);
    CHECK(pixels[10 * stride + 32] != 0);
    CHECK(pixels[11 * stride + 32] != 0);
    CHECK(pixels[12 * stride + 32] == 0);

    cairo_surface_destroy(target);
    cairo_path_destroy(flat_path);

    cairo_new_path(path_context);
    cairo_move_to(path_context, 20.5, 4.5);
    cairo_line_to(path_context, 20.5, 19.5);
    flat_path = cairo_copy_path_flat(path_context);
    target = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 64, 24);
    sharp_memory_outline_render(path_context, flat_path, target, 15, 1.0);
    cairo_surface_flush(target);

    pixels = reinterpret_cast<const uint32_t *>(cairo_image_surface_get_data(target));
    const int vertical_stride = cairo_image_surface_get_stride(target) / sizeof(uint32_t);
    CHECK(pixels[12 * vertical_stride + 18] == 0);
    CHECK(pixels[12 * vertical_stride + 19] != 0);
    CHECK(pixels[12 * vertical_stride + 20] != 0);
    CHECK(pixels[12 * vertical_stride + 21] != 0);
    CHECK(pixels[12 * vertical_stride + 22] == 0);

    cairo_surface_destroy(target);
    cairo_path_destroy(flat_path);
    cairo_destroy(path_context);
    cairo_surface_destroy(path_surface);
}

TEST_CASE("outline style clamps out-of-range brightness") {
    const sharp_memory_outline_style_t maximum = sharp_memory_outline_style(15);
    const sharp_memory_outline_style_t clamped = sharp_memory_outline_style(255);
    CHECK(clamped.period_px == maximum.period_px);
    for (int track = 0; track < SHARP_MEMORY_OUTLINE_TRACK_COUNT; ++track) {
        CHECK(clamped.dash_px[track] == maximum.dash_px[track]);
        CHECK(clamped.phase_px[track] == maximum.phase_px[track]);
    }
}
