/* purr_drvcmds.h - shell commands for the driver classes: gps, radio, modem, sd. Each one finds the first bound driver of its class (purr_class_find) and uses its ops table,
 * so they work with any chip whose driver is loaded. With no driver loaded they say how to get one. */
#ifndef PURR_DRVCMDS_H
#define PURR_DRVCMDS_H

#include "purr_cli.h"

int purr_gps_cmd(purr_cli_t *cli, int argc, char **argv);
int purr_radio_cmd(purr_cli_t *cli, int argc, char **argv);
int purr_battery_cmd(purr_cli_t *cli, int argc, char **argv);       /* the gauge's raw reading and what the OS makes of it */
int purr_modem_cmd(purr_cli_t *cli, int argc, char **argv);
int purr_sd_cmd(purr_cli_t *cli, int argc, char **argv);

#endif
