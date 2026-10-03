/* count_kmod.c - a minimal kernelmod: `count [dir]` prints how many entries a directory has. */
#include "purr_kernel_table.h"

static const purr_kernel_table_t *s_k;

static void tally(void *ctx, const char *name, int is_dir, uint32_t size)
{
    (void)name; (void)is_dir; (void)size;
    (*(int *)ctx)++;
}

static int cmd_count(purr_cli_t *cli, int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/";
    int n = 0;
    int e = s_k->fs_list(path, tally, &n);
    if (e < 0) {
        s_k->printf(cli, "count: %s: %s\n", path, s_k->fs_strerror(e));
        return 1;
    }
    s_k->printf(cli, "%d entr%s in %s\n", n, n == 1 ? "y" : "ies", path);
    return 0;
}

static const purr_cmd_t s_cmds[] = {
    {"count", "count entries in a directory", cmd_count},
};

static const purr_kernel_module_table_t s_table = {
    .abi_version = PURR_KERNEL_TABLE_ABI_VERSION,
    .cmds = s_cmds,
    .cmd_count = 1,
};

__attribute__((used))
const purr_kernel_module_table_t *count_kmod_entry(const purr_kernel_table_t *kernel)
{
    s_k = kernel;
    return &s_table;
}
