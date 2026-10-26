#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
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
#include "hardware/screen/sharp_memory_protocol.h"

#define SHARP_MEMORY_WIDTH 400
#define SHARP_MEMORY_HEIGHT 240
#define SHARP_MEMORY_ROWS_PER_TRANSFER 60
#define SHARP_MEMORY_DEFAULT_SPEED_HZ 2000000
#define SHARP_MEMORY_DUMP_TRIGGER "/tmp/dump-sharp-frame"
#define SHARP_MEMORY_DUMP_PATH "/tmp/sharp-frame.pbm"
#define SHARP_MEMORY_DUMP_TEMP_PATH "/tmp/sharp-frame.pbm.tmp"

typedef struct _screen_sharp_memory_priv {
    char *dev;
    char *cs_chip;
    unsigned int cs_line_offset;
    int spi_fd;
    int cs_fd;
    uint8_t *frame;
    uint8_t *tx_frame;
    size_t frame_size;
    uint8_t *packet;
    size_t packet_size;
    cairo_surface_t *source;
    int fps;
    uint32_t speed_hz;
    bool invert;
    bool stretch;
    bool vcom;
    volatile bool refresh_running;
    pthread_t refresh_thread;
    pthread_mutex_t paint_lock;
    bool frame_dirty;
    bool write_error_reported;
} screen_sharp_memory_priv_t;

static int screen_sharp_memory_config(matron_io_t *io, lua_State *l);
static int screen_sharp_memory_setup(matron_io_t *io);
static void screen_sharp_memory_destroy(matron_io_t *io);
static void screen_sharp_memory_paint(matron_fb_t *fb);
static void screen_sharp_memory_bind(matron_fb_t *fb, cairo_surface_t *surface);

screen_ops_t screen_sharp_memory_ops = {
    {
        // .io_ops
        "screen:sharp-memory",                // .name
        IO_SCREEN,                            // .type
        sizeof(screen_sharp_memory_priv_t),   // .data_size
        screen_sharp_memory_config,           // .config
        screen_sharp_memory_setup,            // .setup
        screen_sharp_memory_destroy,          // .destroy
    },
    screen_sharp_memory_paint, // .paint
    screen_sharp_memory_bind,  // .bind
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
    priv->spi_fd = -1;
    priv->cs_fd = -1;

    if (config_string(l, 2, "dev", &priv->dev, "/dev/spidev0.0") != 0)
        return -1;
    if (config_string(l, 2, "cs_chip", &priv->cs_chip, "pinctrl-rp1") != 0) {
        free(priv->dev);
        priv->dev = NULL;
        return -1;
    }

    const int cs_line = config_integer(l, 2, "cs_line", 25);
    if (cs_line < 0) {
        fprintf(stderr, "ERROR (screen:sharp-memory) 'cs_line' must be non-negative\n");
        free(priv->cs_chip);
        free(priv->dev);
        priv->cs_chip = NULL;
        priv->dev = NULL;
        return -1;
    }
    priv->cs_line_offset = (unsigned int)cs_line;

    priv->fps = config_integer(l, 2, "fps", 15);
    if (priv->fps < 1 || priv->fps > 20) {
        fprintf(stderr, "ERROR (screen:sharp-memory) 'fps' must be between 1 and 20\n");
        free(priv->cs_chip);
        free(priv->dev);
        priv->cs_chip = NULL;
        priv->dev = NULL;
        return -1;
    }

    const int speed_hz = config_integer(l, 2, "speed_hz", SHARP_MEMORY_DEFAULT_SPEED_HZ);
    if (speed_hz < 100000 || speed_hz > SHARP_MEMORY_DEFAULT_SPEED_HZ) {
        fprintf(stderr, "ERROR (screen:sharp-memory) 'speed_hz' must be between 100000 and 2000000\n");
        free(priv->cs_chip);
        free(priv->dev);
        priv->cs_chip = NULL;
        priv->dev = NULL;
        return -1;
    }
    priv->speed_hz = (uint32_t)speed_hz;

    priv->invert = config_boolean(l, 2, "invert", false);
    priv->stretch = config_boolean(l, 2, "stretch", false);
    return 0;
}

static bool spi_write_transaction(screen_sharp_memory_priv_t *priv, const uint8_t *data, size_t size) {
    struct gpio_v2_line_values cs_value = {};
    cs_value.mask = 1;
    cs_value.bits = 1;
    if (ioctl(priv->cs_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &cs_value) < 0)
        return false;

    ssize_t written;
    do {
        written = write(priv->spi_fd, data, size);
    } while (written < 0 && errno == EINTR);

    int transaction_errno = 0;
    if (written < 0)
        transaction_errno = errno;
    else if ((size_t)written != size)
        transaction_errno = EIO;

    cs_value.bits = 0;
    if (ioctl(priv->cs_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &cs_value) < 0 &&
        transaction_errno == 0)
        transaction_errno = errno;

    if (transaction_errno != 0) {
        errno = transaction_errno;
        return false;
    }
    return true;
}

static bool send_command(screen_sharp_memory_priv_t *priv, uint8_t command) {
    uint8_t packet[2];
    const size_t packet_size =
        sharp_memory_encode_command(packet, sizeof(packet), command, priv->vcom);
    return packet_size != 0 && spi_write_transaction(priv, packet, packet_size);
}

static bool send_frame(screen_sharp_memory_priv_t *priv, const uint8_t *frame) {
    for (uint32_t first_row = 0; first_row < SHARP_MEMORY_HEIGHT;
         first_row += SHARP_MEMORY_ROWS_PER_TRANSFER) {
        uint32_t row_count = SHARP_MEMORY_HEIGHT - first_row;
        if (row_count > SHARP_MEMORY_ROWS_PER_TRANSFER)
            row_count = SHARP_MEMORY_ROWS_PER_TRANSFER;

        const size_t encoded = sharp_memory_encode_rows(priv->packet,
                                                        priv->packet_size,
                                                        frame,
                                                        SHARP_MEMORY_WIDTH,
                                                        first_row,
                                                        row_count,
                                                        priv->vcom);
        if (encoded == 0 || !spi_write_transaction(priv, priv->packet, encoded))
            return false;
    }
    return true;
}

static void report_write_error(screen_sharp_memory_priv_t *priv) {
    if (!priv->write_error_reported) {
        fprintf(stderr, "ERROR (screen:sharp-memory) writing %s: %s\n", priv->dev, strerror(errno));
        priv->write_error_reported = true;
    }
}

static void advance_deadline(struct timespec *deadline, long nanoseconds) {
    deadline->tv_nsec += nanoseconds;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec += deadline->tv_nsec / 1000000000L;
        deadline->tv_nsec %= 1000000000L;
    }
}

static bool deadline_reached(const struct timespec *deadline, const struct timespec *now) {
    return deadline->tv_sec < now->tv_sec ||
           (deadline->tv_sec == now->tv_sec && deadline->tv_nsec <= now->tv_nsec);
}

static void *screen_sharp_memory_refresh(void *data) {
    screen_sharp_memory_priv_t *priv = (screen_sharp_memory_priv_t *)data;
    const long tick_ns = 1000000000L / priv->fps;
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    int ticks_until_vcom = priv->fps;
    bool tx_pending = false;
    bool vcom_pending = false;

    while (priv->refresh_running) {
        pthread_mutex_lock(&priv->paint_lock);
        if (priv->frame_dirty) {
            uint8_t *next_frame = priv->frame;
            priv->frame = priv->tx_frame;
            priv->tx_frame = next_frame;
            priv->frame_dirty = false;
            tx_pending = true;
        }
        pthread_mutex_unlock(&priv->paint_lock);

        if (--ticks_until_vcom <= 0) {
            priv->vcom = !priv->vcom;
            vcom_pending = true;
            ticks_until_vcom = priv->fps;
        }

        bool write_ok = true;
        if (tx_pending) {
            write_ok = send_frame(priv, priv->tx_frame);
        } else if (vcom_pending) {
            write_ok = send_command(priv, 0);
        }

        if (!write_ok) {
            report_write_error(priv);
        } else if (tx_pending || vcom_pending) {
            tx_pending = false;
            vcom_pending = false;
            priv->write_error_reported = false;
        }

        event_post(event_data_new(EVENT_SCREEN_REFRESH));

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        do {
            advance_deadline(&deadline, tick_ns);
        } while (deadline_reached(&deadline, &now));

        int sleep_result;
        do {
            sleep_result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
        } while (sleep_result == EINTR && priv->refresh_running);
    }
    return NULL;
}

static void close_spi(screen_sharp_memory_priv_t *priv) {
    if (priv->spi_fd >= 0) {
        close(priv->spi_fd);
        priv->spi_fd = -1;
    }
}

static void close_cs(screen_sharp_memory_priv_t *priv) {
    if (priv->cs_fd >= 0) {
        struct gpio_v2_line_values cs_value = {};
        cs_value.mask = 1;
        ioctl(priv->cs_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &cs_value);
        close(priv->cs_fd);
        priv->cs_fd = -1;
    }
}

static int open_gpio_chip(const char *selector) {
    if (selector[0] == '/')
        return open(selector, O_RDWR | O_CLOEXEC);

    char path[64];
    const int path_length = snprintf(path, sizeof(path), "/dev/%s", selector);
    if (path_length > 0 && path_length < (int)sizeof(path)) {
        const int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0)
            return fd;
    }

    for (unsigned int index = 0; index < 256; ++index) {
        snprintf(path, sizeof(path), "/dev/gpiochip%u", index);
        const int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;

        struct gpiochip_info info = {};
        if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0 &&
            (strcmp(selector, info.name) == 0 || strcmp(selector, info.label) == 0))
            return fd;
        close(fd);
    }

    errno = ENODEV;
    return -1;
}

static int configure_cs(screen_sharp_memory_priv_t *priv) {
    const int chip_fd = open_gpio_chip(priv->cs_chip);
    if (chip_fd < 0) {
        fprintf(stderr, "ERROR (screen:sharp-memory) cannot find GPIO chip '%s': %s\n",
                priv->cs_chip, strerror(errno));
        return -1;
    }

    struct gpio_v2_line_request request = {};
    request.offsets[0] = priv->cs_line_offset;
    request.num_lines = 1;
    request.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    request.config.num_attrs = 1;
    request.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    request.config.attrs[0].mask = 1;
    strncpy(request.consumer, "norns-sharp-memory-cs", sizeof(request.consumer) - 1);

    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &request) < 0) {
        const int request_errno = errno;
        close(chip_fd);
        fprintf(stderr, "ERROR (screen:sharp-memory) cannot use %s line %u for CS: %s\n",
                priv->cs_chip, priv->cs_line_offset, strerror(request_errno));
        errno = request_errno;
        return -1;
    }

    close(chip_fd);
    priv->cs_fd = request.fd;
    return 0;
}

static int configure_spi(screen_sharp_memory_priv_t *priv) {
    uint8_t mode = SPI_MODE_0;
    uint8_t bits_per_word = 8;

    if (ioctl(priv->spi_fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(priv->spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits_per_word) < 0 ||
        ioctl(priv->spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &priv->speed_hz) < 0) {
        fprintf(stderr, "ERROR (screen:sharp-memory) cannot configure %s: %s\n",
                priv->dev, strerror(errno));
        return -1;
    }

    return 0;
}

static int screen_sharp_memory_setup(matron_io_t *io) {
    matron_fb_t *fb = (matron_fb_t *)io;
    screen_sharp_memory_priv_t *priv = (screen_sharp_memory_priv_t *)io->data;
    fb->surface = NULL;
    fb->cairo = NULL;

    priv->spi_fd = open(priv->dev, O_RDWR | O_CLOEXEC);
    if (priv->spi_fd < 0) {
        fprintf(stderr, "ERROR (screen:sharp-memory) cannot open %s: %s\n", priv->dev, strerror(errno));
        return -1;
    }
    if (configure_spi(priv) != 0) {
        close_spi(priv);
        return -1;
    }
    if (configure_cs(priv) != 0) {
        close_spi(priv);
        return -1;
    }

    priv->frame_size = (SHARP_MEMORY_WIDTH * SHARP_MEMORY_HEIGHT) / 8;
    priv->packet_size = sharp_memory_encoded_rows_size(
        SHARP_MEMORY_WIDTH, SHARP_MEMORY_ROWS_PER_TRANSFER);
    priv->frame = (uint8_t *)malloc(priv->frame_size);
    priv->tx_frame = (uint8_t *)malloc(priv->frame_size);
    priv->packet = (uint8_t *)malloc(priv->packet_size);
    if (priv->frame == NULL || priv->tx_frame == NULL || priv->packet == NULL) {
        fprintf(stderr, "ERROR (screen:sharp-memory) cannot allocate display buffers\n");
        free(priv->packet);
        free(priv->tx_frame);
        free(priv->frame);
        priv->packet = NULL;
        priv->tx_frame = NULL;
        priv->frame = NULL;
        close_cs(priv);
        close_spi(priv);
        return -1;
    }
    memset(priv->frame, 0xff, priv->frame_size);
    memset(priv->tx_frame, 0xff, priv->frame_size);

    if (pthread_mutex_init(&priv->paint_lock, NULL) != 0) {
        fprintf(stderr, "ERROR (screen:sharp-memory) cannot initialize paint lock\n");
        free(priv->packet);
        free(priv->tx_frame);
        free(priv->frame);
        priv->packet = NULL;
        priv->tx_frame = NULL;
        priv->frame = NULL;
        close_cs(priv);
        close_spi(priv);
        return -1;
    }

    if (!send_command(priv, SHARP_MEMORY_CLEAR_COMMAND)) {
        fprintf(stderr, "ERROR (screen:sharp-memory) cannot initialize %s: %s\n",
                priv->dev, strerror(errno));
        pthread_mutex_destroy(&priv->paint_lock);
        free(priv->packet);
        free(priv->tx_frame);
        free(priv->frame);
        priv->packet = NULL;
        priv->tx_frame = NULL;
        priv->frame = NULL;
        close_cs(priv);
        close_spi(priv);
        return -1;
    }

    priv->refresh_running = true;
    if (pthread_create(&priv->refresh_thread, NULL, &screen_sharp_memory_refresh, priv) != 0) {
        fprintf(stderr, "ERROR (screen:sharp-memory) cannot start refresh thread\n");
        priv->refresh_running = false;
        pthread_mutex_destroy(&priv->paint_lock);
        free(priv->packet);
        free(priv->tx_frame);
        free(priv->frame);
        priv->packet = NULL;
        priv->tx_frame = NULL;
        priv->frame = NULL;
        close_cs(priv);
        close_spi(priv);
        return -1;
    }

    fprintf(stderr,
            "screen:sharp-memory: %s %dx%d, CS=%s:%u, %u Hz SPI, %d fps, dither=bayer8, invert=%s, stretch=%s\n",
            priv->dev, SHARP_MEMORY_WIDTH, SHARP_MEMORY_HEIGHT, priv->cs_chip,
            priv->cs_line_offset, priv->speed_hz, priv->fps,
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
        pthread_mutex_lock(&priv->paint_lock);
        send_command(priv, SHARP_MEMORY_CLEAR_COMMAND);
        pthread_mutex_unlock(&priv->paint_lock);
        pthread_mutex_destroy(&priv->paint_lock);
    }
    free(priv->packet);
    free(priv->tx_frame);
    free(priv->frame);
    priv->packet = NULL;
    priv->tx_frame = NULL;
    priv->frame = NULL;
    close_cs(priv);
    close_spi(priv);
    free(priv->cs_chip);
    free(priv->dev);
    priv->cs_chip = NULL;
    priv->dev = NULL;
}

static void set_pixel(screen_sharp_memory_priv_t *priv, uint32_t x, uint32_t y, bool white) {
    const size_t offset = (size_t)y * (SHARP_MEMORY_WIDTH / 8) + x / 8;
    const uint8_t mask = (uint8_t)(1U << (x & 7));
    if (white)
        priv->frame[offset] |= mask;
    else
        priv->frame[offset] &= (uint8_t)~mask;
}

static bool write_all(int fd, const void *data, size_t size) {
    const uint8_t *bytes = (const uint8_t *)data;
    while (size > 0) {
        ssize_t written = write(fd, bytes, size);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (written == 0) {
            errno = EIO;
            return false;
        }
        bytes += written;
        size -= (size_t)written;
    }
    return true;
}

static void dump_frame_if_requested(const screen_sharp_memory_priv_t *priv) {
    if (access(SHARP_MEMORY_DUMP_TRIGGER, F_OK) != 0)
        return;

    // Consume the request before doing any I/O so a failure cannot stall every
    // subsequent screen refresh.
    if (unlink(SHARP_MEMORY_DUMP_TRIGGER) != 0 && errno != ENOENT) {
        fprintf(stderr, "ERROR (screen:sharp-memory) cannot consume dump trigger: %s\n",
                strerror(errno));
        return;
    }

    const int fd = open(SHARP_MEMORY_DUMP_TEMP_PATH,
                        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                        0644);
    if (fd < 0) {
        fprintf(stderr, "ERROR (screen:sharp-memory) cannot open frame dump: %s\n",
                strerror(errno));
        return;
    }

    static const char header[] = "P4\n400 240\n";
    bool ok = write_all(fd, header, sizeof(header) - 1);
    uint8_t row[SHARP_MEMORY_WIDTH / 8];
    for (uint32_t y = 0; ok && y < SHARP_MEMORY_HEIGHT; ++y) {
        const uint8_t *source = priv->frame + (size_t)y * sizeof(row);
        for (size_t x = 0; x < sizeof(row); ++x)
            row[x] = sharp_memory_frame_byte_to_pbm(source[x]);
        ok = write_all(fd, row, sizeof(row));
    }

    int dump_errno = ok ? 0 : errno;
    if (close(fd) != 0 && ok) {
        ok = false;
        dump_errno = errno;
    }
    if (ok && rename(SHARP_MEMORY_DUMP_TEMP_PATH, SHARP_MEMORY_DUMP_PATH) != 0) {
        ok = false;
        dump_errno = errno;
    }

    if (!ok) {
        unlink(SHARP_MEMORY_DUMP_TEMP_PATH);
        fprintf(stderr, "ERROR (screen:sharp-memory) cannot write frame dump: %s\n",
                strerror(dump_errno));
        return;
    }

    fprintf(stderr, "screen:sharp-memory: captured %s\n", SHARP_MEMORY_DUMP_PATH);
}

static void screen_sharp_memory_paint(matron_fb_t *fb) {
    screen_sharp_memory_priv_t *priv = (screen_sharp_memory_priv_t *)fb->io.data;
    if (priv->source == NULL || priv->frame == NULL || priv->spi_fd < 0)
        return;

    pthread_mutex_lock(&priv->paint_lock);
    cairo_surface_flush(priv->source);

    const uint32_t source_width = (uint32_t)cairo_image_surface_get_width(priv->source);
    const uint32_t source_height = (uint32_t)cairo_image_surface_get_height(priv->source);
    const int source_stride = cairo_image_surface_get_stride(priv->source);
    const uint8_t *source_data = cairo_image_surface_get_data(priv->source);
    const sharp_memory_viewport_t viewport = sharp_memory_fit_viewport(
        source_width, source_height, SHARP_MEMORY_WIDTH, SHARP_MEMORY_HEIGHT, priv->stretch);

    if (source_data != NULL && source_width > 0 && source_height > 0) {
        for (uint32_t y = 0; y < SHARP_MEMORY_HEIGHT; ++y) {
            for (uint32_t x = 0; x < SHARP_MEMORY_WIDTH; ++x) {
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
                set_pixel(priv, x, y, white);
            }
        }

        dump_frame_if_requested(priv);
        priv->frame_dirty = true;
    }

    pthread_mutex_unlock(&priv->paint_lock);
}

static void screen_sharp_memory_bind(matron_fb_t *fb, cairo_surface_t *surface) {
    screen_sharp_memory_priv_t *priv = (screen_sharp_memory_priv_t *)fb->io.data;
    if (priv->source != NULL)
        cairo_surface_destroy(priv->source);
    priv->source = cairo_surface_reference(surface);
}
