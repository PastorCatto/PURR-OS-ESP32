// purr_sdk_claw.h — catstrap SDK v0.1.0 — .claw tier
// Kernel-access apps: full catcall API, module registration, radio, GPS.
// Apps compiled at this tier are pre-linked into the firmware image.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// Full catcall interface — all subsystems
#include "catcalls.h"

// Kernel module ABI — required for purr_module_header_t export
#include "purr_module.h"

// Kernel runtime API — catcall getters, module registry, panic
#include "purr_kernel.h"

// Version info:
#define PURR_SDK_VERSION   "0.1.0"
#define PURR_TIER_NAME     "claw"
