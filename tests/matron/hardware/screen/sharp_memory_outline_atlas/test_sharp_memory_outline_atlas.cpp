#include <doctest/doctest.h>

#include "hardware/screen/sharp_memory_outline_atlas.h"

TEST_CASE("outline atlas uses compact periods and ordered track fill") {
    const uint8_t expected_periods[16] = {
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        8,
        4,
        4,
        4,
        4,
        4,
        4,
        4,
        4,
    };
    const uint8_t expected_dashes[16][SHARP_MEMORY_OUTLINE_TRACK_COUNT] = {
        {0, 0, 0},
        {0, 1, 0},
        {0, 2, 0},
        {0, 3, 0},
        {0, 4, 0},
        {0, 5, 0},
        {0, 6, 0},
        {0, 7, 0},
        {1, 4, 0},
        {1, 4, 1},
        {2, 4, 1},
        {2, 4, 2},
        {3, 4, 2},
        {4, 4, 2},
        {4, 4, 3},
        {4, 4, 4},
    };

    for (uint8_t level = 0; level < 16; ++level) {
        const sharp_memory_outline_style_t style = sharp_memory_outline_style(level);
        CHECK(style.period_px == expected_periods[level]);
        for (int track = 0; track < SHARP_MEMORY_OUTLINE_TRACK_COUNT; ++track)
            CHECK(style.dash_px[track] == expected_dashes[level][track]);
        CHECK(style.phase_px[SHARP_MEMORY_OUTLINE_UPPER] == 0);
        CHECK(style.phase_px[SHARP_MEMORY_OUTLINE_CENTER] == 0);
        CHECK(style.phase_px[SHARP_MEMORY_OUTLINE_LOWER] == (level >= 8 ? 2 : 0));
    }
}

TEST_CASE("short step indicators retain distinct low brightness levels") {
    cairo_surface_t *path_surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 32, 12);
    cairo_t *path_context = cairo_create(path_surface);
    cairo_move_to(path_context, 8.5, 5.5);
    cairo_line_to(path_context, 20.5, 5.5);
    cairo_path_t *flat_path = cairo_copy_path_flat(path_context);

    size_t previous_count = 0;
    const uint8_t levels[] = {1, 4, 6, 15};
    for (uint8_t level : levels) {
        cairo_surface_t *target =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 32, 12);
        sharp_memory_outline_render(path_context, flat_path, target, level, 1.0);
        cairo_surface_flush(target);

        const uint32_t *pixels =
            reinterpret_cast<const uint32_t *>(cairo_image_surface_get_data(target));
        const int stride = cairo_image_surface_get_stride(target) / sizeof(uint32_t);
        size_t count = 0;
        for (int y = 0; y < 12; ++y) {
            for (int x = 0; x < 32; ++x)
                count += pixels[y * stride + x] != 0;
        }
        CAPTURE(level);
        CAPTURE(count);
        CHECK(count > previous_count);
        previous_count = count;
        cairo_surface_destroy(target);
    }

    cairo_path_destroy(flat_path);
    cairo_destroy(path_context);
    cairo_surface_destroy(path_surface);
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

TEST_CASE("half-pixel rectangles align the physical atlas with logical ownership") {
    cairo_surface_t *ownership =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 128, 64);
    cairo_t *ownership_context = cairo_create(ownership);
    cairo_set_source_rgba(ownership_context, 1.0, 1.0, 1.0, 1.0);
    cairo_set_line_width(ownership_context, 1.0);
    cairo_rectangle(ownership_context, 0.5, 13.5, 127.0, 2.0);
    cairo_stroke(ownership_context);
    cairo_surface_flush(ownership);

    const uint32_t *ownership_pixels =
        reinterpret_cast<const uint32_t *>(cairo_image_surface_get_data(ownership));
    const int ownership_stride = cairo_image_surface_get_stride(ownership) / sizeof(uint32_t);
    CHECK((ownership_pixels[12 * ownership_stride + 64] >> 24) == 0);
    CHECK((ownership_pixels[13 * ownership_stride + 64] >> 24) != 0);
    CHECK((ownership_pixels[14 * ownership_stride + 64] >> 24) == 0);
    CHECK((ownership_pixels[15 * ownership_stride + 64] >> 24) != 0);
    CHECK((ownership_pixels[16 * ownership_stride + 64] >> 24) == 0);

    cairo_surface_t *path_surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 384, 192);
    cairo_t *path_context = cairo_create(path_surface);
    cairo_scale(path_context, 3.0, 3.0);
    cairo_rectangle(path_context, 0.5, 13.5, 127.0, 2.0);
    cairo_path_t *flat_path = cairo_copy_path_flat(path_context);

    cairo_surface_t *atlas =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 384, 192);
    sharp_memory_outline_render(path_context, flat_path, atlas, 15, 1.0);
    cairo_surface_flush(atlas);

    const uint32_t *atlas_pixels =
        reinterpret_cast<const uint32_t *>(cairo_image_surface_get_data(atlas));
    const int atlas_stride = cairo_image_surface_get_stride(atlas) / sizeof(uint32_t);
    CHECK(atlas_pixels[38 * atlas_stride + 192] == 0);
    CHECK(atlas_pixels[39 * atlas_stride + 192] != 0);
    CHECK(atlas_pixels[40 * atlas_stride + 192] != 0);
    CHECK(atlas_pixels[41 * atlas_stride + 192] != 0);
    CHECK(atlas_pixels[42 * atlas_stride + 192] == 0);
    CHECK(atlas_pixels[44 * atlas_stride + 192] == 0);
    CHECK(atlas_pixels[45 * atlas_stride + 192] != 0);
    CHECK(atlas_pixels[46 * atlas_stride + 192] != 0);
    CHECK(atlas_pixels[47 * atlas_stride + 192] != 0);
    CHECK(atlas_pixels[48 * atlas_stride + 192] == 0);

    for (int y = 0; y < 192; ++y) {
        for (int x = 0; x < 384; ++x) {
            if (atlas_pixels[y * atlas_stride + x] == 0)
                continue;
            CAPTURE(x);
            CAPTURE(y);
            CHECK((ownership_pixels[(y / 3) * ownership_stride + x / 3] >> 24) != 0);
        }
    }

    cairo_surface_destroy(atlas);
    cairo_path_destroy(flat_path);
    cairo_destroy(path_context);
    cairo_surface_destroy(path_surface);
    cairo_destroy(ownership_context);
    cairo_surface_destroy(ownership);
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
