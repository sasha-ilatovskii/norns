#pragma once

#include <atomic>
#include <string>

namespace screen_event_test {

struct fake_surface {
    std::atomic<int> references{1};
    std::atomic<int> references_during_display{0};
};

void reset();
void block_save();
void wait_until_save_is_blocked();
void release_save();

int update_count();
int level_count();
double last_line_rel_x();
double last_line_rel_y();
const std::string &last_screenshot_path();

} // namespace screen_event_test
