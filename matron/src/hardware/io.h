#pragma once

#include <stdbool.h>
#include <cairo.h>
#include <lualib.h>
#include <sys/queue.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _matron_io_type {
    IO_SCREEN,
    IO_INPUT,
    IO_END,
} matron_io_type_t;

struct _matron_io;

typedef struct _io_ops {
    const char *name;
    matron_io_type_t type;
    size_t data_size;

    int (*config)(struct _matron_io *io, lua_State *l);
    int (*setup)(struct _matron_io *io);
    void (*destroy)(struct _matron_io *io);
} io_ops_t;

extern io_ops_t *io_types[];

int io_create(lua_State *l, io_ops_t *ops);
int io_setup_all(void);
void io_destroy_all(void);

typedef struct _matron_io {
    void *data;
    io_ops_t *ops;

    TAILQ_ENTRY(_matron_io)
    entries;
} matron_io_t;

extern TAILQ_HEAD(io_head, _matron_io) io_queue;

//// screen
struct _matron_fb;

typedef struct _screen_semantics {
    // Transparent ARGB32 surface containing only text. Glyph coverage is
    // deliberately rendered without antialiasing; RGB stores the norns level.
    cairo_surface_t *text;
    // Transparent ARGB32 surface containing the already-styled physical
    // stroke mask. The backend maps it to the output viewport as-is.
    cairo_surface_t *stroke;
    // Transparent ARGB32 mask covering the canonical stroke footprint at the
    // logical framebuffer resolution. It owns all physical pixels mapped from
    // a touched Cairo pixel so the framebuffer stroke cannot leak through the
    // procedural pattern.
    cairo_surface_t *stroke_coverage;
    // False asks a backend to fall back to the canonical framebuffer for a
    // frame containing blend operations the semantic layers cannot reproduce.
    const bool *valid;
} screen_semantics_t;

typedef struct _screen_ops {
    io_ops_t io_ops;

    void (*paint)(struct _matron_fb *fb);
    void (*bind)(struct _matron_fb *fb, cairo_surface_t *surface);
    void (*bind_semantics)(struct _matron_fb *fb, const screen_semantics_t *semantics);
} screen_ops_t;

typedef struct _matron_fb {
    matron_io_t io;

    cairo_t *cairo;
    cairo_surface_t *surface;
} matron_fb_t;

//// keys & encs
struct _matron_input;

typedef struct _input_ops {
    io_ops_t io_ops;

    void *(*poll)(void *input);
} input_ops_t;

typedef struct _matron_input {
    matron_io_t io;

    pthread_t poll_thread;
} matron_input_t;

#ifdef __cplusplus
}
#endif
