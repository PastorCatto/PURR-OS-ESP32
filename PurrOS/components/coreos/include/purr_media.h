/*
 * purr_media.h - the system's "now playing" session (plan7.md, "The Click Wheel shell"; Catcalls/SPEC.md, `media`).
 *
 * Any app that plays sound can report what it is playing here, and the system (the lock screen, the shell's playing screen, the buttons) shows it and sends the
 * transport commands back. The OS does not play anything itself: the session is a record and a small queue of commands for the one app that owns it. Which apps
 * may report is the "audio" permission's business (purr_perm.h); this file only keeps the state, so it is plain C and tested on the host.
 *
 * One session at a time. An app that is playing takes the session over from one that is not; one that is paused or stopped cannot take it from one that is
 * playing (the playing one keeps the buttons).
 */
#ifndef PURR_MEDIA_H
#define PURR_MEDIA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_MEDIA_TEXT 48
#define PURR_MEDIA_APP  32
#define PURR_MEDIA_CMDS 4               /* waiting commands per session; a fifth is dropped, the oldest stays (a held button must not flood an app) */

#define PURR_MEDIA_STOPPED 0
#define PURR_MEDIA_PLAYING 1
#define PURR_MEDIA_PAUSED  2

/* Commands the system sends to the owner. `arg` is the position in milliseconds for SEEK, else 0. */
#define PURR_MEDIA_CMD_PLAY   1
#define PURR_MEDIA_CMD_PAUSE  2
#define PURR_MEDIA_CMD_TOGGLE 3
#define PURR_MEDIA_CMD_NEXT   4
#define PURR_MEDIA_CMD_PREV   5
#define PURR_MEDIA_CMD_SEEK   6
#define PURR_MEDIA_CMD_STOP   7

/* What the app can do: the shell greys out the rest. */
#define PURR_MEDIA_CAP_PAUSE (1u << 0)  /* PLAY, PAUSE, TOGGLE */
#define PURR_MEDIA_CAP_SKIP  (1u << 1)  /* NEXT, PREV */
#define PURR_MEDIA_CAP_SEEK  (1u << 2)

typedef struct {
    char title[PURR_MEDIA_TEXT];
    char artist[PURR_MEDIA_TEXT];
    char album[PURR_MEDIA_TEXT];
    uint8_t state;                      /* PURR_MEDIA_* */
    uint8_t reserved;
    uint16_t track, tracks;             /* "3 of 12"; 0 = unknown */
    uint32_t position_ms;               /* where it is, as of the moment of the report */
    uint32_t duration_ms;               /* 0 = unknown (a stream) */
    uint32_t caps;                      /* PURR_MEDIA_CAP_* */
} purr_media_info_t;

typedef struct {
    char app[PURR_MEDIA_APP];           /* the owner; "" when there is no session */
    purr_media_info_t info;
    uint32_t reported_ms;               /* the clock at the last report, for the position of a playing track */
    uint8_t cmd[PURR_MEDIA_CMDS];
    uint32_t cmd_arg[PURR_MEDIA_CMDS];
    uint8_t ncmd;
    uint32_t changes;                   /* bumped on every change: the shell redraws when it moves */
} purr_media_t;

void purr_media_init(purr_media_t *m);

/* An app reports. 0; -1 for a bad argument (no app name, state out of range, a position past the duration); -2 when another app that is playing keeps the session.
 * Text is cut to fit. `now_ms` is any millisecond clock. */
int purr_media_report(purr_media_t *m, const char *app, const purr_media_info_t *info, uint32_t now_ms);

/* The app is done (it stopped, closed or called clear): its session goes, and its waiting commands with it. 0, or -1 if it is not the owner. */
int purr_media_clear(purr_media_t *m, const char *app);

/* 1 and a copy of the session if there is one. The position of a playing track is moved on to `now_ms` (and held at the duration). */
int purr_media_get(const purr_media_t *m, uint32_t now_ms, char *app_out, int app_cap, purr_media_info_t *out);

/* The system asks the owner to do something. 0; -1 for an unknown command or none to send it to; -2 when the app has not said it can (the CAP bits);
 * -3 when the queue is full. */
int purr_media_command(purr_media_t *m, int cmd, uint32_t arg);

/* The owner takes the next command. 1 and the command, or 0 when there is none (or `app` is not the owner). */
int purr_media_next_command(purr_media_t *m, const char *app, int *cmd, uint32_t *arg);

#ifdef __cplusplus
}
#endif

#endif /* PURR_MEDIA_H */
