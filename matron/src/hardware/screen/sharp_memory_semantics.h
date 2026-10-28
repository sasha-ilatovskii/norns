#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <cairo.h>

typedef enum _sharp_memory_semantic_kind {
    SHARP_MEMORY_SEMANTIC_FALLBACK = 0,
    SHARP_MEMORY_SEMANTIC_TEXT,
    SHARP_MEMORY_SEMANTIC_STROKE,
    SHARP_MEMORY_SEMANTIC_BACKGROUND,
} sharp_memory_semantic_kind_t;

typedef struct _sharp_memory_semantic_pixel {
    sharp_memory_semantic_kind_t kind;
    uint8_t level;
} sharp_memory_semantic_pixel_t;

// Clear an image surface independently of retained Cairo context state.
void sharp_memory_semantic_surface_clear(cairo_surface_t *surface);

// Apply a logical mask as scale x scale physical blocks. RGB in the mask
// carries the norns level; alpha carries coverage. Later calls overwrite
// earlier semantic pixels, preserving screen API painter order.
void sharp_memory_semantic_apply_logical_mask(cairo_surface_t *target,
                                              cairo_surface_t *mask,
                                              sharp_memory_semantic_kind_t kind,
                                              uint32_t scale,
                                              int32_t offset_x,
                                              int32_t offset_y,
                                              uint8_t minimum_alpha);

// Copy an already physical mask into the compositor without scaling it.
void sharp_memory_semantic_apply_physical_mask(cairo_surface_t *target,
                                               cairo_surface_t *mask,
                                               sharp_memory_semantic_kind_t kind,
                                               uint8_t minimum_alpha);

// Restore fallback rendering wherever a logical mask has coverage.
void sharp_memory_semantic_erase_logical_mask(cairo_surface_t *target,
                                              cairo_surface_t *mask,
                                              uint32_t scale,
                                              uint8_t minimum_alpha);

// Restore fallback rendering in an axis-aligned physical rectangle.
void sharp_memory_semantic_erase_physical_rectangle(cairo_surface_t *target,
                                                    int32_t x,
                                                    int32_t y,
                                                    int32_t width,
                                                    int32_t height);

bool sharp_memory_semantic_pixel_at(cairo_surface_t *surface,
                                    uint32_t x,
                                    uint32_t y,
                                    sharp_memory_semantic_pixel_t *pixel);
