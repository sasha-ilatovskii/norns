#include "sharp_memory_render.h"

#include <stddef.h>
#include <string.h>

bool sharp_memory_map_target_to_source(const sharp_memory_viewport_t *viewport,
                                       uint32_t source_width,
                                       uint32_t source_height,
                                       int target_x,
                                       int target_y,
                                       uint32_t *source_x,
                                       uint32_t *source_y) {
    if (viewport == NULL || source_x == NULL || source_y == NULL ||
        viewport->width == 0 || viewport->height == 0 ||
        source_width == 0 || source_height == 0 ||
        target_x < (int)viewport->x || target_y < (int)viewport->y ||
        target_x >= (int)(viewport->x + viewport->width) ||
        target_y >= (int)(viewport->y + viewport->height))
        return false;

    *source_x = (uint32_t)(((uint64_t)(target_x - (int)viewport->x) * source_width) /
                           viewport->width);
    *source_y = (uint32_t)(((uint64_t)(target_y - (int)viewport->y) * source_height) /
                           viewport->height);
    return true;
}

static uint8_t text_mask_at_target(const uint8_t *mask_data,
                                   int mask_stride,
                                   const sharp_memory_viewport_t *viewport,
                                   uint32_t source_width,
                                   uint32_t source_height,
                                   int target_x,
                                   int target_y) {
    uint32_t source_x;
    uint32_t source_y;
    if (mask_data == NULL ||
        !sharp_memory_map_target_to_source(viewport, source_width, source_height,
                                           target_x, target_y, &source_x, &source_y))
        return 0;
    return mask_data[(size_t)source_y * mask_stride + source_x];
}

static bool luminance_at_target(const uint8_t *source_data,
                                int source_stride,
                                const sharp_memory_viewport_t *viewport,
                                uint32_t source_width,
                                uint32_t source_height,
                                int target_x,
                                int target_y,
                                uint8_t *luminance) {
    uint32_t source_x;
    uint32_t source_y;
    if (source_data == NULL || luminance == NULL ||
        !sharp_memory_map_target_to_source(viewport, source_width, source_height,
                                           target_x, target_y, &source_x, &source_y))
        return false;

    uint32_t argb;
    memcpy(&argb, source_data + (size_t)source_y * source_stride + source_x * 4, 4);
    *luminance = sharp_memory_luminance(argb);
    return true;
}

bool sharp_memory_text_outline_is_white(const uint8_t *source_data,
                                        int source_stride,
                                        const uint8_t *mask_data,
                                        int mask_stride,
                                        const sharp_memory_viewport_t *viewport,
                                        uint32_t source_width,
                                        uint32_t source_height,
                                        int target_x,
                                        int target_y,
                                        uint8_t text_luminance,
                                        uint8_t threshold,
                                        bool *is_outline) {
    uint32_t background_sum = 0;
    uint32_t background_count = 0;

    if (is_outline == NULL)
        return false;

    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0)
                continue;
            const int neighbor_x = target_x + dx;
            const int neighbor_y = target_y + dy;
            if (text_mask_at_target(mask_data, mask_stride, viewport,
                                    source_width, source_height,
                                    neighbor_x, neighbor_y) < threshold) {
                uint8_t background_luminance;
                if (luminance_at_target(source_data, source_stride, viewport,
                                        source_width, source_height,
                                        neighbor_x, neighbor_y, &background_luminance)) {
                    background_sum += background_luminance;
                    background_count++;
                }
            }
        }
    }

    *is_outline = background_count != 0;
    if (!*is_outline)
        return false;
    return text_luminance >= background_sum / background_count;
}
