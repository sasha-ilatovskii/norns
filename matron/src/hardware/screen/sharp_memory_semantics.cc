#include "sharp_memory_semantics.h"

#include <algorithm>
#include <cstring>

namespace {

bool image_surface_data(cairo_surface_t *surface,
                        uint8_t **data,
                        int *width,
                        int *height,
                        int *stride) {
    if (surface == nullptr || cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE)
        return false;
    cairo_surface_flush(surface);
    *data = cairo_image_surface_get_data(surface);
    *width = cairo_image_surface_get_width(surface);
    *height = cairo_image_surface_get_height(surface);
    *stride = cairo_image_surface_get_stride(surface);
    return *data != nullptr && *width > 0 && *height > 0 && *stride > 0;
}

uint32_t encoded_pixel(sharp_memory_semantic_kind_t kind, uint8_t level) {
    if (kind == SHARP_MEMORY_SEMANTIC_FALLBACK)
        return 0;
    return UINT32_C(0xff000000) | ((uint32_t)kind << 16) | ((uint32_t)level << 8);
}

uint8_t mask_level(uint32_t argb) {
    const uint32_t red = (argb >> 16) & 0xff;
    const uint32_t green = (argb >> 8) & 0xff;
    const uint32_t blue = argb & 0xff;
    const uint8_t luminance = (uint8_t)((red * 64 + green * 160 + blue * 32) >> 8);
    const uint8_t level = (uint8_t)(((uint16_t)luminance + 8) / 17);
    return level > 15 ? 15 : level;
}

void write_block(uint8_t *target_data,
                 int target_width,
                 int target_height,
                 int target_stride,
                 int32_t x,
                 int32_t y,
                 uint32_t scale,
                 uint32_t value) {
    const int32_t left = std::max<int32_t>(x, 0);
    const int32_t top = std::max<int32_t>(y, 0);
    const int32_t right = std::min<int32_t>(x + (int32_t)scale, target_width);
    const int32_t bottom = std::min<int32_t>(y + (int32_t)scale, target_height);
    for (int32_t target_y = top; target_y < bottom; ++target_y) {
        uint8_t *row = target_data + (size_t)target_y * target_stride;
        for (int32_t target_x = left; target_x < right; ++target_x)
            std::memcpy(row + (size_t)target_x * 4, &value, sizeof(value));
    }
}

} // namespace

void sharp_memory_semantic_surface_clear(cairo_surface_t *surface) {
    uint8_t *data;
    int width;
    int height;
    int stride;
    if (!image_surface_data(surface, &data, &width, &height, &stride))
        return;
    std::memset(data, 0, (size_t)stride * (size_t)height);
    cairo_surface_mark_dirty(surface);
}

void sharp_memory_semantic_apply_logical_mask(cairo_surface_t *target,
                                              cairo_surface_t *mask,
                                              sharp_memory_semantic_kind_t kind,
                                              uint32_t scale,
                                              int32_t offset_x,
                                              int32_t offset_y,
                                              uint8_t minimum_alpha) {
    uint8_t *target_data;
    int target_width;
    int target_height;
    int target_stride;
    uint8_t *mask_data;
    int mask_width;
    int mask_height;
    int mask_stride;
    if (scale == 0 ||
        !image_surface_data(target, &target_data, &target_width, &target_height, &target_stride) ||
        !image_surface_data(mask, &mask_data, &mask_width, &mask_height, &mask_stride))
        return;

    for (int y = 0; y < mask_height; ++y) {
        const uint8_t *row = mask_data + (size_t)y * mask_stride;
        for (int x = 0; x < mask_width; ++x) {
            uint32_t argb;
            std::memcpy(&argb, row + (size_t)x * 4, sizeof(argb));
            if ((argb >> 24) < minimum_alpha)
                continue;
            write_block(target_data,
                        target_width,
                        target_height,
                        target_stride,
                        x * (int32_t)scale + offset_x,
                        y * (int32_t)scale + offset_y,
                        scale,
                        encoded_pixel(kind, mask_level(argb)));
        }
    }
    cairo_surface_mark_dirty(target);
}

void sharp_memory_semantic_apply_physical_mask(cairo_surface_t *target,
                                               cairo_surface_t *mask,
                                               sharp_memory_semantic_kind_t kind,
                                               uint8_t minimum_alpha) {
    sharp_memory_semantic_apply_logical_mask(
        target, mask, kind, 1, 0, 0, minimum_alpha);
}

void sharp_memory_semantic_erase_logical_mask(cairo_surface_t *target,
                                              cairo_surface_t *mask,
                                              uint32_t scale,
                                              uint8_t minimum_alpha) {
    sharp_memory_semantic_apply_logical_mask(target,
                                             mask,
                                             SHARP_MEMORY_SEMANTIC_FALLBACK,
                                             scale,
                                             0,
                                             0,
                                             minimum_alpha);
}

void sharp_memory_semantic_erase_physical_rectangle(cairo_surface_t *target,
                                                    int32_t x,
                                                    int32_t y,
                                                    int32_t width,
                                                    int32_t height) {
    uint8_t *data;
    int target_width;
    int target_height;
    int stride;
    if (width <= 0 || height <= 0 ||
        !image_surface_data(target, &data, &target_width, &target_height, &stride))
        return;
    const int32_t left = std::max<int32_t>(x, 0);
    const int32_t top = std::max<int32_t>(y, 0);
    const int32_t right = std::min<int32_t>(x + width, target_width);
    const int32_t bottom = std::min<int32_t>(y + height, target_height);
    const uint32_t value = 0;
    for (int32_t target_y = top; target_y < bottom; ++target_y) {
        uint8_t *row = data + (size_t)target_y * stride;
        for (int32_t target_x = left; target_x < right; ++target_x)
            std::memcpy(row + (size_t)target_x * 4, &value, sizeof(value));
    }
    cairo_surface_mark_dirty(target);
}

bool sharp_memory_semantic_pixel_at(cairo_surface_t *surface,
                                    uint32_t x,
                                    uint32_t y,
                                    sharp_memory_semantic_pixel_t *pixel) {
    uint8_t *data;
    int width;
    int height;
    int stride;
    if (pixel == nullptr || !image_surface_data(surface, &data, &width, &height, &stride) ||
        x >= (uint32_t)width || y >= (uint32_t)height)
        return false;

    uint32_t encoded;
    std::memcpy(&encoded, data + (size_t)y * stride + (size_t)x * 4, sizeof(encoded));
    pixel->kind = (sharp_memory_semantic_kind_t)((encoded >> 16) & 0xff);
    pixel->level = (uint8_t)((encoded >> 8) & 0xff);
    return (encoded >> 24) != 0 && pixel->kind != SHARP_MEMORY_SEMANTIC_FALLBACK;
}
