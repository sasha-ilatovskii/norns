#include <doctest/doctest.h>

#include <atomic>
#include <thread>

#include "common/screen_event_stubs.h"
#include "hardware/screen.h"
#include "screen_events.h"

TEST_CASE("screen event queue preserves fractional coordinates and coalesces adjacent updates") {
    screen_event_test::reset();
    screen_events_init();

    screen_event_line_rel(0.5, -1.25);
    screen_event_update();
    screen_event_update();
    screen_events_deinit();

    CHECK(screen_event_test::last_line_rel_x() == doctest::Approx(0.5));
    CHECK(screen_event_test::last_line_rel_y() == doctest::Approx(-1.25));
    CHECK(screen_event_test::update_count() == 1);
}

TEST_CASE("screen event shutdown drains queued work") {
    screen_event_test::reset();
    screen_event_test::block_save();
    screen_events_init();

    screen_event_save();
    screen_event_test::wait_until_save_is_blocked();
    screen_event_level(1);
    screen_event_level(2);
    screen_event_export_screenshot("/tmp/queued-screen.png");
    screen_event_test::release_save();
    screen_events_deinit();

    CHECK(screen_event_test::level_count() == 2);
    CHECK(screen_event_test::last_screenshot_path() == "/tmp/queued-screen.png");
}

TEST_CASE("queued display keeps a surface reference until rendering finishes") {
    screen_event_test::reset();
    screen_event_test::block_save();
    screen_event_test::fake_surface surface;
    screen_events_init();

    screen_event_save();
    screen_event_test::wait_until_save_is_blocked();
    screen_event_display_surface(&surface, 1.0, 2.0);
    screen_surface_free(reinterpret_cast<screen_surface_t *>(&surface));
    CHECK(surface.references == 1);

    screen_event_test::release_save();
    screen_events_deinit();

    CHECK(surface.references_during_display == 1);
    CHECK(surface.references == 0);
}

TEST_CASE("a saturated queue applies backpressure without dropping commands") {
    constexpr int event_count = 17000;
    screen_event_test::reset();
    screen_event_test::block_save();
    screen_events_init();

    screen_event_save();
    screen_event_test::wait_until_save_is_blocked();
    std::atomic<int> submitted{0};
    std::thread producer([&] {
        for (int index = 0; index < event_count; ++index) {
            screen_event_level(index & 15);
            ++submitted;
        }
    });

    while (submitted < 16000)
        std::this_thread::yield();
    screen_event_test::release_save();
    producer.join();
    screen_events_deinit();

    CHECK(submitted == event_count);
    CHECK(screen_event_test::level_count() == event_count);
}
