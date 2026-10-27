#pragma once

#include <cairo.h>
#include <stdint.h>

enum {
    SHARP_MEMORY_OUTLINE_UPPER = 0,
    SHARP_MEMORY_OUTLINE_CENTER = 1,
    SHARP_MEMORY_OUTLINE_LOWER = 2,
    SHARP_MEMORY_OUTLINE_TRACK_COUNT = 3,
};

typedef struct _sharp_memory_outline_style {
    uint8_t period_px;
    uint8_t dash_px[SHARP_MEMORY_OUTLINE_TRACK_COUNT];
    uint8_t phase_px[SHARP_MEMORY_OUTLINE_TRACK_COUNT];
} sharp_memory_outline_style_t;

// Return the procedural outline-atlas row for a norns brightness level.
// Values above 15 are clamped. Level zero has no active tracks.
sharp_memory_outline_style_t sharp_memory_outline_style(uint8_t level);

// Render a flattened Cairo path as three direction-aware parallel tracks.
// Each track is one physical Sharp pixel wide when track_width_px is 1.0.
// The dash period and phase always remain expressed in physical pixels.
void sharp_memory_outline_render(cairo_t *path_context,
                                 const cairo_path_t *flat_path,
                                 cairo_surface_t *target,
                                 uint8_t level,
                                 double track_width_px);
