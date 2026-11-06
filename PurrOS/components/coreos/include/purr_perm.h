/*
 * purr_perm.h - which apps may use which dangerous feature (Permissions/SPEC.md, plan10.md).
 *
 * Android's model: an app asks at the moment it first reaches for the feature; the user answers "allow always",
 * "just once" or "deny"; after repeated denials the user may say "don't ask again". Grants belong to a user and
 * are kept in /home/<user>/permissions.conf. A privileged app has every permission and is never asked.
 *
 * This file is the table and its text format only; asking the user and storing the file are the shell's job.
 * Plain C, no ESP-IDF, host-tested.
 */
#ifndef PURR_PERM_H
#define PURR_PERM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The permissions this table knows. More are added here as catcalls need them (Permissions/SPEC.md section 2). */
#define PURR_PERM_KEEP_AWAKE 0      /* "keep-awake": hold the screen or the CPU awake */
#define PURR_PERM_WAKE_ALARM 1      /* "wake-alarm": wake the device on a schedule */
#define PURR_PERM_SHARED_STORAGE 2  /* "shared-storage": see and change files outside the app's own folder (the user's home) */
#define PURR_PERM_SYSTEM_FILES   3  /* "system-files": read system files, read-only (administrators only) */
#define PURR_PERM_MANAGE_DEVICES 4  /* "manage-devices": find, set up and remove hardware, update drivers and the system (administrators only) */
#define PURR_PERM_INSTALL_APPS 5    /* "install-apps": install, update and remove apps and downloads from the repositories the system trusts */
#define PURR_PERM_MESSAGING 6       /* "messaging": read and send messages through the shared message layer, and list the messaging services (Messaging/SPEC.md) */
#define PURR_PERM_CONTACTS  7       /* "contacts": read and change the address book (Messaging/SPEC.md) */
#define PURR_PERM_RADIO     8       /* "radio": ask for the LoRa radio lease (Messaging/SPEC.md section 4) */
#define PURR_PERM_COUNT      9

/* What the user has said about one permission of one app. */
#define PURR_PERM_ASK    0          /* not decided: ask at first use */
#define PURR_PERM_ALWAYS 1          /* saved */
#define PURR_PERM_ONCE   2          /* until the app stops; never saved */
#define PURR_PERM_NEVER  3          /* "don't ask again"; saved */

/* The answer to "may this app do this now?". */
#define PURR_PERM_YES     1
#define PURR_PERM_NO     -1
#define PURR_PERM_PROMPT  0         /* ask the user (a background app is refused instead of asked) */

#define PURR_PERM_MAX_APPS 16
#define PURR_PERM_APP_LEN  32
#define PURR_PERM_DENIALS_BEFORE_NEVER 2    /* the prompt offers "don't ask again" from the 3rd time */

typedef struct {
    char app[PURR_PERM_APP_LEN];
    uint8_t state[PURR_PERM_COUNT];
    uint8_t denials[PURR_PERM_COUNT];       /* this session only */
} purr_perm_entry_t;

typedef struct {
    purr_perm_entry_t e[PURR_PERM_MAX_APPS];
    int count;
} purr_perm_t;

const char *purr_perm_name(int perm);               /* "keep-awake", or NULL */
int purr_perm_by_name(const char *name);            /* -1 if unknown */
const char *purr_perm_describe(int perm);           /* "keep the device awake", for the prompt */

int purr_perm_state(const purr_perm_t *t, const char *app, int perm);      /* PURR_PERM_ASK if never seen */
int purr_perm_decide(const purr_perm_t *t, const char *app, int perm, int privileged);
int purr_perm_set(purr_perm_t *t, const char *app, int perm, int state);   /* 0, or -1 (bad input / table full) */
int purr_perm_deny(purr_perm_t *t, const char *app, int perm);             /* counts one denial; returns the count */
int purr_perm_denials(const purr_perm_t *t, const char *app, int perm);
void purr_perm_app_stopped(purr_perm_t *t, const char *app);               /* drops "just once" grants */
void purr_perm_forget(purr_perm_t *t, const char *app);                    /* the app was removed */
void purr_perm_reset(purr_perm_t *t, const char *app);                     /* every permission asks again */

/* "app:permission=always|never" lines. Format writes only saved states. Parse ignores what it cannot read. */
size_t purr_perm_format(const purr_perm_t *t, char *out, size_t cap);
void purr_perm_parse(purr_perm_t *t, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* PURR_PERM_H */
