#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <cairo.h>

#include "event_types.h"
#include "events.h"
#include "hardware/io.h"
#include "hardware/screen/screens.h"
#include "hardware/screen/sharp_memory_dither.h"

typedef struct _screen_sharp_memory_priv {
    char *dev;
    int fb_fd;
    uint8_t *frame;
    size_t frame_size;
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    cairo_surface_t *source;
    int fps;
    bool invert;
    bool stretch;
    volatile bool refresh_running;
    pthread_t refresh_thread;
    pthread_mutex_t paint_lock;
    bool write_error_reported;
} screen_sharp_memory_priv_t;

static int screen_sharp_memory_config(matron_io_t *io, lua_State *l);
static int screen_sharp_memory_setup(matron_io_t *io);
static void screen_sharp_memory_destroy(matron_io_t *io);
static void screen_sharp_memory_paint(matron_fb_t *fb);
static void screen_sharp_memory_bind(matron_fb_t *fb, cairo_surface_t *surface);

screen_ops_t screen_sharp_memory_ops = {
    .io_ops.name = "screen:sharp-memory",
    .io_ops.type = IO_SCREEN,
    .io_ops.data_size = sizeof(screen_sharp_memory_priv_t),
    .io_ops.config = screen_sharp_memory_config,
    .io_ops.setup = screen_sharp_memory_setup,
    .io_ops.destroy = screen_sharp_memory_destroy,

    .paint = screen_sharp_memory_paint,
    .bind = screen_sharp_memory_bind,
};

static int config_string(lua_State *l, int table, const char *name, char **value, const char *fallback) {
    lua_getfield(l, table, name);
    const char *configured = fallback;
    if (lua_isstring(l, -1)) {
        configured = lua_tostring(l, -1);
    } else if (!lua_isnil(l, -1)) {
        fprintf(stderr, "ERROR (screen:sharp-memory) config option '%s' should be a string\n", name);
        lua_pop(l, 1);
        return -1;
    }

    *value = (char *)malloc(strlen(configured) + 1);
    if (*value == NULL) {
        fprintf(stderr, "ERROR (screen:sharp-memory) no memory for '%s'\n", name);
        lua_pop(l, 1);
        return -1;
    }
    strcpy(*value, configured);
    lua_pop(l, 1);
    return 0;
}

static int config_integer(lua_State *l, int table, const char *name, int fallback) {
    lua_getfield(l, table, name);
    int value = fallback;
    if (lua_isnumber(l, -1))
        value = (int)lua_tointeger(l, -1);
    lua_pop(l, 1);
    return value;
}

static bool config_boolean(lua_State *l, int table, const char *name, bool fallback) {
    lua_getfield(l, table, name);
    bool value = fallback;
    if (lua_isboolean(l, -1))
        value = lua_toboolean(l, -1);
    lua_pop(l, 1);
    return value;
}

static int screen_sharp_memory_config(matron_io_t *io, lua_State *l) {
    screen_sharp_memory_priv_t *priv = (screen_sharp_memory_priv_t *)io->data;
    memset(priv, 0, sizeof(*priv));
    priv->fb_fd = -1;

    if (config_string(l, 2, "dev", &priv->dev, "/dev/fb0") != 0)
        return -1;

    priv->fps = config_integer(l, 2, "fps", 15);
    if (priv->fps < 1 || priv->fps > 60) {
        fprintf(stderr, "ERROR (screen:sharp-memory) 'fps' must be between 1 and 60\n");
        free(priv->dev);
        priv->dev = NULL;
        return -1;
    }

    priv->invert = config_boolean(l, 2, "invert", false);
    priv->stretch = config_boolean(l, 2, "stretch", false);
    return 0;
}

static void *screen_sharp_memory_refresh(void *data) {
    screen_sharp_memory_priv_t *priv = (screen_sharp_memory_priv_t *)data;
    struct timespec tick = {
        .tv_sec = 0,
        .tv_nsec = 1000000000L / priv->fps,
    };

    while (priv->refresh_running) {
        event_post(event_data_new(EVENT_SCREEN_REFRESH));
        clock_nanosleep(CLOCK_MONOTONIC, 0, &tick, NULL);
    }
    return NULL;
}

static bool framebuffer_format_supported(const struct fb_var_screeninfo *vinfo) {
    if (vinfo->bits_per_pixel != 16 && vinfo->bits_per_pixel != 24 && vinfo->bits_per_pixel != 32)
        return false;
    return vinfo->red.length > 0 && vinfo->green.length > 0 && vinfo->blue.length > 0;
}

static int screen_sharp_memory_setup(matron_io_t *io) {
    matron_fb_t *fb = (matron_fb_t *)io;
    screen_sharp_memory_priv_t *priv = (screen_sharp_memory_priv_t *)io->data;
    fb->surface = NULL;
    fb->cairo = NULL;

    priv->fb_fd = open(priv->dev, O_RDWR | O_CLOEXEC);
    if (priv->fb_fd < 0) {
        fprintf(stderr, "ERROR (screen:sharp-memory) cannot open %s: %s\n", priv->dev, strerror(errno));
        return -1;
    }

    if (ioctl(priv->fb_fd, FBIOGET_VSCREENINFO, &priv->vinfo) < 0 ||
        ioctl(priv->fb_fd, FBIOGET_FSCREENINFO, &priv->finfo) < 0) {
        fprintf(stderr, "ERROR (screen:sharp-memory) cannot read framebuffer information for %s: %s\n",
                priv->dev, strerror(errno));
        close(priv->fb_fd);
        priv->fb_fd = -1;
        return -1;
    }

    if (!framebuffer_format_supported(&priv->vinfo)) {
        fprintf(stderr,
                "ERROR (screen:sharp-memory) %s uses unsupported %u-bit framebuffer format\n",
                priv->dev, priv->vinfo.bits_per_pixel);
        close(priv->fb_fd);
        priv->fb_fd = -1;
        return -1;
    }

    const uint64_t required_size =
        (uint64_t)priv->finfo.line_length * (priv->vinfo.yoffset + priv->vinfo.yres);
    if (required_size == 0 || required_size > SIZE_MAX ||
        (priv->finfo.smem_len != 0 && required_size > priv->finfo.smem_len)) {
        fprintf(stderr, "ERROR (screen:sharp-memory) invalid framebuffer size for %s\n", priv->dev);
        close(priv->fb_fd);
        priv->fb_fd = -1;
        return -1;
    }

    priv->frame_size = (size_t)required_size;
    priv->frame = (uint8_t *)calloc(priv->frame_size, 1);
    if (priv->frame == NULL) {
        fprintf(stderr, "ERROR (screen:sharp-memory) cannot allocate framebuffer copy\n");
        close(priv->fb_fd);
        priv->fb_fd = -1;
        return -1;
    }

    if (pthread_mutex_init(&priv->paint_lock, NULL) != 0) {
        fprintf(stderr, "ERROR (screen:sharp-memory) cannot initialize paint lock\n");
        free(priv->frame);
        priv->frame = NULL;
        close(priv->fb_fd);
        priv->fb_fd = -1;
        return -1;
    }

    priv->refresh_running = true;
    if (pthread_create(&priv->refresh_thread, NULL, &screen_sharp_memory_refresh, priv) != 0) {
        fprintf(stderr, "ERROR (screen:sharp-memory) cannot start refresh thread\n");
        priv->refresh_running = false;
        pthread_mutex_destroy(&priv->paint_lock);
        free(priv->frame);
        priv->frame = NULL;
        close(priv->fb_fd);
        priv->fb_fd = -1;
        return -1;
    }

    fprintf(stderr,
            "screen:sharp-memory: %s %ux%u, %u bpp, %d fps, dither=bayer8, invert=%s, stretch=%s\n",
            priv->dev, priv->vinfo.xres, priv->vinfo.yres, priv->vinfo.bits_per_pixel, priv->fps,
            priv->invert ? "yes" : "no", priv->stretch ? "yes" : "no");
    return 0;
}

static void screen_sharp_memory_destroy(matron_io_t *io) {
    screen_sharp_memory_priv_t *priv = (screen_sharp_memory_priv_t *)io->data;

    if (priv->refresh_running) {
        priv->refresh_running = false;
        pthread_join(priv->refresh_thread, NULL);
    }
    if (priv->source != NULL) {
        cairo_surface_destroy(priv->source);
        priv->source = NULL;
    }
    if (priv->frame != NULL) {
        pthread_mutex_destroy(&priv->paint_lock);
        free(priv->frame);
        priv->frame = NULL;
    }
    if (priv->fb_fd >= 0) {
        close(priv->fb_fd);
        priv->fb_fd = -1;
    }
    free(priv->dev);
    priv->dev = NULL;
}

static uint64_t channel_mask(struct fb_bitfield field) {
    if (field.length == 0)
        return 0;
    if (field.length >= 64)
        return UINT64_MAX;
    return ((UINT64_C(1) << field.length) - 1) << field.offset;
}

static void framebuffer_set_pixel(screen_sharp_memory_priv_t *priv, uint32_t x, uint32_t y, bool white) {
    const uint32_t bytes_per_pixel = (priv->vinfo.bits_per_pixel + 7) / 8;
    const size_t offset = (size_t)(y + priv->vinfo.yoffset) * priv->finfo.line_length +
                          (size_t)(x + priv->vinfo.xoffset) * bytes_per_pixel;
    if (offset + bytes_per_pixel > priv->frame_size)
        return;

    uint64_t pixel = 0;
    if (white) {
        pixel = channel_mask(priv->vinfo.red) |
                channel_mask(priv->vinfo.green) |
                channel_mask(priv->vinfo.blue) |
                channel_mask(priv->vinfo.transp);
    }
    memcpy(priv->frame + offset, &pixel, bytes_per_pixel);
}

static bool framebuffer_write(screen_sharp_memory_priv_t *priv) {
    size_t remaining = priv->frame_size;
    size_t offset = 0;
    while (remaining > 0) {
        const ssize_t written = pwrite(priv->fb_fd, priv->frame + offset, remaining, (off_t)offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
        offset += (size_t)written;
        remaining -= (size_t)written;
    }
    return true;
}

static void screen_sharp_memory_paint(matron_fb_t *fb) {
    screen_sharp_memory_priv_t *priv = (screen_sharp_memory_priv_t *)fb->io.data;
    if (priv->source == NULL || priv->frame == NULL || priv->fb_fd < 0)
        return;

    pthread_mutex_lock(&priv->paint_lock);
    cairo_surface_flush(priv->source);

    const uint32_t source_width = (uint32_t)cairo_image_surface_get_width(priv->source);
    const uint32_t source_height = (uint32_t)cairo_image_surface_get_height(priv->source);
    const int source_stride = cairo_image_surface_get_stride(priv->source);
    const uint8_t *source_data = cairo_image_surface_get_data(priv->source);
    const sharp_memory_viewport_t viewport = sharp_memory_fit_viewport(
        source_width, source_height, priv->vinfo.xres, priv->vinfo.yres, priv->stretch);

    if (source_data != NULL && source_width > 0 && source_height > 0) {
        for (uint32_t y = 0; y < priv->vinfo.yres; ++y) {
            for (uint32_t x = 0; x < priv->vinfo.xres; ++x) {
                bool white = priv->invert;
                if (x >= viewport.x && x < viewport.x + viewport.width &&
                    y >= viewport.y && y < viewport.y + viewport.height) {
                    const uint32_t source_x =
                        (uint32_t)(((uint64_t)(x - viewport.x) * source_width) / viewport.width);
                    const uint32_t source_y =
                        (uint32_t)(((uint64_t)(y - viewport.y) * source_height) / viewport.height);
                    uint32_t argb;
                    memcpy(&argb, source_data + (size_t)source_y * source_stride + source_x * 4, 4);
                    white = sharp_memory_dither_is_white(
                        sharp_memory_luminance(argb), x - viewport.x, y - viewport.y, priv->invert);
                }
                framebuffer_set_pixel(priv, x, y, white);
            }
        }

        if (!framebuffer_write(priv)) {
            if (!priv->write_error_reported) {
                fprintf(stderr, "ERROR (screen:sharp-memory) writing %s: %s\n", priv->dev, strerror(errno));
                priv->write_error_reported = true;
            }
        } else {
            priv->write_error_reported = false;
        }
    }

    pthread_mutex_unlock(&priv->paint_lock);
}

static void screen_sharp_memory_bind(matron_fb_t *fb, cairo_surface_t *surface) {
    screen_sharp_memory_priv_t *priv = (screen_sharp_memory_priv_t *)fb->io.data;
    if (priv->source != NULL)
        cairo_surface_destroy(priv->source);
    priv->source = cairo_surface_reference(surface);
}
