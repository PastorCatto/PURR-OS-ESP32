/*
 * i2cscan_module.c: `i2cscan` -- lists every 7-bit address (0x08 to 0x77) that answers on the
 * board's I2C bus. A kernelmod: it only calls i2c_probe through the kernel table.
 *
 * Needs the i2c_probe field added to purr_kernel_table_t (ABI 7), see
 * documentation/13-writing-drivers.md ("Worked example"). Compiled and linked as a kernelmod
 * with `purrstrap modules build`; not run on hardware.
 */
#include "purr_kernel_table.h"

static const purr_kernel_table_t *s_k;

static int cmd_i2cscan(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    int found = 0;
    s_k->puts(cli, "     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
    for (int row = 0; row < 8; row++) {
        s_k->printf(cli, "%02x:", row * 16);
        for (int col = 0; col < 16; col++) {
            int addr = row * 16 + col;
            if (addr < 0x08 || addr > 0x77) {
                s_k->puts(cli, "   ");
                continue;
            }
            int r = s_k->i2c_probe(addr);
            if (r < 0) {
                s_k->puts(cli, "\nno I2C bus on this board\n");
                return 1;
            }
            if (r > 0) {
                found++;
                s_k->printf(cli, " %02x", addr);
            } else {
                s_k->puts(cli, " --");
            }
        }
        s_k->puts(cli, "\n");
    }
    s_k->printf(cli, "%d device(s) found\n", found);
    return 0;
}

static const purr_cmd_t s_cmds[] = {
    {"i2cscan", "list devices on the I2C bus", cmd_i2cscan},
};

static const purr_kernel_module_table_t s_table = {
    .abi_version = PURR_KERNEL_TABLE_ABI_VERSION,
    .cmds = s_cmds,
    .cmd_count = 1,
};

__attribute__((used))
const purr_kernel_module_table_t *i2cscan_module_entry(const purr_kernel_table_t *kernel)
{
    s_k = kernel;
    return &s_table;
}
