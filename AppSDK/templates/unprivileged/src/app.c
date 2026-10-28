/*
 * app.c - starter template for an unprivileged command-line .cat app (AppSDK/SPEC.md
 * section 6, "Unprivileged app" row).
 *
 * No mini-library yet (AppSDK/SPEC.md section 2 -- not built yet): this calls
 * env->console_print directly, the only catcall-shaped thing the minimal runtime hands out
 * today. Built the same way a module is (purrstrap `apps build`, which reuses the module
 * build pipeline with --kind app -- purr_app_abi.h's own note).
 */
#include <stddef.h>

#include "purr_app_abi.h"

static int on_create(const purr_app_env_t *env, const void *state, uint32_t state_len)
{
    (void)state;
    (void)state_len;
    if (env != NULL && env->console_print != NULL) {
        env->console_print("myapp: on_create ran.\n");
    }
    return 0;
}

static void on_destroy(int reason)
{
    (void)reason;
}

static const purr_app_table_t s_table = {
    .abi_version = PURR_APP_ABI_VERSION,
    .on_create = on_create,
    .on_destroy = on_destroy,
};

__attribute__((used))
const purr_app_table_t *myapp_entry(void)
{
    return &s_table;
}
