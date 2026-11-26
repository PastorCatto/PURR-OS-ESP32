/*
 * purr_catrt_media.h - the `media` catcall and the system's now-playing session behind it (purr_media.h, plan7.md "The Click Wheel shell").
 *
 * An app reports what it plays (the "audio" permission is asked at the first report); the shell reads the session to draw the lock screen and the playing screen,
 * and sends the buttons back as commands, which reach the app as the event PURR_APPEV_USER "media.cmd".
 */
#ifndef PURR_CATRT_MEDIA_H
#define PURR_CATRT_MEDIA_H

#include <stddef.h>
#include <stdint.h>

#include "purr_media.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the catcall with the app runtime. Once, before apps run (after purr_catrt_power_init, whose prompt it uses). */
void purr_catrt_media_init(void);

/* The shell. 1 and a copy if an app is reporting (the position is brought up to now); 0 when nothing is, or when the owner has stopped running (its session is dropped). */
int purr_media_dev_get(char *app, size_t app_cap, purr_media_info_t *out);
/* PURR_MEDIA_CMD_*: 0, or the negative answer of purr_media_command (-2: the app cannot do that; -3: it has not caught up). */
int purr_media_dev_command(int cmd, uint32_t arg);
/* Changes on every report, command-free: redraw when it moves. */
uint32_t purr_media_dev_changes(void);
/* Brings the app that owns the session to the front (the playing screen's "open the player"). 0, or -1. */
int purr_media_dev_open_owner(void);

#ifdef __cplusplus
}
#endif

#endif /* PURR_CATRT_MEDIA_H */
