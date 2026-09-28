/*
 * relocmulti_module.c: proves purrstrap's new multi-file module build (purrstrap/scripts/
 * modules.py, `--source a.c,b.c,...`) actually links and relocates correctly across several
 * compilation units -- not just one file, which is all every module built until now has ever
 * been. Built from four files together: this one, freestanding_libc.c (its memcpy), and two
 * pieces of REAL CoreOS component code, unmodified -- purr_relocate.c and purr_module.c, the
 * same functions the loader itself uses to load this very module. Exercises both directly,
 * statically linked into this module's own relocated code, not through any core table (they
 * are pure computation, exactly the kind of thing PurrOS/components/coreos/SPEC.md section 4.1
 * says doesn't need to cross the kernel<->CoreOS boundary).
 */
#include "purr_module_abi.h"
#include "purr_relocate.h"
#include "purr_module.h"

static const purr_core_table_t *s_core;

static int cmd_relocmulti(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    int ok = 1;

    /* Test 1: purr_relocate() on a hand-built buffer. */
    uint8_t buf[16];
    for (int i = 0; i < 16; i++) {
        buf[i] = 0;
    }
    /* A little-endian 0x00000010 at offset 4, the word purr_relocate() will patch. */
    buf[4] = 0x10;
    uint32_t offsets[1] = {4};
    purr_reloc_result_t rr = purr_relocate(buf, sizeof(buf), offsets, 1, 0x2000);
    uint32_t got = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) |
                   ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
    int reloc_ok = (rr == PURR_RELOC_OK) && (got == 0x2010);
    s_core->printf(cli, "relocmulti: purr_relocate %s (result=%s, word=%#x)\n",
                  reloc_ok ? "OK" : "FAIL", purr_reloc_result_name(rr), (unsigned)got);
    ok = ok && reloc_ok;

    /* Test 2: purr_module_parse_layout() + purr_module_read_data_relocs() on a hand-built
     * payload prefix (entry_offset=8, one data reloc at offset 0, no code relocs, a 24-byte
     * code blob starting at payload offset 16). */
    uint8_t payload[40];
    for (int i = 0; i < 40; i++) {
        payload[i] = 0;
    }
    payload[0] = 8;    /* entry_offset = 8 */
    payload[4] = 1;    /* data_reloc_count = 1 */
    /* data_reloc_offsets[0] = 0 (already zeroed) */
    /* code_reloc_count = 0 (already zeroed, at payload[12]) */

    purr_module_layout_t layout;
    purr_module_layout_result_t lr = purr_module_parse_layout(payload, sizeof(payload), &layout);
    int layout_ok = (lr == PURR_MOD_LAYOUT_OK) && layout.entry_offset == 8 &&
                    layout.data_reloc_count == 1 && layout.code_reloc_count == 0 &&
                    layout.code_offset == 16 && layout.code_size == 24;
    s_core->printf(cli, "relocmulti: parse_layout %s (result=%s)\n",
                  layout_ok ? "OK" : "FAIL", purr_module_layout_result_name(lr));
    ok = ok && layout_ok;

    if (layout_ok) {
        uint32_t data_off[1] = {0xdeadbeef};
        purr_module_read_data_relocs(payload, &layout, data_off);
        int read_ok = (data_off[0] == 0);
        s_core->printf(cli, "relocmulti: read_data_relocs %s (val=%u)\n",
                      read_ok ? "OK" : "FAIL", (unsigned)data_off[0]);
        ok = ok && read_ok;
    }

    s_core->puts(cli, ok ? "relocmulti: PASS\n" : "relocmulti: FAIL\n");
    return ok ? 0 : 1;
}

static const purr_cmd_t s_cmds[] = {
    {"relocmulti", "temporary: proves purrstrap's multi-file module build", cmd_relocmulti},
};

static const purr_module_table_t s_table = {
    .abi_version = PURR_MODULE_ABI_VERSION,
    .cmds = s_cmds,
    .cmd_count = 1,
};

__attribute__((used))
const purr_module_table_t *relocmulti_module_entry(const purr_core_table_t *core)
{
    s_core = core;
    return &s_table;
}
