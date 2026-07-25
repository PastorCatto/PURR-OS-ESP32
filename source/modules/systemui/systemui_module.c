// systemui_module.c — kernel module header for the system UI.
//
// Unusual among PURR modules in that init() builds nothing: the system UI's
// surfaces are LVGL objects that must be created on the host backend's own
// render task, which doesn't exist yet at static-module-load time (and LVGL
// isn't thread-safe, so they can't be built from here anyway — see
// systemui.h's threading note). The host calls purr_systemui_init() itself,
// later, from inside its render loop.
//
// So what is this header for? Two things that are worth the file:
//   * device.pcat selection — purrstrap's _generate_glue() turns a
//     [modules] systemui = "systemui" entry into a static registration, which
//     is what makes this module something a device opts into rather than
//     something hardcoded into one UI backend.
//   * Visibility — it shows up in the loaded-module registry, so terminal's
//     `modules` command and purr_kernel_module_at() report it like anything
//     else, instead of it being an invisible library folded into Cupcake.

#include "systemui.h"
#include "../../kernel/core/purr_module.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "systemui";

static int systemui_init(void)
{
#ifdef CONFIG_PURR_SYSTEMUI
    // Presence marker only — see this file's header comment for why the real
    // build happens later, on the host's render task.
    ESP_LOGI(TAG, "system UI available — awaiting host purr_systemui_init()");
#else
    ESP_LOGI(TAG, "system UI compiled out (CONFIG_PURR_SYSTEMUI off) — stubs active");
#endif
    return 0;
}

static void systemui_deinit(void)
{
    // Nothing to tear down: every LVGL object this module creates lives on
    // lv_layer_top(), which the host backend's own deinit (lv_deinit()) frees
    // wholesale. Freeing them here would race that.
}

PURR_MODULE_REGISTER(systemui) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_SYSTEM,
    // IMPORTANT, not REQUIRED: a device with no system UI still boots to a
    // usable launcher, it just has no status bar/nav bar/lock screen.
    .load_priority     = PURR_PRIORITY_IMPORTANT,
    .name              = "systemui",
    .version           = "0.1.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = CATCALL_FLAG_DISPLAY,
    .init              = systemui_init,
    .deinit            = systemui_deinit,
};
