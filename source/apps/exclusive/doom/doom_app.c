// doom_app.c — DOOM host: launches PrBoom under speed demon.
//
// New for PURR OS. It replaces the Espressif port's main/app_main.c, which was
// a whole firmware image whose entire job was to start Doom; here Doom is one
// app among many, and the OS has to be intact again when it exits.
//
// ── Failure has to be recoverable ───────────────────────────────────────────
// Speed demon has already unloaded the launcher and the system UI by the time
// this runs. If the WAD is missing, doing the obvious thing — log an error and
// return — leaves the device on a black screen with no UI and no way back
// short of a power cycle. So every failure path here goes through
// fail_screen(), which puts a readable message on the panel, waits for a
// keypress, and then exits properly so the OS is restored.
//
// That is also why the WAD is loaded HERE rather than inside PrBoom's W_Init:
// by the time PrBoom is initialising it calls I_Error() on failure, which in
// this port cannot do anything useful.

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "purr_kernel.h"
#include "purr_module.h"
#include "catcall_display.h"
#include "catcall_input.h"
#include "speed_demon.h"
#include "boot_splash.h"
#include "app_manager.h"

#include "doom_wad.h"

static const char *TAG = "doom";

// PrBoom's entry point (prboom/include/i_system.h). Never returns in normal
// play — it exits through I_Quit/exit().
extern int doom_main(int argc, char const * const * argv);

static TaskHandle_t   s_task    = NULL;
static volatile bool  s_running = false;

// ── Keyboard ────────────────────────────────────────────────────────────────

// Resolved by capability, not by name: a keyboard-class driver implements
// set_backlight (bbq20's under-key LEDs), a trackball does not.
// purr_kernel_input() is NOT usable — it returns the FIRST registered input,
// which on T-Deck Plus is the trackball. Same test mochi_hal.c and MagiDOS use.
const catcall_input_t *doom_find_keyboard(void)
{
    int n = purr_kernel_input_count();
    for (int i = 0; i < n; i++) {
        const catcall_input_t *in = purr_kernel_input_at(i);
        if (in && in->poll_event && in->set_backlight) return in;
    }
    return NULL;
}

// ── Failure screen ──────────────────────────────────────────────────────────

// Draws through boot_splash rather than a local framebuffer and font. It is
// already compiled into every image (it is a CORE_COMPONENT), it talks straight
// to catcall_display with no LVGL behind it — which matters, because LVGL's
// backend is one of the things speed demon just unloaded — and speed demon
// already uses it for its own restore screen, so the visual language matches.
static void fail_screen(const char *line1, const char *line2)
{
    ESP_LOGE(TAG, "%s%s%s", line1, line2 ? " / " : "", line2 ? line2 : "");

    purr_splash_show("DOOM", 1);
    purr_splash_status(line1);

    const catcall_input_t *kbd = doom_find_keyboard();
    if (!kbd) {
        // No keyboard to press a key on. Hold the message up long enough to be
        // read and photographed, then leave on our own — better than sitting
        // on an unreadable-for-how-long screen waiting for input that cannot
        // arrive.
        ESP_LOGW(TAG, "no keyboard; exiting in 8s");
        for (int i = 0; i < 8 && s_running; i++) {
            purr_speed_demon_heartbeat();
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        return;
    }

    // Alternate the two lines on the single status row, ~1.6s each. The splash
    // has room for one line and the second half of the message ("press any
    // key") is the part that stops this looking like a hang.
    input_event_t ev;
    while (kbd->poll_event(&ev)) { }      // drain whatever launched us

    int phase = 0;
    while (s_running) {
        purr_speed_demon_heartbeat();

        purr_splash_status((phase & 1) && line2 ? line2 : line1);
        phase++;

        for (int i = 0; i < 16 && s_running; i++) {
            while (kbd->poll_event(&ev)) {
                if (ev.type == INPUT_EVENT_KEY_DOWN) {
                    purr_splash_status("Exiting...");
                    return;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// ── Task ────────────────────────────────────────────────────────────────────

static void doom_task(void *arg)
{
    (void)arg;

    // Speed demon is already active: app_manager enters it before init()
    // because this app declares `.speed_demon = 1`. Deliberately NOT entered
    // from here or from init() — init() can run on the UI render task, and
    // entering unloads the UI backend, which would delete the task making the
    // call.

    ESP_LOGI(TAG, "internal free at start: %u, psram free: %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    purr_splash_show("DOOM", 1);
    purr_splash_status("Loading WAD from SD...");
    purr_speed_demon_heartbeat();

    char wadname[64] = "";
    doom_wad_err_t werr = doom_wad_load(wadname, sizeof(wadname));

    if (werr != DOOM_WAD_OK) {
        // The recoverable path this app is built around — see the file header.
        fail_screen(doom_wad_err_str(werr), "Press any key to exit");
        goto done;
    }

    ESP_LOGI(TAG, "WAD %s (%u bytes); psram free now %u",
             wadname, (unsigned)doom_wad_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    purr_splash_status("Starting DOOM...");

    // argv as the original port used it: -cout ICWEFDA routes every lprintf
    // severity to stdout, which is the serial log. Verbose on purpose — this
    // is the only visibility into the engine while it is running, and there is
    // no UI to put a diagnostic on.
    {
        char const *argv[] = { "doom", "-cout", "ICWEFDA", NULL };
        doom_main(3, argv);
    }

    // Reached only if doom_main returns, which it does not do in normal play.
    ESP_LOGW(TAG, "doom_main returned");

done:
    doom_wad_free();

    s_running = false;

    // Report the exit, or this app can only ever be launched once per boot:
    // app_manager_launch_path() returns early on state == APP_STATE_RUNNING and
    // nothing clears that for an app that exits on its own. By NAME, not task
    // handle — native apps are launched from a short-lived task that calls
    // init() and exits, so the handle app_manager tracks is not this one.
    //
    // This is also what restores everything speed demon unloaded, so it must
    // run on every path out of this function, including the WAD failures above.
    app_manager_notify_exited("doom");

    s_task = NULL;
    vTaskDelete(NULL);
}

// ── Module ──────────────────────────────────────────────────────────────────

static int doom_init(void)
{
    if (s_task) return 0;
    s_running = true;

    // 24KB, and INTERNAL — not PSRAM. Two separate reasons, both learned the
    // hard way on this hardware:
    //
    //   Size: PrBoom recurses through BSP traversal and its renderer. The
    //   Espressif port used 22480 bytes; this rounds up rather than trimming,
    //   because a stack overflow here surfaces as a corrupted-heap panic
    //   somewhere unrelated.
    //
    //   Placement: exiting writes NVS (speed demon's crash-guard marker), and
    //   writing NVS disables the flash cache — a task running on a PSRAM stack
    //   then faults on its own stack the moment it is touched. This is exactly
    //   the fault that broke the declarative speed_demon flag when the entry
    //   ran on native_task's PSRAM stack.
    //
    // Core 1: the app owns the machine, and core 0 carries what remains of the
    // system tasks. Same placement MagiDOS uses.
    if (xTaskCreatePinnedToCore(doom_task, "doom", 24 * 1024, NULL, 4, &s_task, 1) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        s_running = false;
        return -1;
    }
    return 0;
}

static void doom_deinit(void)
{
    s_running = false;
    // The task reports its own exit (which restores the OS) and deletes itself.
    // Wait for it rather than vTaskDelete()ing from outside, which would strand
    // the OS with every module still unloaded.
    for (int i = 0; i < 150 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
}

PURR_MODULE_REGISTER(doom) = {
    // Needs the whole machine — app_manager unloads the launcher, system UI,
    // mesh stack and radios before init() and restores them on exit.
    .speed_demon       = 1,
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_APP,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "doom",
    .version           = "0.1.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = CATCALL_FLAG_DISPLAY,
    .init              = doom_init,
    .deinit            = doom_deinit,
};
