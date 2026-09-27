#include "commands.h"

#include <string.h>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "purr_console.h"
#include "purr_kernel.h"

#define VERSION "0.1.0"

#if CONFIG_PURR_PROFILE_RECOVERY
#define SYSTEM_NAME  "KittenOS"
#define PROFILE_NAME "recovery"
#elif CONFIG_PURR_PROFILE_MINIMAL
#define SYSTEM_NAME  "PURR Loader"
#define PROFILE_NAME "minimal"
#else
#define SYSTEM_NAME  "PURR OS"
#define PROFILE_NAME "full"
#endif

const char *purr_system_name(void)
{
    return SYSTEM_NAME;
}

static int cmd_version(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    purr_cli_printf(cli, "%s %s (%s profile)\n", SYSTEM_NAME, VERSION, PROFILE_NAME);
    return 0;
}

static int cmd_info(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    purr_display_info_t di = {0};
    const purr_display_v2_t *d = purr_kernel_display();
    if (d) {
        d->get_info(&di);
    }
    purr_cli_printf(cli, "system:  %s %s\n", SYSTEM_NAME, VERSION);
    purr_cli_printf(cli, "board:   %s\n", purr_board()->name);
    purr_cli_printf(cli, "chip:    %s rev %d.%d, %d cores\n", CONFIG_IDF_TARGET,
                    chip.revision / 100, chip.revision % 100, chip.cores);
    purr_cli_printf(cli, "display: %s %dx%d\n", d ? di.name : "none", di.width, di.height);
    purr_cli_printf(cli, "idf:     %s\n", esp_get_idf_version());
    return 0;
}

static int cmd_mem(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    purr_cli_printf(cli, "internal: %u free, %u largest block\n",
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram) {
        purr_cli_printf(cli, "psram:    %u free of %u\n",
                        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)psram);
    }
    return 0;
}

static int cmd_uptime(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    unsigned s = (unsigned)(esp_timer_get_time() / 1000000);
    purr_cli_printf(cli, "up %u:%02u:%02u\n", s / 3600, (s / 60) % 60, s % 60);
    return 0;
}

static int cmd_parts(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    purr_cli_puts(cli, "name      type sub offset   size\n");
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        purr_cli_printf(cli, "%-9s %-4s %02x  %06x  %uK\n", p->label,
                        p->type == ESP_PARTITION_TYPE_APP ? "app" : "data",
                        (unsigned)p->subtype, (unsigned)p->address, (unsigned)(p->size / 1024));
    }
    esp_partition_iterator_release(it);
    return 0;
}

static int cmd_echo(purr_cli_t *cli, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        purr_cli_printf(cli, "%s%s", i > 1 ? " " : "", argv[i]);
    }
    purr_cli_puts(cli, "\n");
    return 0;
}

static int cmd_clear(purr_cli_t *cli, int argc, char **argv)
{
    (void)cli; (void)argc; (void)argv;
    purr_console_clear();
    return 0;
}

static int cmd_reboot(purr_cli_t *cli, int argc, char **argv)
{
    if (argc > 1) {
        /* Needs the boot target in purrcfg, which does not exist yet. */
        purr_cli_puts(cli, "reboot: options are not built yet (use the boot menu)\n");
        return 1;
    }
    purr_cli_puts(cli, "restarting...\n");
    purr_console_flush();
    esp_restart();
    return 0;
}

static const purr_cmd_t s_cmds[] = {
    {"help",    "list the commands",             purr_cli_cmd_help},
    {"version", "show the system and profile",   cmd_version},
    {"info",    "board, chip and display",       cmd_info},
    {"mem",     "free memory",                   cmd_mem},
    {"uptime",  "time since power on",           cmd_uptime},
    {"parts",   "the partition table",           cmd_parts},
    {"echo",    "print the arguments",           cmd_echo},
    {"clear",   "clear the screen",              cmd_clear},
    {"reboot",  "restart the device",            cmd_reboot},
};

const purr_cmd_t *purr_commands(int *count)
{
    *count = (int)(sizeof(s_cmds) / sizeof(s_cmds[0]));
    return s_cmds;
}
