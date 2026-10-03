#include <stdint.h>

#include "purr_kernel_table.h"

/*
 * The first real (not synthetic) CoreOS entry point, loaded by the real standalone kernel
 * binary (Kernel/, PurrOS/SPEC.md section 6) from a file on the real root filesystem --
 * closing the loop CoreOSSpike opened 2026-09-28 (the mechanism, proven in isolation) and the
 * 16-file purrstrap build proved 2026-09-30 (that real coreos/src compiles this way), but this
 * time with the real kernel as the host, not the monolith standing in for it.
 *
 * Deliberately minimal: no command dispatch table yet (that's commands.c's real content,
 * genuinely large, a separate and much bigger conversion). This proves the full real path --
 * mount -> load -> relocate -> call entry with the real kernel table -> real code runs and
 * genuinely uses it (puts, fs_list, heap query, not just a self-contained checksum) -- works
 * end to end before any of that content moves over.
 */

static const purr_kernel_table_t *s_k;

static void list_root(void *ctx, const char *name, int is_dir, uint32_t size)
{
    (void)ctx;
    s_k->printf(NULL, "  %s%s (%u bytes)\n", name, is_dir ? "/" : "", (unsigned)size);
}

static const purr_kernel_module_table_t s_table = {
    .abi_version = PURR_KERNEL_TABLE_ABI_VERSION,
    .cmds = NULL,
    .cmd_count = 0,
};

__attribute__((used))
const purr_kernel_module_table_t *coreos_min_entry(const purr_kernel_table_t *kernel)
{
    s_k = kernel;
    s_k->puts(NULL, "CoreOS (minimal): loaded and running from a real file.\n");
    s_k->printf(NULL, "free PSRAM: %u bytes\n", (unsigned)s_k->heap_free_psram());
    s_k->puts(NULL, "root filesystem:\n");
    s_k->fs_list("/", list_root, NULL);
    return &s_table;
}
