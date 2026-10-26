#pragma once

#include "hardware/io.h"

#ifdef __cplusplus
extern "C" {
#endif

extern screen_ops_t screen_sharp_memory_ops;

#ifdef NORNS_DESKTOP
extern screen_ops_t screen_sdl_ops;
#endif

#ifdef __cplusplus
}
#endif
