#ifndef KERNEL_TABLE_H
#define KERNEL_TABLE_H

#include "purr_fs.h"
#include "purr_kernel_table.h"

/* Must be called once, after the root filesystem is mounted and before anything calls
 * purr_kernel_table()'s fs_* entries. */
void kernel_table_set_fs(purr_fs_t *fs);

const purr_kernel_table_t *purr_kernel_table(void);

#endif
