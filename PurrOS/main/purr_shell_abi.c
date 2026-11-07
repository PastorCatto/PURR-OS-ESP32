/*
 * purr_shell_abi.c - the kernel's side of the shell ABI: the real address of every name in purr_shell_exports.def (purr_shell_abi.h).
 *
 * The table is built from the same list the module build reads, so a name that does not exist (or was renamed) in the kernel is a compile error here, not a
 * crash on a device. The headers below are the ones the Windows shell includes: they declare everything the list names.
 */
#include "purr_shell_headers.h"
#include "purr_shell_abi.h"

#define SHELL_FN(n) (const void *)n,
#define SHELL_DATA(t, n) (const void *)&n,
const void *const purr_shell_exports[] = {
#include "purr_shell_exports.def"
};
#undef SHELL_FN
#undef SHELL_DATA

#define SHELL_FN(n) #n,
#define SHELL_DATA(t, n) #n,
const char *const purr_shell_export_names[] = {
#include "purr_shell_exports.def"
};
#undef SHELL_FN
#undef SHELL_DATA

const size_t purr_shell_export_count = sizeof(purr_shell_exports) / sizeof(purr_shell_exports[0]);

#define SHELL_LAYOUT(name, expr) {PURR_SHELL_LAYOUT_##name, (uint32_t)(expr)},
const purr_shell_layout_t purr_shell_layout[] = {
#include "purr_shell_layout.def"
};
#undef SHELL_LAYOUT

const size_t purr_shell_layout_count = sizeof(purr_shell_layout) / sizeof(purr_shell_layout[0]);
