#include "ssd1322.h"

#include <stdio.h>

void ssd1322_init() {
    fprintf(stderr, "ERROR (screen) SSD1322 support is unavailable in a CM5 build\n");
}

void ssd1322_deinit() {
}

void ssd1322_refresh() {
}

void ssd1322_update(cairo_surface_t *, bool) {
}

void ssd1322_set_brightness(uint8_t) {
}

void ssd1322_set_contrast(uint8_t) {
}

void ssd1322_set_display_mode(ssd1322_display_mode_t) {
}

void ssd1322_set_gamma(double) {
}

void ssd1322_set_refresh_rate(uint8_t) {
}

uint8_t *ssd1322_resize_buffer(size_t) {
    return NULL;
}
