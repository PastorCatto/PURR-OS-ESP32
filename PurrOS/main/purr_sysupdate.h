/*
 * purr_sysupdate.h - updating and reinstalling the system from Settings: which versions are installed, whether the server has newer ones,
 * and asking the recovery loader to do the work. The loader is still the only thing that writes the system (it downloads, checks every
 * image against the keys, and writes only what is newer, or everything when the owner asked for a reinstall); this just sets it up.
 *
 * The developer server is ONE setting for everything (apps, drivers, system updates, reinstall): the one the owner chose last
 * (purr_repo_dev_last). Choosing it also records it for the loader, which uses it only while developer mode and the bootloader's
 * DEV_RECOVERY flag are on. Every call blocks except purr_sysupdate_request: call from a worker task.
 */
#ifndef PURR_SYSUPDATE_H
#define PURR_SYSUPDATE_H

#include <stddef.h>

#include "purr_repo_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The versions in the kernel and KittenOS slots ("?" if unreadable). */
void purr_sysupdate_versions(char kernel[16], char kitten[16]);

/* Compares the installed versions with the server's recovery.manifest: the official server, or the developer server (use_dev).
 * Returns 1 if something is newer, 0 if up to date, -1 if the server could not be read. msg gets a sentence for the screen. */
int purr_sysupdate_check(int use_dev, char *msg, size_t cap);

/* 1 if the last purr_sysupdate_check found a newer BOOTLOADER than the one running (it is swapped in by KittenOS with a backup, after the reinstall). */
int purr_sysupdate_includes_bootloader(void);

/* The developer server for everything: set from a typed address (resolved and checked to be local) or a discovered server.
 * 0 or -1 with err. Developer mode only. */
int purr_devsrv_set_typed(const char *typed, char *err, size_t errcap);
int purr_devsrv_set(const purr_repo_server_t *s);
int purr_devsrv_get(purr_repo_server_t *out);      /* 0 if one is set */

/* Asks the loader to update (force = 0: only what is newer) or to reinstall (force = 1: rewrite the kernel and KittenOS even if they look
 * good), from the developer server or the official one, keeping the data. 0 when set; the caller restarts. */
int purr_sysupdate_request(int use_dev, int force);

#ifdef __cplusplus
}
#endif

#endif /* PURR_SYSUPDATE_H */
