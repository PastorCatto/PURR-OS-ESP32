/*
 * PURR OS - the shell.
 *
 * Every profile of this project comes up on the display and drops to the shell:
 * "KittenOS shell" in the recovery profile, "PURR OS shell" in the full one. The
 * commands are in commands.c; the engine and the terminal are in CoreOS.
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

static const char *TAG = "purros";

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

    static purr_cli_t cli;
    static char prompt[24];
    int n;
    const purr_cmd_t *cmds = purr_commands(&n);
    snprintf(prompt, sizeof(prompt), "%s> ", purr_system_name());
    purr_cli_init(&cli, cmds, n, purr_console_put, NULL, prompt);

    purr_cli_printf(&cli, "%s shell\n", purr_system_name());
    purr_fs_setup(&cli);
    purr_net_setup(&cli);
    purr_cli_puts(&cli, "Type help for the commands.\n\n");
    purr_cli_prompt(&cli);
    purr_console_flush();

    for (;;) {
        char c = purr_kernel_key();
        if (c != 0) {
            purr_cli_feed(&cli, c);
            purr_console_flush();
        } else {
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }
}
