#include <doctest/doctest.h>

#include <cstring>

#include "hardware/screen/sharp_memory_semantics.h"

namespace {

void set_mask_pixel(cairo_surface_t *surface,
                    int x,
                    int y,
                    uint8_t level,
                    uint8_t alpha = 255) {
    cairo_surface_flush(surface);
    uint8_t *data = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    const uint32_t luminance = (uint32_t)level * 17;
    const uint32_t argb = ((uint32_t)alpha << 24) | (luminance << 16) |
                          (luminance << 8) | luminance;
    memcpy(data + (size_t)y * stride + (size_t)x * 4, &argb, sizeof(argb));
    cairo_surface_mark_dirty(surface);
}

sharp_memory_semantic_pixel_t pixel_at(cairo_surface_t *surface, int x, int y) {
    sharp_memory_semantic_pixel_t pixel = {};
    CHECK(sharp_memory_semantic_pixel_at(surface, x, y, &pixel));
    return pixel;
}

} // namespace

TEST_CASE("logical semantic masks expand to physical blocks with level metadata") {
    cairo_surface_t *mask = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 2, 1);
    cairo_surface_t *target = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 6, 3);
    set_mask_pixel(mask, 0, 0, 5);
    set_mask_pixel(mask, 1, 0, 12);

    sharp_memory_semantic_apply_logical_mask(target,
                                             mask,
                                             SHARP_MEMORY_SEMANTIC_TEXT,
                                             3,
                                             0,
                                             0,
                                             128);

    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 6; ++x) {
            const sharp_memory_semantic_pixel_t pixel = pixel_at(target, x, y);
            CHECK(pixel.kind == SHARP_MEMORY_SEMANTIC_TEXT);
            CHECK(pixel.level == (x < 3 ? 5 : 12));
        }
    }

    cairo_surface_destroy(target);
    cairo_surface_destroy(mask);
}

TEST_CASE("later semantic operations overwrite earlier operations in painter order") {
    cairo_surface_t *logical = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 2, 2);
    cairo_surface_t *physical = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 6, 6);
    cairo_surface_t *target = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 6, 6);
    set_mask_pixel(logical, 0, 0, 7);
    set_mask_pixel(physical, 1, 1, 11);

    sharp_memory_semantic_apply_physical_mask(
        target, physical, SHARP_MEMORY_SEMANTIC_STROKE, 128);
    CHECK(pixel_at(target, 1, 1).kind == SHARP_MEMORY_SEMANTIC_STROKE);

    sharp_memory_semantic_apply_logical_mask(target,
                                             logical,
                                             SHARP_MEMORY_SEMANTIC_TEXT,
                                             3,
                                             0,
                                             0,
                                             128);
    CHECK(pixel_at(target, 1, 1).kind == SHARP_MEMORY_SEMANTIC_TEXT);
    CHECK(pixel_at(target, 1, 1).level == 7);

    sharp_memory_semantic_erase_logical_mask(target, logical, 3, 1);
    sharp_memory_semantic_pixel_t erased = {};
    CHECK_FALSE(sharp_memory_semantic_pixel_at(target, 1, 1, &erased));

    cairo_surface_destroy(target);
    cairo_surface_destroy(physical);
    cairo_surface_destroy(logical);
}

TEST_CASE("offset shadows are clipped and text can overwrite their overlap") {
    cairo_surface_t *mask = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_surface_t *target = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 4, 4);
    set_mask_pixel(mask, 0, 0, 8);

    sharp_memory_semantic_apply_logical_mask(target,
                                             mask,
                                             SHARP_MEMORY_SEMANTIC_BACKGROUND,
                                             3,
                                             2,
                                             2,
                                             128);
    sharp_memory_semantic_apply_logical_mask(target,
                                             mask,
                                             SHARP_MEMORY_SEMANTIC_TEXT,
                                             3,
                                             0,
                                             0,
                                             128);

    CHECK(pixel_at(target, 2, 2).kind == SHARP_MEMORY_SEMANTIC_TEXT);
    CHECK(pixel_at(target, 3, 3).kind == SHARP_MEMORY_SEMANTIC_BACKGROUND);

    cairo_surface_destroy(target);
    cairo_surface_destroy(mask);
}

TEST_CASE("physical rectangle erase is rectangular and clipped") {
    cairo_surface_t *mask = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 5, 4);
    cairo_surface_t *target = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 5, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 5; ++x)
            set_mask_pixel(mask, x, y, 15);
    sharp_memory_semantic_apply_physical_mask(
        target, mask, SHARP_MEMORY_SEMANTIC_STROKE, 1);

    sharp_memory_semantic_erase_physical_rectangle(target, 1, 1, 2, 1);
    sharp_memory_semantic_pixel_t pixel = {};
    CHECK_FALSE(sharp_memory_semantic_pixel_at(target, 1, 1, &pixel));
    CHECK_FALSE(sharp_memory_semantic_pixel_at(target, 2, 1, &pixel));
    CHECK(pixel_at(target, 3, 1).kind == SHARP_MEMORY_SEMANTIC_STROKE);
    CHECK(pixel_at(target, 1, 2).kind == SHARP_MEMORY_SEMANTIC_STROKE);

    cairo_surface_destroy(target);
    cairo_surface_destroy(mask);
}

TEST_CASE("semantic surface clear ignores retained Cairo state") {
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 5, 4);
    cairo_t *context = cairo_create(surface);
    cairo_set_source_rgb(context, 1.0, 1.0, 1.0);
    cairo_paint(context);
    cairo_rectangle(context, 1, 1, 1, 1);

    sharp_memory_semantic_surface_clear(surface);

    sharp_memory_semantic_pixel_t pixel = {};
    CHECK_FALSE(sharp_memory_semantic_pixel_at(surface, 0, 0, &pixel));
    CHECK_FALSE(sharp_memory_semantic_pixel_at(surface, 4, 3, &pixel));

    cairo_destroy(context);
    cairo_surface_destroy(surface);
}
