/*
 * screen.c
 *
 * cairo drawing (and a little ioctl)
 */

#include <assert.h>
#include <cairo-ft.h>
#include <cairo.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "args.h"
#include "event_types.h"
#include "events.h"
#include "hardware/io.h"
#include "hardware/screen.h"
#include "hardware/screen/sharp_memory_outline_atlas.h"
#include "hardware/screen/ssd1322.h"
#include "screen.h"
#include "screen_results.h"

#define NUM_FONTS 69
#define NUM_OPS 29
#define SEMANTIC_STROKE_SCALE 3

static char font_path[NUM_FONTS][32];

static float c[16] = {0, 0.066666666666667, 0.13333333333333, 0.2, 0.26666666666667, 0.33333333333333,
                      0.4, 0.46666666666667, 0.53333333333333, 0.6, 0.66666666666667, 0.73333333333333,
                      0.8, 0.86666666666667, 0.93333333333333, 1};

static cairo_operator_t ops[NUM_OPS] = {
    CAIRO_OPERATOR_OVER,
    CAIRO_OPERATOR_XOR,
    CAIRO_OPERATOR_ADD,
    CAIRO_OPERATOR_MULTIPLY,
    CAIRO_OPERATOR_SCREEN,
    CAIRO_OPERATOR_OVERLAY,
    CAIRO_OPERATOR_DARKEN,
    CAIRO_OPERATOR_LIGHTEN,
    CAIRO_OPERATOR_COLOR_DODGE,
    CAIRO_OPERATOR_COLOR_BURN,
    CAIRO_OPERATOR_HARD_LIGHT,
    CAIRO_OPERATOR_SOFT_LIGHT,
    CAIRO_OPERATOR_DIFFERENCE,
    CAIRO_OPERATOR_EXCLUSION,
    CAIRO_OPERATOR_CLEAR,
    CAIRO_OPERATOR_SOURCE,
    CAIRO_OPERATOR_IN,
    CAIRO_OPERATOR_OUT,
    CAIRO_OPERATOR_ATOP,
    CAIRO_OPERATOR_DEST,
    CAIRO_OPERATOR_DEST_OVER,
    CAIRO_OPERATOR_DEST_IN,
    CAIRO_OPERATOR_DEST_OUT,
    CAIRO_OPERATOR_DEST_ATOP,
    CAIRO_OPERATOR_SATURATE,
    CAIRO_OPERATOR_HSL_HUE,
    CAIRO_OPERATOR_HSL_SATURATION,
    CAIRO_OPERATOR_HSL_COLOR,
    CAIRO_OPERATOR_HSL_LUMINOSITY};

static cairo_surface_t *surface;
static cairo_surface_t *image;
static cairo_t *cr;
static cairo_t *cr_primary;
static cairo_surface_t *semantic_text_surface;
static cairo_t *semantic_text_cr;
static cairo_surface_t *semantic_stroke_surface;
static cairo_t *semantic_stroke_cr;
static cairo_surface_t *semantic_stroke_coverage_surface;
static cairo_t *semantic_stroke_coverage_cr;
static bool semantic_surfaces_valid;
static uint8_t semantic_stroke_level_current;
static std::vector<uint8_t> semantic_stroke_level_stack;
static bool surface_may_have_color = false;

static cairo_font_face_t *ct[NUM_FONTS];
static FT_Library value;
static FT_Error status;
static FT_Face face[NUM_FONTS];

static void init_font_faces(void);

static bool semantic_text_active(void) {
    return cr == cr_primary && semantic_text_cr != NULL && semantic_stroke_cr != NULL &&
           semantic_stroke_coverage_cr != NULL;
}

static bool semantic_surfaces_requested(void) {
    matron_io_t *io;
    TAILQ_FOREACH(io, &io_queue, entries) {
        if (io->ops->type != IO_SCREEN)
            continue;
        screen_ops_t *screen_ops = (screen_ops_t *)io->ops;
        if (screen_ops->bind_semantics != NULL)
            return true;
    }
    return false;
}

static void semantic_text_set_hard_antialias(void) {
    cairo_font_options_t *font_options = cairo_font_options_create();
    cairo_set_antialias(semantic_text_cr, CAIRO_ANTIALIAS_NONE);
    cairo_font_options_set_antialias(font_options, CAIRO_ANTIALIAS_NONE);
    cairo_set_font_options(semantic_text_cr, font_options);
    cairo_font_options_destroy(font_options);

    font_options = cairo_font_options_create();
    cairo_set_antialias(semantic_stroke_cr, CAIRO_ANTIALIAS_NONE);
    cairo_font_options_set_antialias(font_options, CAIRO_ANTIALIAS_NONE);
    cairo_set_font_options(semantic_stroke_cr, font_options);
    cairo_font_options_destroy(font_options);
}

static void semantic_text_clear(void) {
    cairo_save(semantic_text_cr);
    cairo_set_operator(semantic_text_cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(semantic_text_cr);
    cairo_restore(semantic_text_cr);

    cairo_save(semantic_stroke_cr);
    cairo_set_operator(semantic_stroke_cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(semantic_stroke_cr);
    cairo_restore(semantic_stroke_cr);

    cairo_save(semantic_stroke_coverage_cr);
    cairo_set_operator(semantic_stroke_coverage_cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(semantic_stroke_coverage_cr);
    cairo_restore(semantic_stroke_coverage_cr);
}

static uint8_t semantic_stroke_level(void) {
    // Capture the level from the canonical context at stroke time. This is
    // the source that Cairo actually uses for the 128x64 framebuffer, so it
    // also remains authoritative when sidecar context state has temporarily
    // diverged. Non-solid sources fall back to the tracked screen.level.
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double alpha = 0.0;
    if (cr == cr_primary &&
        cairo_pattern_get_rgba(cairo_get_source(cr), &red, &green, &blue, &alpha) ==
            CAIRO_STATUS_SUCCESS) {
        int level = (int)floor(red * 15.0 + 0.5);
        return (uint8_t)(level < 0 ? 0 : (level > 15 ? 15 : level));
    }
    return semantic_stroke_level_current;
}

static void semantic_replace_path(cairo_t *context, const cairo_path_t *path) {
    cairo_new_path(context);
    cairo_append_path(context, path);
}

static void semantic_replace_paths(const cairo_path_t *path) {
    semantic_replace_path(semantic_text_cr, path);
    semantic_replace_path(semantic_stroke_cr, path);
    semantic_replace_path(semantic_stroke_coverage_cr, path);
}

static void semantic_stroke_append_and_stroke(const cairo_path_t *path,
                                              double width,
                                              double dash,
                                              double gap,
                                              cairo_operator_t op) {
    cairo_new_path(semantic_stroke_cr);
    cairo_append_path(semantic_stroke_cr, path);
    cairo_set_operator(semantic_stroke_cr, op);
    cairo_set_line_width(semantic_stroke_cr, width / SEMANTIC_STROKE_SCALE);
    if (dash > 0.0) {
        const double dashes[2] = {
            dash / SEMANTIC_STROKE_SCALE,
            gap / SEMANTIC_STROKE_SCALE,
        };
        cairo_set_dash(semantic_stroke_cr, dashes, 2, 0.0);
    } else {
        cairo_set_dash(semantic_stroke_cr, NULL, 0, 0.0);
    }
    cairo_stroke(semantic_stroke_cr);
}

static void semantic_stroke_draw(uint8_t level) {
    cairo_path_t *path = cairo_copy_path(semantic_stroke_cr);
    cairo_path_t *flat_path = cairo_copy_path_flat(semantic_stroke_cr);
    const double api_width = cairo_get_line_width(semantic_stroke_cr);

    // The semantic context has a permanent 3x base transform. Remove that
    // scale when translating screen.line_width into a physical track width;
    // any additional user transform remains represented in the result.
    double horizontal_x = api_width;
    double horizontal_y = 0.0;
    double vertical_x = 0.0;
    double vertical_y = api_width;
    cairo_user_to_device_distance(semantic_stroke_cr, &horizontal_x, &horizontal_y);
    cairo_user_to_device_distance(semantic_stroke_cr, &vertical_x, &vertical_y);
    const double track_width_px =
        (hypot(horizontal_x, horizontal_y) + hypot(vertical_x, vertical_y)) /
        (2.0 * SEMANTIC_STROKE_SCALE);

    cairo_save(semantic_stroke_cr);
    cairo_set_source_rgba(semantic_stroke_cr, 1.0, 1.0, 1.0, 1.0);

    // Remove an older semantic stroke wherever the canonical Cairo stroke is
    // painting. The new brightness-specific layers are then built back up.
    semantic_stroke_append_and_stroke(
        path, api_width * SEMANTIC_STROKE_SCALE, 0.0, 0.0, CAIRO_OPERATOR_CLEAR);
    cairo_restore(semantic_stroke_cr);

    sharp_memory_outline_render(
        semantic_stroke_cr, flat_path, semantic_stroke_surface, level, track_width_px);

    // Claim the complete stroke footprint at canonical 128x64 resolution.
    // The backend expands each touched logical pixel with the same mapping as
    // the framebuffer, so half-pixel paths and antialiasing cannot drift away
    // from the physical outline atlas.
    cairo_save(semantic_stroke_coverage_cr);
    cairo_set_operator(semantic_stroke_coverage_cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(semantic_stroke_coverage_cr, 1.0, 1.0, 1.0, 1.0);
    cairo_stroke(semantic_stroke_coverage_cr);
    cairo_restore(semantic_stroke_coverage_cr);

    cairo_new_path(semantic_stroke_cr);
    cairo_path_destroy(flat_path);
    cairo_path_destroy(path);
}

static void semantic_stroke_erase_path(bool stroke) {
    cairo_save(semantic_stroke_cr);
    cairo_set_operator(semantic_stroke_cr, CAIRO_OPERATOR_CLEAR);
    if (stroke)
        cairo_stroke(semantic_stroke_cr);
    else
        cairo_fill(semantic_stroke_cr);
    cairo_restore(semantic_stroke_cr);

    cairo_save(semantic_stroke_coverage_cr);
    cairo_set_operator(semantic_stroke_coverage_cr, CAIRO_OPERATOR_CLEAR);
    if (stroke)
        cairo_stroke(semantic_stroke_coverage_cr);
    else
        cairo_fill(semantic_stroke_coverage_cr);
    cairo_restore(semantic_stroke_coverage_cr);
}

static void semantic_show_text(const char *text) {
    cairo_show_text(semantic_text_cr, text);
    cairo_save(semantic_stroke_cr);
    cairo_set_operator(semantic_stroke_cr, CAIRO_OPERATOR_CLEAR);
    cairo_show_text(semantic_stroke_cr, text);
    cairo_restore(semantic_stroke_cr);

    // Text has its own hard semantic mask. Clear logical stroke ownership
    // with the same hard glyph raster instead of the coverage context's
    // canonical path antialiasing mode.
    cairo_font_options_t *font_options = cairo_font_options_create();
    cairo_font_options_set_antialias(font_options, CAIRO_ANTIALIAS_NONE);
    cairo_save(semantic_stroke_coverage_cr);
    cairo_set_operator(semantic_stroke_coverage_cr, CAIRO_OPERATOR_CLEAR);
    cairo_set_antialias(semantic_stroke_coverage_cr, CAIRO_ANTIALIAS_NONE);
    cairo_set_font_options(semantic_stroke_coverage_cr, font_options);
    cairo_show_text(semantic_stroke_coverage_cr, text);
    cairo_restore(semantic_stroke_coverage_cr);
    cairo_font_options_destroy(font_options);

    // Font hinting can produce a slightly different advance at the stroke
    // layer's 3x CTM. Keep all sidecar current points locked to canonical Cairo
    // so later relative drawing commands cannot accumulate drift.
    double x, y;
    cairo_get_current_point(cr, &x, &y);
    cairo_move_to(semantic_text_cr, x, y);
    cairo_move_to(semantic_stroke_cr, x, y);
    cairo_move_to(semantic_stroke_coverage_cr, x, y);
}

static void semantic_text_erase_path(bool stroke) {
    cairo_save(semantic_text_cr);
    cairo_set_operator(semantic_text_cr, CAIRO_OPERATOR_CLEAR);
    if (stroke)
        cairo_stroke(semantic_text_cr);
    else
        cairo_fill(semantic_text_cr);
    cairo_restore(semantic_text_cr);
}

static void semantic_text_erase_rectangle(double x, double y, double width, double height) {
    cairo_save(semantic_text_cr);
    cairo_set_operator(semantic_text_cr, CAIRO_OPERATOR_CLEAR);
    cairo_rectangle(semantic_text_cr, x, y, width, height);
    cairo_fill(semantic_text_cr);
    cairo_restore(semantic_text_cr);

    cairo_save(semantic_stroke_cr);
    cairo_set_operator(semantic_stroke_cr, CAIRO_OPERATOR_CLEAR);
    cairo_rectangle(semantic_stroke_cr, x, y, width, height);
    cairo_fill(semantic_stroke_cr);
    cairo_restore(semantic_stroke_cr);

    cairo_save(semantic_stroke_coverage_cr);
    cairo_set_operator(semantic_stroke_coverage_cr, CAIRO_OPERATOR_CLEAR);
    cairo_rectangle(semantic_stroke_coverage_cr, x, y, width, height);
    cairo_fill(semantic_stroke_coverage_cr);
    cairo_restore(semantic_stroke_coverage_cr);
}

static void semantic_text_erase_raw_rectangle(double x, double y, double width, double height) {
    cairo_path_t *path = cairo_copy_path(semantic_text_cr);
    cairo_new_path(semantic_text_cr);
    cairo_save(semantic_text_cr);
    cairo_identity_matrix(semantic_text_cr);
    cairo_set_operator(semantic_text_cr, CAIRO_OPERATOR_CLEAR);
    cairo_rectangle(semantic_text_cr, x, y, width, height);
    cairo_fill(semantic_text_cr);
    cairo_restore(semantic_text_cr);
    cairo_append_path(semantic_text_cr, path);
    cairo_path_destroy(path);

    path = cairo_copy_path(semantic_stroke_cr);
    cairo_new_path(semantic_stroke_cr);
    cairo_save(semantic_stroke_cr);
    cairo_identity_matrix(semantic_stroke_cr);
    cairo_set_operator(semantic_stroke_cr, CAIRO_OPERATOR_CLEAR);
    cairo_rectangle(semantic_stroke_cr,
                    x * SEMANTIC_STROKE_SCALE,
                    y * SEMANTIC_STROKE_SCALE,
                    width * SEMANTIC_STROKE_SCALE,
                    height * SEMANTIC_STROKE_SCALE);
    cairo_fill(semantic_stroke_cr);
    cairo_restore(semantic_stroke_cr);
    cairo_append_path(semantic_stroke_cr, path);
    cairo_path_destroy(path);

    path = cairo_copy_path(semantic_stroke_coverage_cr);
    cairo_new_path(semantic_stroke_coverage_cr);
    cairo_save(semantic_stroke_coverage_cr);
    cairo_identity_matrix(semantic_stroke_coverage_cr);
    cairo_set_operator(semantic_stroke_coverage_cr, CAIRO_OPERATOR_CLEAR);
    cairo_rectangle(semantic_stroke_coverage_cr, x, y, width, height);
    cairo_fill(semantic_stroke_coverage_cr);
    cairo_restore(semantic_stroke_coverage_cr);
    cairo_append_path(semantic_stroke_coverage_cr, path);
    cairo_path_destroy(path);
}

//---------------------------------------
//--- extern function definitions

void screen_init(void) {
    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 128, 64);
    cr = cr_primary = cairo_create(surface);
    semantic_surfaces_valid = true;
    semantic_stroke_level_current = 0;
    semantic_stroke_level_stack.clear();
    if (semantic_surfaces_requested()) {
        semantic_text_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 128, 64);
        semantic_text_cr = cairo_create(semantic_text_surface);
        semantic_stroke_surface = cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32, 128 * SEMANTIC_STROKE_SCALE, 64 * SEMANTIC_STROKE_SCALE);
        semantic_stroke_cr = cairo_create(semantic_stroke_surface);
        semantic_stroke_coverage_surface =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 128, 64);
        semantic_stroke_coverage_cr = cairo_create(semantic_stroke_coverage_surface);
        // Map logical Cairo geometry directly onto the exact 3x pixel grid.
        // In particular, the half-integer coordinates used for crisp norns
        // strokes must remain centered inside their logical 3x3 tiles.
        cairo_scale(semantic_stroke_cr, SEMANTIC_STROKE_SCALE, SEMANTIC_STROKE_SCALE);
        semantic_text_clear();
        semantic_text_set_hard_antialias();
    }

    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_font_options_t *font_options = cairo_font_options_create();
    cairo_font_options_set_antialias(font_options, CAIRO_ANTIALIAS_SUBPIXEL);
    cairo_set_font_options(cr, font_options);
    cairo_font_options_destroy(font_options);

    init_font_faces();

    // default font
    cairo_set_font_face(cr, ct[0]);
    cairo_set_font_size(cr, 8.0);

    // config buffer
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cr, surface, 0, 0);
}

//-----------------
//-- screen commands

void screen_text_right(const char *s) {
    cairo_text_extents_t extents;
    cairo_text_extents(cr, s, &extents);
    cairo_rel_move_to(cr, -extents.width, 0);
    if (semantic_text_active()) {
        cairo_rel_move_to(semantic_text_cr, -extents.width, 0);
        cairo_rel_move_to(semantic_stroke_cr, -extents.width, 0);
        cairo_rel_move_to(semantic_stroke_coverage_cr, -extents.width, 0);
    }
    cairo_show_text(cr, s);
    if (semantic_text_active())
        semantic_show_text(s);
}

void screen_text_center(const char *s) {
    cairo_text_extents_t extents;
    double x, y;
    cairo_text_extents(cr, s, &extents);
    cairo_get_current_point(cr, &x, &y);

    // Keep pixel fonts on the logical pixel grid. Newer Cairo/FreeType
    // versions rasterize glyphs placed at half-pixel origins differently,
    // which can widen centered text and join adjacent labels.
    x = floor(x - extents.width * 0.5 + 0.5);
    cairo_move_to(cr, x, y);
    if (semantic_text_active()) {
        cairo_move_to(semantic_text_cr, x, y);
        cairo_move_to(semantic_stroke_cr, x, y);
        cairo_move_to(semantic_stroke_coverage_cr, x, y);
    }
    cairo_show_text(cr, s);
    if (semantic_text_active())
        semantic_show_text(s);
}

void screen_text_trim(char *s, double w) {
    cairo_text_extents_t extents;
    int n = strlen(s);
    do {
        s[n--] = '\0';
        cairo_text_extents(cr, s, &extents);
        if (n <= 0) {
            return;
        }
    } while (extents.width > w);
    cairo_show_text(cr, s);
    if (semantic_text_active())
        semantic_show_text(s);
}

void screen_current_point() {
    double x, y;
    cairo_get_current_point(cr, &x, &y);
    union screen_results_data *results = screen_results_data_new(SCREEN_RESULTS_CURRENT_POINT);
    results->current_point.x = x;
    results->current_point.y = y;
    screen_results_post(results);
}

//-------------------------------------------------------
//-- static function definitions

void init_font_faces(void) {

    status = FT_Init_FreeType(&value);
    if (status != 0) {
        fprintf(stderr, "ERROR (screen) freetype init\n");
        return;
    }

    int font_idx = 0;
    strcpy(font_path[font_idx++], "norns.ttf");
    strcpy(font_path[font_idx++], "liquid.ttf");
    strcpy(font_path[font_idx++], "Roboto-Thin.ttf");
    strcpy(font_path[font_idx++], "Roboto-Light.ttf");
    strcpy(font_path[font_idx++], "Roboto-Regular.ttf");
    strcpy(font_path[font_idx++], "Roboto-Medium.ttf");
    strcpy(font_path[font_idx++], "Roboto-Bold.ttf");
    strcpy(font_path[font_idx++], "Roboto-Black.ttf");
    strcpy(font_path[font_idx++], "Roboto-ThinItalic.ttf");
    strcpy(font_path[font_idx++], "Roboto-LightItalic.ttf");
    strcpy(font_path[font_idx++], "Roboto-Italic.ttf");
    strcpy(font_path[font_idx++], "Roboto-MediumItalic.ttf");
    strcpy(font_path[font_idx++], "Roboto-BoldItalic.ttf");
    strcpy(font_path[font_idx++], "Roboto-BlackItalic.ttf");
    strcpy(font_path[font_idx++], "VeraBd.ttf");
    strcpy(font_path[font_idx++], "VeraBI.ttf");
    strcpy(font_path[font_idx++], "VeraIt.ttf");
    strcpy(font_path[font_idx++], "VeraMoBd.ttf");
    strcpy(font_path[font_idx++], "VeraMoBI.ttf");
    strcpy(font_path[font_idx++], "VeraMoIt.ttf");
    strcpy(font_path[font_idx++], "VeraMono.ttf");
    strcpy(font_path[font_idx++], "VeraSeBd.ttf");
    strcpy(font_path[font_idx++], "VeraSe.ttf");
    strcpy(font_path[font_idx++], "Vera.ttf");
    //------------------
    // bitmap fonts
    strcpy(font_path[font_idx++], "bmp/tom-thumb.bdf");
    strcpy(font_path[font_idx++], "bmp/creep.bdf");
    strcpy(font_path[font_idx++], "bmp/ctrld-fixed-10b.bdf");
    strcpy(font_path[font_idx++], "bmp/ctrld-fixed-10r.bdf");
    strcpy(font_path[font_idx++], "bmp/ctrld-fixed-13b.bdf");
    strcpy(font_path[font_idx++], "bmp/ctrld-fixed-13b-i.bdf");
    strcpy(font_path[font_idx++], "bmp/ctrld-fixed-13r.bdf");
    strcpy(font_path[font_idx++], "bmp/ctrld-fixed-13r-i.bdf");
    strcpy(font_path[font_idx++], "bmp/ctrld-fixed-16b.bdf");
    strcpy(font_path[font_idx++], "bmp/ctrld-fixed-16b-i.bdf");
    strcpy(font_path[font_idx++], "bmp/ctrld-fixed-16r.bdf");
    strcpy(font_path[font_idx++], "bmp/ctrld-fixed-16r-i.bdf");
    strcpy(font_path[font_idx++], "bmp/scientifica-11.bdf");
    strcpy(font_path[font_idx++], "bmp/scientificaBold-11.bdf");
    strcpy(font_path[font_idx++], "bmp/scientificaItalic-11.bdf");
    strcpy(font_path[font_idx++], "bmp/ter-u12b.bdf");
    strcpy(font_path[font_idx++], "bmp/ter-u12n.bdf");
    strcpy(font_path[font_idx++], "bmp/ter-u14b.bdf");
    strcpy(font_path[font_idx++], "bmp/ter-u14n.bdf");
    strcpy(font_path[font_idx++], "bmp/ter-u14v.bdf");
    strcpy(font_path[font_idx++], "bmp/ter-u16b.bdf");
    strcpy(font_path[font_idx++], "bmp/ter-u16n.bdf");
    strcpy(font_path[font_idx++], "bmp/ter-u16v.bdf");
    strcpy(font_path[font_idx++], "bmp/ter-u18b.bdf");
    strcpy(font_path[font_idx++], "bmp/ter-u18n.bdf");
    strcpy(font_path[font_idx++], "bmp/ter-u20b.bdf");
    strcpy(font_path[font_idx++], "bmp/ter-u20n.bdf");
    strcpy(font_path[font_idx++], "bmp/ter-u22b.bdf");
    strcpy(font_path[font_idx++], "bmp/ter-u22n.bdf");
    strcpy(font_path[font_idx++], "bmp/ter-u24b.bdf");
    strcpy(font_path[font_idx++], "bmp/ter-u24n.bdf");
    strcpy(font_path[font_idx++], "bmp/ter-u28b.bdf");
    strcpy(font_path[font_idx++], "bmp/ter-u28n.bdf");
    strcpy(font_path[font_idx++], "bmp/ter-u32b.bdf");
    strcpy(font_path[font_idx++], "bmp/ter-u32n.bdf");
    strcpy(font_path[font_idx++], "bmp/unscii-16-full.pcf");
    strcpy(font_path[font_idx++], "bmp/unscii-16.pcf");
    strcpy(font_path[font_idx++], "bmp/unscii-8-alt.pcf");
    strcpy(font_path[font_idx++], "bmp/unscii-8-fantasy.pcf");
    strcpy(font_path[font_idx++], "bmp/unscii-8-mcr.pcf");
    strcpy(font_path[font_idx++], "bmp/unscii-8.pcf");
    strcpy(font_path[font_idx++], "bmp/unscii-8-tall.pcf");
    strcpy(font_path[font_idx++], "bmp/unscii-8-thin.pcf");
    //------------------
    // not specifically bitmap but added to the tail of the font list to avoid breaking changes
    strcpy(font_path[font_idx++], "Particle.ttf");
    strcpy(font_path[font_idx++], "norns.ttf"); // alias 04B_03__.TTF to norns.ttf
    assert(font_idx == NUM_FONTS);

#if 0 // is this really necessary?
    fprintf(stderr, "fonts: \n");
    for (int i = 0; i < NUM_FONTS; ++i) {
        fprintf(stderr, "  %d: %s\n", i, font_path[i]);
    }
#endif

    int path_bytes = 512;
    char *path = (char *)malloc(path_bytes);
    for (int i = 0; i < NUM_FONTS; i++) {
        status = snprintf(path, path_bytes, "%s/norns/resources/%s", getenv("HOME"), font_path[i]);
        if (status > path_bytes) {
            fprintf(stderr, "ERROR (screen) font path too long: %s\n", path);
        }

        status = FT_New_Face(value, path, 0, &face[i]);
        if (status != 0) {
            fprintf(stderr, "ERROR (screen) couldn't load font: %s\n", path);
        } else {
            ct[i] = cairo_ft_font_face_create_for_ft_face(face[i], 0);
        }
    }
    free(path);

    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_font_options_t *font_options = cairo_font_options_create();
    cairo_font_options_set_antialias(font_options, CAIRO_ANTIALIAS_GRAY);
    cairo_set_font_options(cr, font_options);
    cairo_font_options_destroy(font_options);

    // default font
    cairo_set_font_face(cr, ct[0]);
    cairo_set_font_size(cr, 8.0);
    if (semantic_text_cr != NULL && semantic_stroke_cr != NULL &&
        semantic_stroke_coverage_cr != NULL) {
        cairo_set_font_face(semantic_text_cr, ct[0]);
        cairo_set_font_size(semantic_text_cr, 8.0);
        cairo_set_font_face(semantic_stroke_cr, ct[0]);
        cairo_set_font_size(semantic_stroke_cr, 8.0);
        cairo_set_font_face(semantic_stroke_coverage_cr, ct[0]);
        cairo_set_font_size(semantic_stroke_coverage_cr, 8.0);
        semantic_text_set_hard_antialias();
    }

    fprintf(stderr, "font setup OK.\n");
    matron_io_t *io;
    TAILQ_FOREACH(io, &io_queue, entries) {
        if (io->ops->type != IO_SCREEN)
            continue;
        matron_fb_t *fb = (matron_fb_t *)io;
        screen_ops_t *fb_ops = (screen_ops_t *)io->ops;
        fb_ops->bind(fb, surface);
        if (fb_ops->bind_semantics != NULL) {
            screen_semantics_t semantics;
            semantics.text = semantic_text_surface;
            semantics.stroke = semantic_stroke_surface;
            semantics.stroke_coverage = semantic_stroke_coverage_surface;
            semantics.valid = &semantic_surfaces_valid;
            fb_ops->bind_semantics(fb, &semantics);
        }
    }
}

void screen_deinit(void) {
    semantic_stroke_level_stack.clear();
    if (semantic_stroke_coverage_cr != NULL) {
        cairo_destroy(semantic_stroke_coverage_cr);
        semantic_stroke_coverage_cr = NULL;
    }
    if (semantic_stroke_coverage_surface != NULL) {
        cairo_surface_destroy(semantic_stroke_coverage_surface);
        semantic_stroke_coverage_surface = NULL;
    }
    if (semantic_stroke_cr != NULL) {
        cairo_destroy(semantic_stroke_cr);
        semantic_stroke_cr = NULL;
    }
    if (semantic_stroke_surface != NULL) {
        cairo_surface_destroy(semantic_stroke_surface);
        semantic_stroke_surface = NULL;
    }
    if (semantic_text_cr != NULL) {
        cairo_destroy(semantic_text_cr);
        semantic_text_cr = NULL;
    }
    if (semantic_text_surface != NULL) {
        cairo_surface_destroy(semantic_text_surface);
        semantic_text_surface = NULL;
    }
    if (cr != NULL) {
        cairo_destroy(cr);
        cr = NULL;
        cr_primary = NULL;
    }
    if (surface != NULL) {
        cairo_surface_destroy(surface);
        surface = NULL;
    }
}

bool screen_has_io_backend(void) {
    matron_io_t *io;
    TAILQ_FOREACH(io, &io_queue, entries) {
        if (io->ops->type == IO_SCREEN)
            return true;
    }
    return false;
}

void screen_update(void) {
    bool painted = false;
    matron_io_t *io;
    TAILQ_FOREACH(io, &io_queue, entries) {
        if (io->ops->type != IO_SCREEN)
            continue;
        matron_fb_t *fb = (matron_fb_t *)io;
        screen_ops_t *fb_ops = (screen_ops_t *)io->ops;
        fb_ops->paint(fb);
        painted = true;
    }
    if (painted)
        return;

    cairo_surface_flush(surface);
    ssd1322_update(surface, surface_may_have_color);
}

void screen_save(void) {
    cairo_save(cr);
    if (semantic_text_active()) {
        semantic_stroke_level_stack.push_back(semantic_stroke_level_current);
        cairo_save(semantic_text_cr);
        cairo_save(semantic_stroke_cr);
        cairo_save(semantic_stroke_coverage_cr);
    }
}

void screen_restore(void) {
    cairo_restore(cr);
    if (semantic_text_active()) {
        cairo_restore(semantic_text_cr);
        cairo_restore(semantic_stroke_cr);
        cairo_restore(semantic_stroke_coverage_cr);
        if (!semantic_stroke_level_stack.empty()) {
            semantic_stroke_level_current = semantic_stroke_level_stack.back();
            semantic_stroke_level_stack.pop_back();
        }
    }
}

void screen_font_face(int i) {
    if ((i >= 0) && (i < NUM_FONTS)) {
        cairo_set_font_face(cr, ct[i]);
        if (semantic_text_active()) {
            cairo_set_font_face(semantic_text_cr, ct[i]);
            cairo_set_font_face(semantic_stroke_cr, ct[i]);
            cairo_set_font_face(semantic_stroke_coverage_cr, ct[i]);
        }
    }
}

void screen_font_size(double z) {
    cairo_set_font_size(cr, z);
    if (semantic_text_active()) {
        cairo_set_font_size(semantic_text_cr, z);
        cairo_set_font_size(semantic_stroke_cr, z);
        cairo_set_font_size(semantic_stroke_coverage_cr, z);
    }
}

void screen_aa(int s) {
    cairo_font_options_t *font_options = cairo_font_options_create();
    if (s == 0) {
        cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
        cairo_font_options_set_antialias(font_options, CAIRO_ANTIALIAS_NONE);
    } else {
        cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
        cairo_font_options_set_antialias(font_options, CAIRO_ANTIALIAS_GRAY);
    }
    cairo_set_font_options(cr, font_options);
    cairo_font_options_destroy(font_options);
    if (semantic_text_active()) {
        cairo_set_antialias(semantic_stroke_coverage_cr,
                            s == 0 ? CAIRO_ANTIALIAS_NONE : CAIRO_ANTIALIAS_DEFAULT);
        semantic_text_set_hard_antialias();
    }
}

void screen_brightness(int v) {
    if (screen_has_io_backend())
        return;

    if (v < 0) {
        v = 0;
    }
    if (v > 15) {
        v = 15;
    }

    // True range of pre-charge voltage, AKA "brightness" is 0-31.
    // Below 16 is too dark for the lowest screen levels, so the range
    // is limited and offset.
    v += 16;

    ssd1322_set_brightness((uint8_t)v);
}

void screen_contrast(int c) {
    if (screen_has_io_backend())
        return;

    if (c < 0) {
        c = 0;
    }
    if (c > 255) {
        c = 255;
    }
    ssd1322_set_contrast((uint8_t)c);
}

void screen_gamma(double g) {
    if (screen_has_io_backend())
        return;

    if (g < 0.0) {
        g = 0;
    }

    ssd1322_set_gamma(g);
}

void screen_invert(int inverted) {
    if (screen_has_io_backend())
        return;
    ssd1322_set_display_mode((inverted != 0) ? SSD1322_DISPLAY_MODE_INVERT : SSD1322_DISPLAY_MODE_NORMAL);
}

void screen_level(int z) {
    z = z < 0 ? 0 : (z > 15 ? 15 : z);
    cairo_set_source_rgb(cr, c[z], c[z], c[z]);
    if (semantic_text_active()) {
        semantic_stroke_level_current = (uint8_t)z;
        cairo_set_source_rgb(semantic_text_cr, c[z], c[z], c[z]);
        cairo_set_source_rgb(semantic_stroke_cr, c[z], c[z], c[z]);
        cairo_set_source_rgb(semantic_stroke_coverage_cr, c[z], c[z], c[z]);
    }
}

void screen_line_width(double w) {
    cairo_set_line_width(cr, w);
    if (semantic_text_active()) {
        cairo_set_line_width(semantic_text_cr, w);
        cairo_set_line_width(semantic_stroke_cr, w);
        cairo_set_line_width(semantic_stroke_coverage_cr, w);
    }
}

void screen_line_cap(const char *style) {
    if (strcmp(style, "round") == 0) {
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        if (semantic_text_active()) {
            cairo_set_line_cap(semantic_text_cr, CAIRO_LINE_CAP_ROUND);
            cairo_set_line_cap(semantic_stroke_cr, CAIRO_LINE_CAP_ROUND);
            cairo_set_line_cap(semantic_stroke_coverage_cr, CAIRO_LINE_CAP_ROUND);
        }
    } else if (strcmp(style, "square") == 0) {
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
        if (semantic_text_active()) {
            cairo_set_line_cap(semantic_text_cr, CAIRO_LINE_CAP_SQUARE);
            cairo_set_line_cap(semantic_stroke_cr, CAIRO_LINE_CAP_SQUARE);
            cairo_set_line_cap(semantic_stroke_coverage_cr, CAIRO_LINE_CAP_SQUARE);
        }
    } else {
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
        if (semantic_text_active()) {
            cairo_set_line_cap(semantic_text_cr, CAIRO_LINE_CAP_BUTT);
            cairo_set_line_cap(semantic_stroke_cr, CAIRO_LINE_CAP_BUTT);
            cairo_set_line_cap(semantic_stroke_coverage_cr, CAIRO_LINE_CAP_BUTT);
        }
    }
}

void screen_line_join(const char *style) {
    if (strcmp(style, "round") == 0) {
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        if (semantic_text_active()) {
            cairo_set_line_join(semantic_text_cr, CAIRO_LINE_JOIN_ROUND);
            cairo_set_line_join(semantic_stroke_cr, CAIRO_LINE_JOIN_ROUND);
            cairo_set_line_join(semantic_stroke_coverage_cr, CAIRO_LINE_JOIN_ROUND);
        }
    } else if (strcmp(style, "bevel") == 0) {
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_BEVEL);
        if (semantic_text_active()) {
            cairo_set_line_join(semantic_text_cr, CAIRO_LINE_JOIN_BEVEL);
            cairo_set_line_join(semantic_stroke_cr, CAIRO_LINE_JOIN_BEVEL);
            cairo_set_line_join(semantic_stroke_coverage_cr, CAIRO_LINE_JOIN_BEVEL);
        }
    } else {
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
        if (semantic_text_active()) {
            cairo_set_line_join(semantic_text_cr, CAIRO_LINE_JOIN_MITER);
            cairo_set_line_join(semantic_stroke_cr, CAIRO_LINE_JOIN_MITER);
            cairo_set_line_join(semantic_stroke_coverage_cr, CAIRO_LINE_JOIN_MITER);
        }
    }
}

void screen_miter_limit(double limit) {
    cairo_set_miter_limit(cr, limit);
    if (semantic_text_active()) {
        cairo_set_miter_limit(semantic_text_cr, limit);
        cairo_set_miter_limit(semantic_stroke_cr, limit);
        cairo_set_miter_limit(semantic_stroke_coverage_cr, limit);
    }
}

void screen_move(double x, double y) {
    cairo_move_to(cr, x, y);
    if (semantic_text_active()) {
        cairo_move_to(semantic_text_cr, x, y);
        cairo_move_to(semantic_stroke_cr, x, y);
        cairo_move_to(semantic_stroke_coverage_cr, x, y);
    }
}

void screen_line(double x, double y) {
    cairo_line_to(cr, x, y);
    if (semantic_text_active()) {
        cairo_line_to(semantic_text_cr, x, y);
        cairo_line_to(semantic_stroke_cr, x, y);
        cairo_line_to(semantic_stroke_coverage_cr, x, y);
    }
}

void screen_line_rel(double x, double y) {
    cairo_rel_line_to(cr, x, y);
    if (semantic_text_active()) {
        cairo_rel_line_to(semantic_text_cr, x, y);
        cairo_rel_line_to(semantic_stroke_cr, x, y);
        cairo_rel_line_to(semantic_stroke_coverage_cr, x, y);
    }
}

void screen_move_rel(double x, double y) {
    cairo_rel_move_to(cr, x, y);
    if (semantic_text_active()) {
        cairo_rel_move_to(semantic_text_cr, x, y);
        cairo_rel_move_to(semantic_stroke_cr, x, y);
        cairo_rel_move_to(semantic_stroke_coverage_cr, x, y);
    }
}

void screen_curve(double x1, double y1, double x2, double y2, double x3, double y3) {
    cairo_curve_to(cr, x1, y1, x2, y2, x3, y3);
    if (semantic_text_active()) {
        cairo_curve_to(semantic_text_cr, x1, y1, x2, y2, x3, y3);
        cairo_curve_to(semantic_stroke_cr, x1, y1, x2, y2, x3, y3);
        cairo_curve_to(semantic_stroke_coverage_cr, x1, y1, x2, y2, x3, y3);
    }
}

void screen_curve_rel(double dx1, double dy1, double dx2, double dy2, double dx3, double dy3) {
    cairo_rel_curve_to(cr, dx1, dy1, dx2, dy2, dx3, dy3);
    if (semantic_text_active()) {
        cairo_rel_curve_to(semantic_text_cr, dx1, dy1, dx2, dy2, dx3, dy3);
        cairo_rel_curve_to(semantic_stroke_cr, dx1, dy1, dx2, dy2, dx3, dy3);
        cairo_rel_curve_to(
            semantic_stroke_coverage_cr, dx1, dy1, dx2, dy2, dx3, dy3);
    }
}

void screen_arc(double x, double y, double r, double a1, double a2) {
    cairo_arc(cr, x, y, r, a1, a2);
    if (semantic_text_active()) {
        cairo_arc(semantic_text_cr, x, y, r, a1, a2);
        cairo_arc(semantic_stroke_cr, x, y, r, a1, a2);
        cairo_arc(semantic_stroke_coverage_cr, x, y, r, a1, a2);
    }
}

void screen_rect(double x, double y, double w, double h) {
    cairo_rectangle(cr, x, y, w, h);
    if (semantic_text_active()) {
        cairo_rectangle(semantic_text_cr, x, y, w, h);
        cairo_rectangle(semantic_stroke_cr, x, y, w, h);
        cairo_rectangle(semantic_stroke_coverage_cr, x, y, w, h);
    }
}

void screen_close_path(void) {
    cairo_close_path(cr);
    if (semantic_text_active()) {
        cairo_close_path(semantic_text_cr);
        cairo_close_path(semantic_stroke_cr);
        cairo_close_path(semantic_stroke_coverage_cr);
    }
}

void screen_stroke(void) {
    const bool draw_semantic_stroke = semantic_text_active();
    const uint8_t semantic_level = draw_semantic_stroke ? semantic_stroke_level() : 0;
    cairo_path_t *primary_path = draw_semantic_stroke ? cairo_copy_path(cr) : NULL;
    cairo_stroke(cr);
    if (draw_semantic_stroke) {
        // The primary Cairo context is authoritative. Rebuild all sidecar
        // paths from it at the consuming operation so an earlier fill/stroke
        // can never leak retained geometry into a later semantic stroke.
        if (primary_path != NULL && primary_path->status == CAIRO_STATUS_SUCCESS)
            semantic_replace_paths(primary_path);
        semantic_text_erase_path(true);
        semantic_stroke_draw(semantic_level);
    }
    if (primary_path != NULL)
        cairo_path_destroy(primary_path);
}

void screen_fill(void) {
    const bool erase_semantics = semantic_text_active();
    cairo_path_t *primary_path = erase_semantics ? cairo_copy_path(cr) : NULL;
    cairo_fill(cr);
    if (erase_semantics) {
        if (primary_path != NULL && primary_path->status == CAIRO_STATUS_SUCCESS)
            semantic_replace_paths(primary_path);
        semantic_text_erase_path(false);
        semantic_stroke_erase_path(false);
    }
    if (primary_path != NULL)
        cairo_path_destroy(primary_path);
}

void screen_text(const char *s) {
    cairo_show_text(cr, s);
    if (semantic_text_active())
        semantic_show_text(s);
}

void screen_clear(void) {
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    if (semantic_text_active()) {
        semantic_text_clear();
        cairo_set_operator(semantic_text_cr, CAIRO_OPERATOR_OVER);
        cairo_set_operator(semantic_stroke_cr, CAIRO_OPERATOR_OVER);
        cairo_set_operator(semantic_stroke_coverage_cr, CAIRO_OPERATOR_OVER);
        semantic_surfaces_valid = true;
    }
    surface_may_have_color = false;
}

void screen_text_extents(const char *s) {
    cairo_text_extents_t extents;
    cairo_text_extents(cr, s, &extents);
    union screen_results_data *results = screen_results_data_new(SCREEN_RESULTS_TEXT_EXTENTS);
    results->text_extents.x_bearing = extents.x_bearing;
    results->text_extents.y_bearing = extents.y_bearing;
    results->text_extents.width = extents.width;
    results->text_extents.height = extents.height;
    results->text_extents.x_advance = extents.x_advance;
    results->text_extents.y_advance = extents.y_advance;
    screen_results_post(results);
}

extern void screen_export_png(const char *s) {
    cairo_surface_write_to_png(surface, s);
}

extern void screen_export_screenshot(const char *s) {
    static cairo_surface_t *png;
    static cairo_t *temp; // for bg fill
    // width = 640 (128*4 pixels with 64 pixel black border)
    // hieght = 384 (64*4 pixels plus border)
    png = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 640, 384);
    temp = cairo_create(png);
    // fill
    cairo_set_source_rgb(temp, 0, 0, 0);
    cairo_rectangle(temp, 0, 0, 640, 384);
    cairo_fill(temp);
    // copy big pixles
    uint32_t *src = (uint32_t *)cairo_image_surface_get_data(surface);
    uint32_t *dst = (uint32_t *)cairo_image_surface_get_data(png);
    if (!src || !dst)
        return;
    dst += 64 + 640 * 64;
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 128; x++) {
            // FIXME: needs some sort of gamma correction?
            uint32_t p = *src++ | 0xFF000000; // FF for alpha
            for (int xx = 0; xx < 4; xx++) {
                *(dst + 1920) = p;
                *(dst + 1280) = p;
                *(dst + 640) = p;
                *dst++ = p;
            }
        }
        dst += 640 * 4 - 128 * 4;
    }
    // cleanup
    cairo_destroy(temp);
    cairo_surface_write_to_png(png, s);
    cairo_surface_destroy((cairo_surface_t *)png);
}

void screen_display_png(const char *filename, double x, double y) {
    int img_w, img_h;

    image = cairo_image_surface_create_from_png(filename);

    if (cairo_surface_status(image)) {
        fprintf(stderr, "display_png: %s\n", cairo_status_to_string(cairo_surface_status(image)));
        return;
    }

    surface_may_have_color = true;

    img_w = cairo_image_surface_get_width(image);
    img_h = cairo_image_surface_get_height(image);

    cairo_save(cr);
    cairo_set_source_surface(cr, image, x, y);
    cairo_rectangle(cr, x, y, img_w, img_h);
    cairo_fill(cr);
    cairo_surface_destroy(image);
    cairo_restore(cr);
    if (semantic_text_active())
        semantic_text_erase_rectangle(x, y, img_w, img_h);
}

void screen_peek(int x, int y, int w, int h) {
    w = (w <= (128 - x)) ? w : (128 - x);
    h = (h <= (64 - y)) ? h : (64 - y);
    char *buf = (char *)malloc(w * h);
    if (!buf) {
        return;
    }
    cairo_surface_flush(surface);
    uint32_t *data = (uint32_t *)cairo_image_surface_get_data(surface);
    if (!data) {
        fprintf(stderr, "ERROR: screen_peek: no data\n");
        free(buf);
        return;
    }
    char *p = buf;
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            *p = data[j * 128 + i] & 0xF;
            p++;
        }
    }
    union screen_results_data *results = screen_results_data_new(SCREEN_RESULTS_PEEK);
    results->peek.w = w;
    results->peek.h = h;
    results->peek.buf = buf;
    screen_results_post(results);
}

void screen_poke(int x, int y, int w, int h, unsigned char *buf) {
    w = (w <= (128 - x)) ? w : (128 - x);
    h = (h <= (64 - y)) ? h : (64 - y);
    // NB: peek/poke do not actually access the CR,
    // but we do want to avoid torn values
    uint32_t *data = (uint32_t *)cairo_image_surface_get_data(surface);
    if (!data) {
        return;
    }
    uint8_t *p = buf;
    uint32_t pixel;
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            pixel = *p;
            pixel = pixel | (pixel << 4);
            data[j * 128 + i] = pixel | (pixel << 8) | (pixel << 16) | (pixel << 24);
            p++;
        }
    }
    cairo_surface_mark_dirty(surface);
    if (semantic_text_active())
        semantic_text_erase_raw_rectangle(x, y, w, h);
}

void screen_rotate(double r) {
    cairo_rotate(cr, r);
    if (semantic_text_active()) {
        cairo_rotate(semantic_text_cr, r);
        cairo_rotate(semantic_stroke_cr, r);
        cairo_rotate(semantic_stroke_coverage_cr, r);
    }
}

void screen_translate(double x, double y) {
    cairo_translate(cr, x, y);
    if (semantic_text_active()) {
        cairo_translate(semantic_text_cr, x, y);
        cairo_translate(semantic_stroke_cr, x, y);
        cairo_translate(semantic_stroke_coverage_cr, x, y);
    }
}

void screen_set_operator(int i) {
    if (0 <= i && i < NUM_OPS) {
        cairo_set_operator(cr, ops[i]);
        if (semantic_text_active()) {
            cairo_set_operator(semantic_text_cr, ops[i]);
            cairo_set_operator(semantic_stroke_cr, ops[i]);
            cairo_set_operator(semantic_stroke_coverage_cr, ops[i]);
            if (i != 0 && i != 15)
                semantic_surfaces_valid = false;
        }
    }
}

screen_surface_t *screen_surface_new(double width, double height) {
    int w = (int)floor(width);
    int h = (int)floor(height);
    cairo_format_t format = CAIRO_FORMAT_ARGB32;
    cairo_surface_t *image = cairo_image_surface_create(format, w, h);
    cairo_status_t status = cairo_surface_status(image);
    if (status == CAIRO_STATUS_SUCCESS) {
        return (screen_surface_t *)image;
    }
    fprintf(stderr, "surface_new: %s (%d)\n", cairo_status_to_string(status), status);
    return NULL;
}

screen_surface_t *screen_surface_load_png(const char *filename) {
    cairo_surface_t *image = cairo_image_surface_create_from_png(filename);
    if (cairo_surface_status(image)) {
        fprintf(stderr, "load_png: %s\n", cairo_status_to_string(cairo_surface_status(image)));
        return NULL;
    }
    return (screen_surface_t *)image;
}

void screen_surface_free(screen_surface_t *s) {
    cairo_surface_destroy((cairo_surface_t *)s);
}

void screen_surface_get_extents(screen_surface_t *s) {
    union screen_results_data *results = screen_results_data_new(SCREEN_RESULTS_SURFACE_GET_EXTENTS);
    cairo_surface_t *image = (cairo_surface_t *)s;

    results->surface_get_extents.extents.width = cairo_image_surface_get_width(image);
    results->surface_get_extents.extents.height = cairo_image_surface_get_height(image);
    screen_results_post(results);
}

void screen_surface_display(screen_surface_t *s, double x, double y) {
    cairo_surface_t *image = (cairo_surface_t *)s;

    surface_may_have_color = true;

    int width = cairo_image_surface_get_width(image);
    int height = cairo_image_surface_get_height(image);

    cairo_save(cr);
    cairo_set_source_surface(cr, image, x, y);
    cairo_rectangle(cr, x, y, width, height);
    cairo_fill(cr);
    cairo_restore(cr);
    if (semantic_text_active())
        semantic_text_erase_rectangle(x, y, width, height);
}

void screen_surface_display_region(screen_surface_t *s,
                                   double left, double top, double width, double height,
                                   double x, double y) {
    cairo_surface_t *image = (cairo_surface_t *)s;
    cairo_save(cr);
    cairo_set_source_surface(cr, image, -left + x, -top + y);
    cairo_rectangle(cr, x, y, width, height);
    cairo_fill(cr);
    cairo_restore(cr);
    if (semantic_text_active())
        semantic_text_erase_rectangle(x, y, width, height);
}

void screen_context_new(screen_surface_t *target) {
    union screen_results_data *results = screen_results_data_new(SCREEN_RESULTS_CONTEXT_NEW);
    cairo_surface_t *image = (cairo_surface_t *)target;
    // NOTE: cairo_create increases the ref count on image which avoids a double
    // free and allows the image and context to be freed in any order
    cairo_t *context = cairo_create(image);
    cairo_status_t status = cairo_status(context);
    if (status != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "context_new: %s (%d)\n", cairo_status_to_string(status), status);
        context = NULL;
    }

    results->context_new.context = (screen_context_t *)context;
    screen_results_post(results);
}

void screen_context_free(screen_context_t *context) {
    cairo_destroy((cairo_t *)context);
}

void screen_context_get_current() {
    union screen_results_data *results = screen_results_data_new(SCREEN_RESULTS_CONTEXT_GET_CURRENT);
    results->context_get_current.context = (screen_context_t *)cr;
    screen_results_post(results);
}

inline static void _screen_context_set(cairo_t *cr_incoming) {
    // return early if attempting to assign the same context
    if (cr_incoming == cr) {
        return;
    }

    // manage ref count for contexts which aren't the primary context
    if (cr_incoming != cr_primary) {
        cairo_reference(cr_incoming);
    }

    // decrement ref count on current context if it is not the primary
    if (cr != cr_primary) {
        cairo_destroy(cr);
    }

    cr = cr_incoming;
}

void screen_context_set(const screen_context_t *context) {
    _screen_context_set((cairo_t *)context);
}

void screen_context_set_primary(void) {
    _screen_context_set(cr_primary);
}

const screen_context_t *screen_context_get_primary(void) {
    return (const screen_context_t *)cr_primary;
}
