/* The shell's commands, shared by the KittenOS (recovery) and full profiles. */
#ifndef PURR_COMMANDS_H
#define PURR_COMMANDS_H

#include "purr_cli.h"

/* The table of commands for this build's profile. */
const purr_cmd_t *purr_commands(int *count);

/* What this build calls itself, e.g. "KittenOS" or "PURR OS". */
const char *purr_system_name(void);

#endif
