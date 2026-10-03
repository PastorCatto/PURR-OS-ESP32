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
#include "purr_cli.h"
#include "purr_console.h"
#include "purr_kernel.h"
#include "recovery_loader.h"
#include "sdkconfig.h"

#if CONFIG_PURR_PROFILE_RECOVERY
#include "esp_partition.h"
#endif

static const char *TAG = "purros";

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
    /* F-10: the account list only exists in memory from here on (purr_fs_setup() ran at
     * boot, before any login) -- this is the first point /home/<name> ownership can
     * actually be assigned by name. */
    purr_fs_permissions_migrate();
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

    if (purr_kernel_init() != ESP_OK || purr_kernel_display() == NULL) {
        /* No display: there is nothing to draw on, so say so on the serial console. */
        for (;;) {
            ESP_LOGE(TAG, "display did not come up");
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    const purr_display_v2_t *d = purr_kernel_display();
    if (d->set_brightness) {
        d->set_brightness(255);
    }
    purr_console_init(d);

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
    purr_modules_setup(&boot_cli);
    purr_apps_setup(&boot_cli);
    purr_net_setup(&boot_cli);
    purr_continue_install(&boot_cli);
    purr_console_flush();
    vTaskDelay(pdMS_TO_TICKS(1200));   /* long enough to read before the login prompt clears it */

    /* Reached a stable state: reset the bootloader's failure-count ladder
     * (bootloader/SPEC.md section 6) so it doesn't keep climbing from here. */
    purr_mark_boot_healthy(&boot_cli);

    for (;;) {
        purr_kernel_table()->console_clear();
        run_login();
        run_shell();
    }
}
