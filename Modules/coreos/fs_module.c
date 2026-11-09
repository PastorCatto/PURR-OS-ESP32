/*
 * fs_module.c: `ls`, `cat`, `mkdir`, `rm`, `mv`, `write`, `df`, `format` -- the filesystem
 * shell commands, the second (and much bigger) real slice ported onto the prototype kernel
 * table (purr_kernel_table.h), after mem/uptime. Same freestanding discipline: no libc, no
 * ESP-IDF, everything through the table. A module is -nostdlib/-fno-builtin, so the handful
 * of tiny string operations these commands need (streq, a plain string copy) are hand-written
 * locally, the same way every module needing them has done since about_module.c.
 */
#include "purr_kernel_table.h"

static const purr_kernel_table_t *s_k;

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void fail(purr_cli_t *cli, const char *what, int err)
{
    s_k->printf(cli, "%s: %s\n", what, s_k->fs_strerror(err));
}

static int need_fs(purr_cli_t *cli)
{
    if (s_k->fs_mounted()) {
        return 1;
    }
    s_k->puts(cli, "no filesystem mounted (run: format --yes)\n");
    return 0;
}

/* Paths are absolute. A missing leading slash is added, so "etc" means "/etc". No snprintf
 * (no libc) -- a plain bounded copy instead. */
static const char *abs_path(const char *in, char *out, size_t n)
{
    size_t pos = 0;
    if (in[0] != '/' && pos + 1 < n) {
        out[pos++] = '/';
    }
    while (*in && pos + 1 < n) {
        out[pos++] = *in++;
    }
    out[pos] = '\0';
    return out;
}

static void print_entry(void *ctx, const char *name, int is_dir, uint32_t size)
{
    purr_cli_t *cli = ctx;
    if (is_dir) {
        s_k->printf(cli, "  %s/\n", name);
    } else {
        s_k->printf(cli, "  %-20s %u\n", name, (unsigned)size);
    }
}

static int cmd_ls(purr_cli_t *cli, int argc, char **argv)
{
    if (!need_fs(cli)) return 1;
    char path[80];
    const char *arg = argc > 1 ? argv[1] : "/";
    int e = s_k->fs_list(abs_path(arg, path, sizeof(path)), print_entry, cli);
    if (e < 0) { fail(cli, arg, e); return 1; }
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
    if (argc < 2) { s_k->puts(cli, "usage: cat <file>\n"); return 1; }
    if (!need_fs(cli)) return 1;
    char path[80];
    int e = s_k->fs_read(abs_path(argv[1], path, sizeof(path)), show, cli);
    if (e < 0) { fail(cli, argv[1], e); return 1; }
    s_k->puts(cli, "\n");
    return 0;
}

static int cmd_mkdir(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2) { s_k->puts(cli, "usage: mkdir <dir>\n"); return 1; }
    if (!need_fs(cli)) return 1;
    char path[80];
    int e = s_k->fs_mkdir(abs_path(argv[1], path, sizeof(path)));
    if (e < 0) { fail(cli, argv[1], e); return 1; }
    return 0;
}

static int cmd_rm(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2) { s_k->puts(cli, "usage: rm <file or empty dir>\n"); return 1; }
    if (!need_fs(cli)) return 1;
    char path[80];
    int e = s_k->fs_remove(abs_path(argv[1], path, sizeof(path)));
    if (e < 0) { fail(cli, argv[1], e); return 1; }
    return 0;
}

static int cmd_mv(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 3) { s_k->puts(cli, "usage: mv <from> <to>\n"); return 1; }
    if (!need_fs(cli)) return 1;
    char a[80], b[80];
    int e = s_k->fs_rename(abs_path(argv[1], a, sizeof(a)), abs_path(argv[2], b, sizeof(b)));
    if (e < 0) { fail(cli, argv[1], e); return 1; }
    return 0;
}

static int cmd_write(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 3) { s_k->puts(cli, "usage: write <file> <text...>\n"); return 1; }
    if (!need_fs(cli)) return 1;
    char path[80];
    char text[120];
    size_t pos = 0;
    for (int i = 2; i < argc && pos + 1 < sizeof(text); i++) {
        if (i > 2 && pos + 1 < sizeof(text)) {
            text[pos++] = ' ';
        }
        const char *a = argv[i];
        while (*a && pos + 1 < sizeof(text)) {
            text[pos++] = *a++;
        }
    }
    if (pos + 1 < sizeof(text)) {
        text[pos++] = '\n';
    }
    int e = s_k->fs_write(abs_path(argv[1], path, sizeof(path)), text, (uint32_t)pos);
    if (e < 0) { fail(cli, argv[1], e); return 1; }
    return 0;
}

static int cmd_df(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!need_fs(cli)) return 1;
    uint32_t used, total;
    int e = s_k->fs_usage(&used, &total);
    if (e < 0) { fail(cli, "df", e); return 1; }
    s_k->printf(cli, "root: %uK used of %uK (%u blocks free)\n", (unsigned)(used * 4),
               (unsigned)(total * 4), (unsigned)(total - used));
    return 0;
}

static int cmd_format(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2 || !streq(argv[1], "--yes")) {
        s_k->puts(cli, "this erases everything on the root filesystem.\nrun: format --yes\n");
        return 1;
    }
    s_k->puts(cli, "formatting...\n");
    s_k->console_flush();
    int e = s_k->fs_format();
    if (e < 0) { fail(cli, "format", e); return 1; }
    s_k->puts(cli, "done.\n");
    return 0;
}

static const purr_cmd_t s_cmds[] = {
    {"ls",     "list a directory",              cmd_ls},
    {"cat",    "show a file",                   cmd_cat},
    {"mkdir",  "make a directory",              cmd_mkdir},
    {"rm",     "remove a file or empty dir",    cmd_rm},
    {"mv",     "rename or move",                cmd_mv},
    {"write",  "write text to a file",          cmd_write},
    {"df",     "filesystem space",              cmd_df},
    {"format", "erase and create the fs",       cmd_format},
};

static const purr_kernel_module_table_t s_table = {
    .abi_version = PURR_KERNEL_TABLE_ABI_VERSION,
    .cmds = s_cmds,
    .cmd_count = 8,
};

__attribute__((used))
const purr_kernel_module_table_t *fs_module_entry(const purr_kernel_table_t *kernel)
{
    s_k = kernel;
    return &s_table;
}
