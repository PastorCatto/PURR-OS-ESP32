#include "commands.h"

#include <stdio.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "purr_console.h"
#include "purr_fs.h"
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

/* ---------------------------------------------------------------- filesystem */

static purr_fs_t s_fs;

static void fail(purr_cli_t *cli, const char *what, int err)
{
    purr_cli_printf(cli, "%s: %s\n", what, purr_fs_strerror(err));
}

/* Paths are absolute. A missing leading slash is added, so "etc" means "/etc". */
static const char *abs_path(const char *in, char *out, size_t n)
{
    snprintf(out, n, "%s%s", in[0] == '/' ? "" : "/", in);
    return out;
}

static int need_fs(purr_cli_t *cli)
{
    if (purr_fs_mounted(&s_fs)) {
        return 1;
    }
    purr_cli_puts(cli, "no filesystem mounted (run: format --yes)\n");
    return 0;
}

void purr_fs_setup(purr_cli_t *cli)
{
    purr_bd_t bd;
    if (purr_fs_flash_bd("root", &bd) != 0) {
        purr_cli_puts(cli, "root: no such partition\n");
        return;
    }
    int e = purr_fs_mount(&s_fs, &bd);
    if (e == 0) {
        uint32_t used = 0, total = 0;
        purr_fs_usage(&s_fs, &used, &total);
        purr_cli_printf(cli, "root: mounted, %uK of %uK used\n",
                        (unsigned)(used * 4), (unsigned)(total * 4));
    } else {
        purr_cli_printf(cli, "root: not mounted (%s)\nrun: format --yes\n", purr_fs_strerror(e));
    }
}

static void print_entry(void *ctx, const char *name, int is_dir, uint32_t size)
{
    purr_cli_t *cli = ctx;
    if (is_dir) {
        purr_cli_printf(cli, "  %s/\n", name);
    } else {
        purr_cli_printf(cli, "  %-20s %u\n", name, (unsigned)size);
    }
}

static int cmd_ls(purr_cli_t *cli, int argc, char **argv)
{
    if (!need_fs(cli)) return 1;
    char path[80];
    int e = purr_fs_list(&s_fs, abs_path(argc > 1 ? argv[1] : "/", path, sizeof(path)), print_entry, cli);
    if (e < 0) { fail(cli, argc > 1 ? argv[1] : "/", e); return 1; }
    return 0;
}

static int show(void *ctx, const void *data, uint32_t len)
{
    purr_cli_t *cli = ctx;
    const uint8_t *p = data;
    for (uint32_t i = 0; i < len; i++) {
        if (p[i] == '\n' || (p[i] >= 0x20 && p[i] < 0x7F)) {
            cli->put(cli->put_ctx, (char)p[i]);
        } else if (p[i] != '\r') {
            cli->put(cli->put_ctx, '.');          /* binary: show a dot */
        }
    }
    return 0;
}

static int cmd_cat(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2) { purr_cli_puts(cli, "usage: cat <file>\n"); return 1; }
    if (!need_fs(cli)) return 1;
    char path[80];
    int e = purr_fs_read(&s_fs, abs_path(argv[1], path, sizeof(path)), show, cli);
    if (e < 0) { fail(cli, argv[1], e); return 1; }
    purr_cli_puts(cli, "\n");
    return 0;
}

static int cmd_mkdir(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2) { purr_cli_puts(cli, "usage: mkdir <dir>\n"); return 1; }
    if (!need_fs(cli)) return 1;
    char path[80];
    int e = purr_fs_mkdir(&s_fs, abs_path(argv[1], path, sizeof(path)));
    if (e < 0) { fail(cli, argv[1], e); return 1; }
    return 0;
}

static int cmd_rm(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2) { purr_cli_puts(cli, "usage: rm <file or empty dir>\n"); return 1; }
    if (!need_fs(cli)) return 1;
    char path[80];
    int e = purr_fs_remove(&s_fs, abs_path(argv[1], path, sizeof(path)));
    if (e < 0) { fail(cli, argv[1], e); return 1; }
    return 0;
}

static int cmd_mv(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 3) { purr_cli_puts(cli, "usage: mv <from> <to>\n"); return 1; }
    if (!need_fs(cli)) return 1;
    char a[80], b[80];
    int e = purr_fs_rename(&s_fs, abs_path(argv[1], a, sizeof(a)), abs_path(argv[2], b, sizeof(b)));
    if (e < 0) { fail(cli, argv[1], e); return 1; }
    return 0;
}

static int cmd_write(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 3) { purr_cli_puts(cli, "usage: write <file> <text...>\n"); return 1; }
    if (!need_fs(cli)) return 1;
    char path[80], text[PURR_CLI_LINE];
    text[0] = '\0';
    for (int i = 2; i < argc; i++) {
        size_t used = strlen(text);
        snprintf(text + used, sizeof(text) - used, "%s%s", i > 2 ? " " : "", argv[i]);
    }
    strncat(text, "\n", sizeof(text) - strlen(text) - 1);
    int e = purr_fs_write(&s_fs, abs_path(argv[1], path, sizeof(path)), text, (uint32_t)strlen(text));
    if (e < 0) { fail(cli, argv[1], e); return 1; }
    return 0;
}

static int cmd_df(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!need_fs(cli)) return 1;
    uint32_t used, total;
    int e = purr_fs_usage(&s_fs, &used, &total);
    if (e < 0) { fail(cli, "df", e); return 1; }
    purr_cli_printf(cli, "root: %uK used of %uK (%u blocks free)\n", (unsigned)(used * 4),
                    (unsigned)(total * 4), (unsigned)(total - used));
    return 0;
}

static int cmd_format(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "--yes") != 0) {
        purr_cli_puts(cli, "this erases everything on the root filesystem.\nrun: format --yes\n");
        return 1;
    }
    purr_bd_t bd;
    if (purr_fs_flash_bd("root", &bd) != 0) {
        purr_cli_puts(cli, "root: no such partition\n");
        return 1;
    }
    purr_cli_puts(cli, "formatting...\n");
    purr_console_flush();
    purr_fs_unmount(&s_fs);
    int e = purr_fs_format(&s_fs, &bd);
    if (e < 0) { fail(cli, "format", e); return 1; }
    purr_cli_puts(cli, "done.\n");
    return 0;
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
    {"ls",      "list a directory",              cmd_ls},
    {"cat",     "show a file",                   cmd_cat},
    {"mkdir",   "make a directory",              cmd_mkdir},
    {"rm",      "remove a file or empty dir",    cmd_rm},
    {"mv",      "rename or move",                cmd_mv},
    {"write",   "write text to a file",          cmd_write},
    {"df",      "filesystem space",              cmd_df},
    {"format",  "erase and create the fs",       cmd_format},
    {"echo",    "print the arguments",           cmd_echo},
    {"clear",   "clear the screen",              cmd_clear},
    {"reboot",  "restart the device",            cmd_reboot},
};

const purr_cmd_t *purr_commands(int *count)
{
    *count = (int)(sizeof(s_cmds) / sizeof(s_cmds[0]));
    return s_cmds;
}
