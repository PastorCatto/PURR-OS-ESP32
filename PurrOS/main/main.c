/*
 * PURR OS - the shell.
 *
 * Every profile of this project comes up on the display and drops to a login (Users/SPEC.md
 * section 4), then the shell: "KittenOS shell" in the recovery profile, "PURR OS shell" in
 * the full one. The commands are in commands.c; the engine and the terminal are in CoreOS;
 * the login gate and the session are in login.c.
 */
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "commands.h"
#include "login.h"
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
#if CONFIG_PURR_PROFILE_RECOVERY
    if (!purros_installed()) {
        purr_login_skip("setup", PURR_ROLE_USER_ADMIN);
        return;
    }
#endif
    purr_login_run();
}

static void run_shell(void)
{
    static purr_cli_t cli;
    static char prompt[40];
    int n;
    const purr_cmd_t *cmds = purr_commands(&n);
    snprintf(prompt, sizeof(prompt), "%s@%s> ", purr_login_current()->name, purr_system_name());
    purr_cli_init(&cli, cmds, n, purr_console_put, NULL, prompt);

    purr_console_clear();
    purr_cli_printf(&cli, "%s shell\n", purr_system_name());
    purr_cli_puts(&cli, "Type help for the commands.\n\n");
    purr_cli_prompt(&cli);
    purr_console_flush();

    while (!purr_login_take_logout()) {
        char c = purr_kernel_key();
        if (c != 0) {
            purr_cli_feed(&cli, c);
            purr_console_flush();
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
    purr_modules_setup(&boot_cli);
    purr_apps_setup(&boot_cli);
    purr_net_setup(&boot_cli);
    purr_console_flush();
    vTaskDelay(pdMS_TO_TICKS(1200));   /* long enough to read before the login prompt clears it */

    for (;;) {
        purr_console_clear();
        run_login();
        run_shell();
    }
}
