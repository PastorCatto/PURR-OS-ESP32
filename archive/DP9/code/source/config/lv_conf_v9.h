// lv_conf_v9.h — PURR OS memory-placement override for LVGL v9, used only
// by the Tab5 build (Nougat backend). Sibling to lv_conf.h (LVGL v8, every
// other device) — kept as a SEPARATE file rather than version-guarding one
// shared file, since v8 and v9 have genuinely different lv_conf.h schemas
// (see CoreOS/CMakeLists.txt's PURR_DEVICE-conditional LV_CONF_PATH
// selection). Same narrow scope as lv_conf.h: only the memory allocator is
// overridden here, everything else still comes from Kconfig.
//
// Why: same rationale as lv_conf.h (internal SRAM is scarce, PSRAM is
// abundant) — LVGL v9 restructured its memory config from v8's simple
// LV_MEM_CUSTOM_ALLOC/FREE/REALLOC macros to a stdlib-abstraction scheme
// (LV_USE_STDLIB_MALLOC = LV_STDLIB_CUSTOM, then real C functions named
// lv_malloc_core()/lv_free_core()/lv_realloc_core() — confirmed against
// LVGL's own v9 docs, not guessed, given a wrong allocator override here
// would silently reintroduce the exact internal-RAM-exhaustion bug this
// file exists to prevent).
//
// Defined as `static inline` directly in this header — not in a separate
// .c file — deliberately: there's a documented, real LVGL/ESP-IDF issue
// (lvgl#8191) where defining these functions in a separate application
// component and relying on the linker to override LVGL's own weak/default
// symbols does NOT reliably work in ESP-IDF's per-component static-library
// build model. Since every LVGL source file (including lv_mem.c, which
// calls lv_malloc_core() et al.) transitively includes this exact file via
// the global LV_CONF_PATH property, a `static inline` definition here is
// visible directly within lv_mem.c's own translation unit — no cross-
// component linking involved at all. Same reasoning lv_conf.h's own v8
// macros already rely on successfully (confirmed working there via its own
// one-time diagnostic log line).

#ifndef PURR_LV_CONF_V9_H
#define PURR_LV_CONF_V9_H
#define LV_CONF_H   /* silences lv_conf_internal.h's missing-include check */

#include "esp_heap_caps.h"
#include "esp_log.h"

#define LV_USE_STDLIB_MALLOC LV_STDLIB_CUSTOM

static inline void *lv_malloc_core(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static inline void lv_free_core(void *p) {
    heap_caps_free(p);
}
static inline void *lv_realloc_core(void *p, size_t new_size) {
    return heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// lv_init.c (a different translation unit than lv_mem.c) calls lv_mem_init()/
// lv_mem_deinit() unconditionally — every other LV_USE_STDLIB_MALLOC backend
// (BUILTIN/CLIB/rtthread/micropython) provides these in its own vendored .c
// file, but LV_STDLIB_CUSTOM provides none, since a custom allocator has no
// pool to set up (confirmed by reading every stdlib/*/lv_mem_core_*.c —
// none of them compile in for LV_STDLIB_CUSTOM). heap_caps_malloc() needs no
// init/teardown, so these are true no-ops — same `static inline`-in-this-
// header trick as the *_core functions above, for the same reason (visible
// directly inside lv_init.c's own translation unit, no cross-component
// linking needed).
static inline void lv_mem_init(void) {}
static inline void lv_mem_deinit(void) {}

#endif // PURR_LV_CONF_V9_H
