/*
 * hello_app.c - the first real `.cat` app, built to prove AppRuntime's minimal-first manager
 * (AppRuntime/SPEC.md section 1, chunk C/D/E) end to end on real hardware: load, relocate,
 * call entry(), call on_create() with a real purr_app_env_t, call on_destroy().
 *
 * No mini-library (AppSDK/SPEC.md section 2 -- not built yet): this calls env->console_print
 * directly with string literals, the only catcall-shaped thing the minimal runtime hands out.
 * Built the same way a module is (purrstrap `modules build --kind app`), since the container,
 * layout and relocation scheme are identical (purr_app_abi.h's own note).
 */
#include <stddef.h>

#include "purr_app_abi.h"

static int on_create(const purr_app_env_t *env, const void *state, uint32_t state_len)
{
    (void)state;
    (void)state_len;
    if (env != NULL && env->console_print != NULL) {
        env->console_print("hello from hello_app! on_create ran.\n");
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
const purr_app_table_t *hello_app_entry(void)
{
    return &s_table;
}
