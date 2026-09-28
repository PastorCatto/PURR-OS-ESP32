/* The shell's commands, shared by the KittenOS (recovery) and full profiles. */
#ifndef PURR_COMMANDS_H
#define PURR_COMMANDS_H

#include "purr_cli.h"
#include "purr_fs.h"

/* The shared, already-mounted root filesystem, for login.c's account files. */
purr_fs_t *purr_login_fs(void);

/* The table of commands for this build's profile. */
const purr_cmd_t *purr_commands(int *count);

/* What this build calls itself, e.g. "KittenOS" or "PURR OS". */
const char *purr_system_name(void);

/* Mount the root filesystem and say how it went. */
void purr_fs_setup(purr_cli_t *cli);

/* Mount the apps filesystem and say how it went. */
void purr_apps_setup(purr_cli_t *cli);

/* Start the Wi-Fi service, sharing the mounted root filesystem for saved networks. */
void purr_net_setup(purr_cli_t *cli);

#endif
