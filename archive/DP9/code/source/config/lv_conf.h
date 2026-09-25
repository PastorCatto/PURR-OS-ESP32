// lv_conf.h — PURR OS memory-placement override for the vendored LVGL
// component (CoreOS/managed_components/lvgl__lvgl, gitignored/re-fetched,
// so this file cannot live inside that tree).
//
// Wired in globally via LV_CONF_PATH (see CoreOS/CMakeLists.txt), which
// lv_conf_internal.h honors ahead of any Kconfig fallback
// (src/lv_conf_internal.h:32-37). This only overrides the three memory
// macros below — every other LVGL setting still comes from Kconfig
// (CONFIG_LV_MEM_CUSTOM=y etc., see CoreOS/sdkconfig_tdeck_plus.overrides)
// exactly as before.
//
// Why: internal SRAM on this board is only ~144KB and chronically almost
// exhausted (drops to ~1-2KB free within 14s of boot — confirmed live via
// serial logging), while PSRAM sits at ~8MB free, essentially idle.
// CONFIG_LV_MEM_CUSTOM routed LVGL's allocator to plain malloc()/free()/
// realloc(), so every label/button/textarea/list object in every open
// window was landing in internal RAM by default. These overrides force
// LVGL's own allocator to draw from PSRAM instead.

#ifndef PURR_LV_CONF_H
#define PURR_LV_CONF_H
#define LV_CONF_H   /* silences lv_conf_internal.h's missing-include check */

#include "esp_heap_caps.h"
#include "esp_log.h"

// Signatures matched against the real call sites (src/misc/lv_mem.c:136,
// 183, 208 — LV_MEM_CUSTOM_ALLOC(size), LV_MEM_CUSTOM_FREE(data),
// LV_MEM_CUSTOM_REALLOC(data_p, new_size)) and heap_caps_realloc()'s real
// signature (ptr, size, caps).
//
// TEMPORARY: real functions instead of a direct macro-to-heap_caps_malloc
// redirect, with a one-time log line, specifically to prove at runtime
// whether LVGL is actually routing through this override at all — internal
// RAM is still dropping by several KB per open app window even with this
// file wired in (confirmed via compile_commands.json), which shouldn't
// happen if every LVGL allocation is really landing in PSRAM. Revert to
// plain macros once confirmed/fixed.
static inline void *purr_lv_alloc(size_t size) {
    static int n = 0;
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (n < 5) { ESP_LOGW("lv_conf", "purr_lv_alloc #%d size=%u -> %p", n, (unsigned)size, p); n++; }
    return p;
}
static inline void purr_lv_free(void *ptr) {
    heap_caps_free(ptr);
}
static inline void *purr_lv_realloc(void *ptr, size_t size) {
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

#define LV_MEM_CUSTOM_ALLOC(size)        purr_lv_alloc((size))
#define LV_MEM_CUSTOM_FREE(ptr)          purr_lv_free((ptr))
#define LV_MEM_CUSTOM_REALLOC(ptr, size) purr_lv_realloc((ptr), (size))

#endif // PURR_LV_CONF_H
