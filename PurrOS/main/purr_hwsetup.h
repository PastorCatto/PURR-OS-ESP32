/*
 * purr_hwsetup.h - setting up hardware that "Find hardware" found, and keeping the drivers current (plan8.md "Driver foundation").
 *
 * Three jobs, all blocking (call from a worker task):
 *   boot       read /etc/board.conf (the discovered profile) and bind its devices to whatever drivers are loaded;
 *   set up     for each device a scan found: get its driver if it is not loaded (from the driver index, checked against the index entry's size and
 *              hash, then verified by the loader like any driver: unsigned only in developer mode), add the device and its pins to the profile,
 *              save it, and bind it;
 *   update     compare the installed drivers with the index and download newer ones (they apply after a restart).
 *
 * The driver index is a manifest of `type=driver` entries (purr_manifest.h): next to the module index on the official server, and in
 * developer mode also on the dev server the owner chose (`<base>/recovery/drivers.manifest`, what `broken-kitten-recovery add-driver` makes).
 */
#ifndef PURR_HWSETUP_H
#define PURR_HWSETUP_H

#include <stddef.h>

#include "purr_cli.h"
#include "purr_hwprofile.h"
#include "purr_hwscan_plat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called once at boot after the drivers in /boot/drivers are loaded. */
void purr_hwsetup_boot(purr_cli_t *cli);

/* The saved profile (a copy). Returns how many devices. */
int purr_hwsetup_profile(purr_hwp_t *out);

/* 1 if this scan hit is already in the profile. */
int purr_hwsetup_is_set_up(const purr_hwscan_found_t *f);

/* Sets up every known device in the report that is not set up yet. progress gets a short line per step (may be NULL). summary says what
 * happened. Returns how many devices were set up; < 0 if nothing could be done at all (summary says why). */
int purr_hwsetup_install(const purr_hwscan_report_t *rep, void (*progress)(const char *), char *summary, size_t cap);

#define PURR_HWSETUP_MAX_DRIVERS 12
typedef struct {
    char file[40];
    char name[32];
    char version[12];
} purr_hwsetup_driver_t;

/* The driver files in /boot/drivers. */
int purr_hwsetup_list_drivers(purr_hwsetup_driver_t *out, int max);

/* Downloads newer versions of the installed drivers. Returns how many were updated, < 0 if the index could not be read. */
int purr_hwsetup_update_drivers(void (*progress)(const char *), char *summary, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PURR_HWSETUP_H */
