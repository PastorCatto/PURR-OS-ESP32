/*
 * PURR OS - the shell.
 *
 * Every profile of this project comes up on the display and drops to a login (Users/SPEC.md
 * section 4), then the shell: "KittenOS shell" in the recovery profile, "PURR OS shell" in
 * the full one. The commands are in commands.c; the engine and the terminal are in CoreOS;
 * the login gate and the session are in login.c.
 *
 * run_login()/run_shell() below call everything kernel/hardware-level through
 * purr_kernel_table() (purr_kernel_table.h) instead of touching purr_login_*()/
 * purr_kernel_key() directly -- PurrOS/SPEC.md section 6: this is the boot orchestration that
 * would move into a separately loaded CoreOS once that split is real, so it's written now the
 * way it would have to be written then, the same discipline every kernelmod already keeps.
 * purros_installed()'s raw esp_partition_* check and the login.h include stay out of that:
 * it's a profile-level boot decision (which of login_run/login_skip to call at all), not
 * something CoreOS itself would ever need to make.
 */
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "commands.h"
#include "login.h"
#include "purr_catrt.h"
#include "purr_settings.h"
#include "purr_power_plat.h"
#include "oobe_firstboot.h"
#include "recovery_screen.h"
#include "purr_cli.h"
#include "purr_console.h"
#include "purr_kernel.h"
#include "recovery_loader.h"
#include "sdkconfig.h"

#if CONFIG_PURR_BOARD_TDECK || CONFIG_PURR_BOARD_TDECK_PLUS
#include "purr_card_shell.h"
#include "recovery_app.h"
#if !CONFIG_PURR_PROFILE_RECOVERY
#include "purr_w10m.h"
#endif
#include "purr_lvgl.h"
#include "purr_ui_assets.h"
#include "purr_ui_config.h"
#endif

#if CONFIG_PURR_PROFILE_RECOVERY
#include "esp_partition.h"
#include "purr_slot_trailer.h"
#endif

static const char *TAG = "purros";

/* The kernel-start screen (recovery_screen.c): colour T-Deck boards, full profile. BOOT_STEP sets the stage line
 * and the bar's step (of 5) while it is on screen, and does nothing otherwise. */
#if (CONFIG_PURR_BOARD_TDECK || CONFIG_PURR_BOARD_TDECK_PLUS) && CONFIG_PURR_PROFILE_FULL
#define BOOT_SCREEN 1
#else
#define BOOT_SCREEN 0
#endif
#define BOOT_STEP(i, text)                       \
    do {                                         \
        if (BOOT_SCREEN && rs_active()) {        \
            rs_line(text);                       \
            rs_progress(i, 5, 0);                \
        }                                        \
    } while (0)

#if CONFIG_PURR_PROFILE_RECOVERY
/* Whether either OTA slot holds something that looks like a real app image. If not, KittenOS
 * is the only thing on this device: requiring an account before it can even install PURR OS
 * would be backwards, so login is skipped in that case (see run_login()). */
static int purros_installed(void)
{
    static const esp_partition_subtype_t slots[] = {ESP_PARTITION_SUBTYPE_APP_OTA_0,
                                                     ESP_PARTITION_SUBTYPE_APP_OTA_1};
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, slots[i], NULL);
        uint32_t first = 0;
        if (p != NULL && esp_partition_read(p, 0, &first, sizeof(first)) == ESP_OK &&
            (first & 0xFF) == 0xE9) {
            return 1;
        }
    }
    return 0;
}
#endif

/* Everything that follows a successful sign-in, however it was done (the text prompt, or the
 * graphical sign-in screen). */
void purr_session_started(void)
{
    /* F-10: the account list only exists in memory from here on (purr_fs_setup() ran at
     * boot, before any login) -- this is the first point /home/<name> ownership can
     * actually be assigned by name. */
    purr_fs_permissions_migrate();
    /* The apps that were waiting for a user start now. */
    purr_catrt_user_logged_in();
}

static void run_login(void)
{
    const purr_kernel_table_t *k = purr_kernel_table();
#if CONFIG_PURR_PROFILE_RECOVERY
    if (!purros_installed()) {
        k->login_skip("setup", PURR_ROLE_USER_ADMIN);
        return;
    }
#endif
    k->login_run();
    purr_session_started();
}

static void run_shell(void)
{
    const purr_kernel_table_t *k = purr_kernel_table();
    static purr_cli_t cli;
    static char prompt[40];
    int n;
    const purr_cmd_t *cmds = purr_commands(&n);
    purr_klogin_who_t who;
    k->login_whoami(&who);
    snprintf(prompt, sizeof(prompt), "%s@%s> ", who.name, purr_system_name());
    purr_cli_init(&cli, cmds, n, purr_console_put, NULL, prompt);

    k->console_clear();
    purr_cli_printf(&cli, "%s shell\n", purr_system_name());
    purr_cli_puts(&cli, "Type help for the commands.\n\n");
    purr_cli_prompt(&cli);
    k->console_flush();

    while (!k->login_take_logout()) {
        char c = k->read_key();
        if (c != 0) {
            purr_cli_feed(&cli, c);
            k->console_flush();
        } else {
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "%s starting", purr_system_name());

#if CONFIG_PURR_PROFILE_MINIMAL
    purr_recovery_loader_main();   /* no shell: connects, recovers KittenOS, restarts */
    return;                        /* not reached; kept so the function's shape stays plain */
#endif

    if (purr_kernel_init() != ESP_OK) {
        /* A real hardware failure (the SPI bus itself didn't start) -- distinct from "no
         * display driver bound yet", which is now expected at this point (st7789 converted
         * to a loadable module, Modules/drivers/st7789_module.c; it cannot bind before the
         * filesystem mounts the way the compiled-in version always could). */
        for (;;) {
            ESP_LOGE(TAG, "kernel init failed (board hardware did not come up)");
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

    /* No display is bound yet -- purr_console_put()/purr_console_clear() only touch the
     * in-memory terminal buffer (purr_console.c's own comment), and purr_console_flush()
     * already no-ops with none set, so boot status can safely accumulate here, sized from
     * the board profile instead of a live display query. The real purr_console_init(d)
     * below (once display has had its chance to load) resets this -- the few lines printed
     * in this window are serial-log-only (ESP_LOGI/ESP_LOGW already duplicate everything),
     * not lost information, just not shown on the physical screen for that brief window. */
    purr_console_init_headless(purr_board());

    /* A throwaway cli, just so purr_fs_setup/purr_net_setup have somewhere to print their
     * status before the login prompt (which uses the console directly, not the shell). */
    static purr_cli_t boot_cli;
    purr_cli_init(&boot_cli, NULL, 0, purr_console_put, NULL, "");
    purr_cli_printf(&boot_cli, "%s starting\n", purr_system_name());
    purr_fs_setup(&boot_cli);
#if CONFIG_PURR_PROFILE_RECOVERY
    /* KittenOS performs the swap for any pending system-file update (PurrOS/SPEC.md section
     * 6.1) before anything else touches /boot -- reboots and never returns if there was one. */
    purr_swap_setup(&boot_cli);
#endif
    purr_modules_setup(&boot_cli);   /* loads/registers display (+ keyboard/touch) from here */

    /* Display is the one driver that still has to exist before anything can be shown on the
     * physical screen -- same fail-stop this function always had, just moved to after the
     * filesystem mounts and purr_modules_setup() has had its chance to load the module. */
    const purr_display_v2_t *d = purr_kernel_display();
    if (d == NULL) {
        for (;;) {
            ESP_LOGE(TAG, "display did not come up");
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    if (d->set_brightness) {
        d->set_brightness(255);
    }
    purr_console_init(d);
    purr_console_flush();   /* paint whatever's already buffered (the "starting"/fs-mount
                              * lines) immediately instead of waiting for the next flush. */
    /* The boot menu's hand-off screen carries on while the kernel starts: the same frame, a stage line and a bar,
     * instead of the log text (which is still in the serial log). The text console comes back if the shell is
     * the text one, or a login is asked for. Full profile on the colour T-Deck boards only. */
    if (BOOT_SCREEN) {
        rs_begin_boot();
    }
    BOOT_STEP(0, "Starting services");

    purr_apps_setup(&boot_cli);
    purr_settings_setup();              /* /etc/system.conf: time zone, clock format; before any UI reads them */
    purr_power_sys_start();             /* screen timeout and wake locks; reads its settings from system.conf */
    purr_catrt_setup(&boot_cli);
    BOOT_STEP(1, "Starting network");
    purr_net_setup(&boot_cli);
    BOOT_STEP(2, "Preparing the shell");
#if CONFIG_PURR_BOARD_TDECK || CONFIG_PURR_BOARD_TDECK_PLUS
    /* mode.conf first, then only the active shell's own config: a W10M device has no use for the
     * card stack's or the nav shell's files. A file already there is never touched, so a device that
     * switches shell later plants the new one's config on its next boot. */
#if !CONFIG_PURR_PROFILE_RECOVERY           /* KittenOS shows the recovery app, not a shell: no card stack or nav config */
    purr_ui_mode_setup(&boot_cli);
    switch (purr_ui_mode_get()) {
    case PURR_UI_MODE_NAV:
        purr_ui_config_setup(&boot_cli);
        break;
    case PURR_UI_MODE_CARD_SHELL:
        purr_ui_card_shell_config_setup(&boot_cli);
        break;
    default:                            /* W10M keeps its settings in the theme files */
        break;
    }
#endif
#endif
#if CONFIG_PURR_PROFILE_RECOVERY
    /* KittenOS right after the recovery loader restored it, with no account yet: the setup wizard (account, colour, date and time)
     * instead of the text prompts, while the rest of the system installs in the background. It does not return unless LVGL
     * could not start, and then the text flow below carries on as before. */
    if (oobe_firstboot_available() || oobe_firstboot_finishing_available()) {
        oobe_firstboot_run();
    }
#endif
#if CONFIG_PURR_PROFILE_RECOVERY
    int resumed = purr_stage2_pending();     /* an install (a first install, or a reset reinstalling in place) is carrying on */
#endif
    purr_continue_install(&boot_cli);
    purr_console_flush();
#if CONFIG_PURR_PROFILE_RECOVERY
    /* Restoring in place: the reset marked the kernel BAD and the install has just written a new one (which clears the mark). Nobody has to
     * press anything: start PURR OS. If the kernel did not get installed (no network), the mark is still there and we stay in recovery. */
    if (resumed) {
        const esp_partition_t *kp = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
        if (kp != NULL && !purr_slot_marked_bad(kp)) {
            purr_cli_puts(&boot_cli, "restarting into PURR OS...\n");
            purr_console_flush();
            vTaskDelay(pdMS_TO_TICKS(1500));
            esp_restart();
        }
    }
#endif
    vTaskDelay(pdMS_TO_TICKS(1200));   /* long enough to read before the login prompt clears it */

    /* Reached a stable state: reset the bootloader's failure-count ladder
     * (bootloader/SPEC.md section 6) so it doesn't keep climbing from here. */
    purr_mark_boot_healthy(&boot_cli);

#if CONFIG_PURR_BOARD_TDECK || CONFIG_PURR_BOARD_TDECK_PLUS
    /* The real escape hatch: hold space right now, during login, to skip the LVGL shell for
     * this boot and land in the text shell instead (plantmodules, appinstall, run, and
     * everything else the graphical shell has no equivalent for yet) -- there was no way to
     * reach it at all before this, found testing chunk C/D/E (AppRuntime/SPEC.md) on a
     * freshly erased T-Deck Plus, 2026-10-11. Checked before login runs, so it is available
     * at the exact moment a person would otherwise be typing a username or password, not a
     * separate silent window with its own timing to learn. Polls for a fixed, short window
     * rather than blocking, so a device nobody is touching proceeds exactly as before.
     * Space, not a letter, because it is the one key unlikely to collide with whatever the
     * keyboard driver reports for a key merely resting near a finger. */
    purr_cli_printf(&boot_cli, "(hold space for the text shell instead of the graphical one)\n");
    purr_console_flush();
    BOOT_STEP(3, "Hold space: text shell");     /* the hint, now that the console text is not on screen */
    int shell_escape = 0;
    for (int i = 0; i < 70; i++) {         /* ~1s at 15ms/tick */
        if (purr_kernel_table()->read_key() == ' ') {
            shell_escape = 1;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }

    /* LVGL is the primary shell UI on boards with the flash budget for it (purr_lvgl.h's own
     * comment on why only these two). purr_console's text login/shell loop below is the
     * fallback: it runs if the space-bar escape above was held, if LVGL can't even start (no
     * display, draw buffer alloc failure -- purr_lvgl_init()'s own checks), or once
     * purr_lvgl_run() returns because something called purr_lvgl_report_critical_error().
     * All leave the text console ready to take over immediately -- purr_console_init()
     * already ran above, unconditionally, before any of these decisions are even made. */
    if (shell_escape) {
        ESP_LOGW(TAG, "shell escape held; skipping LVGL for this boot");
        rs_end();
    } else {
        BOOT_STEP(4, "Starting PURR OS");
        /* A real login here, not a create-only check: chunk F (the app_grid widget,
         * memory/[[project_appruntime_fullstack_plan]]) needs to know who is logged in to
         * know whose apps to show, and nothing before this point ever sets that for an
         * already-provisioned device (run_login()'s own first-time-setup branch is the only
         * thing that calls purr_login_current()'s setter, and only on a brand-new device).
         * Deliberate behavior change from the first version of this fix: every boot now
         * shows a real login prompt before the graphical shell, not just the very first
         * one -- skipped entirely above if the shell escape was held, so this never runs
         * twice in a row for that path. */
        /* The Windows 10 Mobile style shell has its own sign-in screen (purr_w10m.c), shown before
         * anything else and with nothing started for the user until it succeeds, so the text
         * login is skipped for it. With no accounts yet there is nothing to sign in to, so the
         * text first-time setup still runs, and the shell then starts already signed in. */
        /* Automatic sign-in (Settings > Accounts, or the VM test image's built-in account): the session
         * starts here with no password asked. Holding space, handled above, skips this block entirely. */
        int auto_in = 0;
#if !CONFIG_PURR_PROFILE_RECOVERY
        if (purr_login_autologin()) {
            auto_in = 1;
            purr_session_started();
        }
#endif
#if CONFIG_PURR_PROFILE_RECOVERY
        int own_signin = 1;                   /* the recovery app has no sign-in: erasing asks an administrator for their password */
#else
        int own_signin = purr_ui_mode_get() == PURR_UI_MODE_W10M && purr_login_has_accounts();
#endif
        if (!own_signin && !auto_in) {
            rs_end();                           /* a text login needs the console */
            run_login();
        }
        if (purr_lvgl_init() == ESP_OK) {
            /* Best-effort: a missing/corrupt ui_assets partition just means "image"
             * properties in the config can't be found (purr_ui_assets.h's own comment) --
             * never critical. */
            purr_ui_assets_init();
            /* Which screen actually boots is itself a choice now (/ui/mode.conf, planted by
             * purr_ui_mode_setup() above) -- two real, compiled engines, not a general
             * scripting format (memory/[[project_card_shell_ui_redesign]]'s own note on why
             * only this much, not more, is a config decision). card_shell is the gesture-
             * driven full-screen-card shell (its own separate /ui/cardshell.conf, loaded by
             * purr_card_shell_start() itself); nav is this file's older declarative
             * label/button/image/app_grid/navbar screen (/ui/shell.conf, planted above by
             * purr_ui_config_setup()) -- real nav buttons already exist there, it just
             * wasn't reachable from the normal boot path until now. */
#if CONFIG_PURR_PROFILE_RECOVERY
            recovery_app_start();
#else
            if (purr_ui_mode_get() == PURR_UI_MODE_NAV) {
                purr_ui_load_and_build("/ui/shell.conf");
            } else if (purr_ui_mode_get() == PURR_UI_MODE_W10M) {
#if CONFIG_PURR_PROFILE_RECOVERY
                purr_card_shell_start();
#else
                purr_w10m_start();
#endif
            } else {
                purr_card_shell_start();
            }
#endif
            purr_lvgl_run();
            ESP_LOGW(TAG, "LVGL stopped; falling back to the text shell");
            purr_console_set_muted(0);          /* the graphical shell had muted it */
            purr_kernel_table()->console_clear();
        } else {
            ESP_LOGW(TAG, "LVGL did not start; falling back to the text shell");
        }
    }
#endif

    rs_end();
    for (;;) {
        purr_kernel_table()->console_clear();
        run_login();
        run_shell();
    }
}
