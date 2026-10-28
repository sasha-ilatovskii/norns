#include "screen_event_stubs.h"

#include <condition_variable>
#include <mutex>

#include "hardware/screen.h"

namespace {

std::atomic<int> updates{0};
std::atomic<int> levels{0};
double line_rel_x = 0.0;
double line_rel_y = 0.0;
std::string screenshot_path;

std::mutex save_mutex;
std::condition_variable save_condition;
bool save_should_block = false;
bool save_is_blocked = false;

} // namespace

namespace screen_event_test {

void reset() {
    updates = 0;
    levels = 0;
    line_rel_x = 0.0;
    line_rel_y = 0.0;
    screenshot_path.clear();
    std::lock_guard<std::mutex> lock(save_mutex);
    save_should_block = false;
    save_is_blocked = false;
}

void block_save() {
    std::lock_guard<std::mutex> lock(save_mutex);
    save_should_block = true;
    save_is_blocked = false;
}

void wait_until_save_is_blocked() {
    std::unique_lock<std::mutex> lock(save_mutex);
    save_condition.wait(lock, [] { return save_is_blocked; });
}

void release_save() {
    std::lock_guard<std::mutex> lock(save_mutex);
    save_should_block = false;
    save_condition.notify_all();
}

int update_count() {
    return updates;
}
int level_count() {
    return levels;
}
double last_line_rel_x() {
    return line_rel_x;
}
double last_line_rel_y() {
    return line_rel_y;
}
const std::string &last_screenshot_path() {
    return screenshot_path;
}

} // namespace screen_event_test

void screen_update() {
    ++updates;
}

void screen_save() {
    std::unique_lock<std::mutex> lock(save_mutex);
    if (!save_should_block)
        return;
    save_is_blocked = true;
    save_condition.notify_all();
    save_condition.wait(lock, [] { return !save_should_block; });
}

void screen_restore() {
}
void screen_font_face(int) {
}
void screen_font_size(double) {
}
void screen_aa(int) {
}
void screen_gamma(double) {
}
void screen_brightness(int) {
}
void screen_contrast(int) {
}
void screen_invert(int) {
}
void screen_level(int) {
    ++levels;
}
void screen_line_width(double) {
}
void screen_line_cap(const char *) {
}
void screen_line_join(const char *) {
}
void screen_miter_limit(double) {
}
void screen_move(double, double) {
}
void screen_line(double, double) {
}
void screen_move_rel(double, double) {
}
void screen_line_rel(double x, double y) {
    line_rel_x = x;
    line_rel_y = y;
}
void screen_curve(double, double, double, double, double, double) {
}
void screen_curve_rel(double, double, double, double, double, double) {
}
void screen_arc(double, double, double, double, double) {
}
void screen_rect(double, double, double, double) {
}
void screen_stroke() {
}
void screen_fill() {
}
void screen_text(const char *) {
}
void screen_text_right(const char *) {
}
void screen_text_center(const char *) {
}
void screen_text_extents(const char *) {
}
void screen_text_trim(char *, double) {
}
void screen_clear() {
}
void screen_close_path() {
}
void screen_export_png(const char *) {
}
void screen_export_screenshot(const char *path) {
    screenshot_path = path;
}
void screen_display_png(const char *, double, double) {
}
void screen_peek(int, int, int, int) {
}
void screen_poke(int, int, int, int, unsigned char *) {
}
void screen_rotate(double) {
}
void screen_translate(double, double) {
}
void screen_set_operator(int) {
}
void screen_current_point() {
}

screen_surface_t *screen_surface_new(double, double) {
    return nullptr;
}
screen_surface_t *screen_surface_load_png(const char *) {
    return nullptr;
}

screen_surface_t *screen_surface_reference(screen_surface_t *surface) {
    auto *fake = reinterpret_cast<screen_event_test::fake_surface *>(surface);
    ++fake->references;
    return surface;
}

void screen_surface_free(screen_surface_t *surface) {
    auto *fake = reinterpret_cast<screen_event_test::fake_surface *>(surface);
    --fake->references;
}

void screen_surface_get_extents(screen_surface_t *) {
}

void screen_surface_display(screen_surface_t *surface, double, double) {
    auto *fake = reinterpret_cast<screen_event_test::fake_surface *>(surface);
    fake->references_during_display = fake->references.load();
}

void screen_surface_display_region(screen_surface_t *surface,
                                   double,
                                   double,
                                   double,
                                   double,
                                   double,
                                   double) {
    screen_surface_display(surface, 0.0, 0.0);
}

void screen_context_new(screen_surface_t *) {
}
void screen_context_free(screen_context_t *) {
}
void screen_context_get_current() {
}
void screen_context_set(const screen_context_t *) {
}
void screen_context_set_primary() {
}
const screen_context_t *screen_context_get_primary() {
    return nullptr;
}
