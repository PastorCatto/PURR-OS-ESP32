// purr_sdk_paws.h — catstrap SDK v0.1.0 — .paws tier
// Userland apps: display output + touch/input polling via win.* API.
// No direct kernel registration. No radio or GPS access.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// Catcall types — for reading display info and touch/input events.
// .paws apps use these types but do NOT call init/deinit.
#include "catcall_display.h"
#include "catcall_touch.h"
#include "catcall_input.h"

// Window + filesystem access — provided by the active UI module.
// Implemented at link time against the kernel's win_api.
// purr_win_create(), purr_win_draw_text(), purr_win_close(), etc.
// purr_sd_open(), purr_sd_read(), purr_sd_write(), purr_sd_close()
#ifdef PURR_PAWS_IMPL
// These are resolved by the kernel at pre-link. Leave this block empty
// unless you are implementing the .paws ABI bridge (kernel internal).
#endif

// Version info available to all .paws apps:
#define PURR_SDK_VERSION   "0.1.0"
#define PURR_TIER_NAME     "paws"
