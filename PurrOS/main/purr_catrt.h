/*
 * purr_catrt.h - the device side of the app runtime (plan6.md): runs the manager in
 * components/coreos/src/purr_runtime.c on FreeRTOS.
 *
 * Every running app gets its own task and event queue; every manager call is serialized on one
 * manager task, so purr_runtime.c stays single-threaded and needs no locks. The manager waits for
 * an app's callback to return (with a timeout) and an app never waits for the manager: an app's
 * calls into the manager (close_self, request_focus, event emit) are posted and return at once.
 *
 * The platform (loading a signed .cat, the filesystem, the console) is handed in by commands.c,
 * which owns those statics.
 */
#ifndef PURR_CATRT_H
#define PURR_CATRT_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "purr_app_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Find, verify and load an app by name. On success *table is the app's v1 or v2 table and
     * *handle is an opaque value for unload. Returns 0; PURR_RT_HOST_UNTRUSTED (-2) if the image is
     * unsigned, altered, or signed by a key that may not sign apps (such an app never runs, in
     * any secure mode); any other negative value if it cannot be found. */
    int  (*load)(const char *name, const purr_app_table_t **table, void **handle);
    void (*unload)(void *handle);
    /* Saved state, kept per app name. load_state returns the length, 0 for none, < 0 on error. */
    int  (*save_state)(const char *name, const void *buf, uint32_t len);
    int  (*load_state)(const char *name, void *buf, uint32_t cap);
    /* Output of the app that has console focus (the terminal). */
    void (*console_write)(const char *buf, size_t len);
    /* Whole-file read for /etc/apps.conf: returns the length (NUL-terminated), or < 0. */
    int  (*read_file)(const char *path, char *buf, size_t cap);
    /* Whole-file write, creating parent directories; used to plant defaults and test apps. */
    int  (*write_file)(const char *path, const void *buf, size_t len);
} purr_catrt_platform_t;

/* Starts the manager task and loads /etc/apps.conf (planting a default one the first time), then
 * opens its exec-once apps. Safe to call once, after the filesystem is mounted. */
esp_err_t purr_catrt_start(const purr_catrt_platform_t *platform);

/* Tells the runtime a user has signed in. The apps listed as `exec-once` in /etc/apps.conf start now
 * (the first time this is called after boot), not at boot: they are user apps, they live in the
 * user's own folder, and nobody was there to run them for. The ones listed as `exec-boot`, the
 * system needs with nobody logged in, are already running. Safe from any task; returns at once. */
void purr_catrt_user_logged_in(void);

/* The control surface, callable from any task (the shell, a UI, an app's ctl catcall). Each is a
 * synchronous request to the manager task. Return 0 or a negative error; the reply text is in out. */
int purr_catrt_dispatch(const char *line, char *out, size_t cap);
int purr_catrt_query(const char *line, char *out, size_t cap);
int purr_catrt_key(unsigned mods, const char *key);
/* Re-reads /etc/apps.conf. The number of bad lines is returned (0 = clean) and described in err. */
int purr_catrt_reload(char *err, size_t errcap);

/* The event stream, for `purrctl events` and for a UI. A reader keeps its own cursor (start it at
 * purr_catrt_event_cursor()) and calls next() until it returns 0. Events a slow reader missed are
 * skipped, not queued without bound. */
uint32_t purr_catrt_event_cursor(void);
int purr_catrt_event_next(uint32_t *cursor, char *line, size_t cap);

/* ------------------------------------------------------------------ catcalls provided elsewhere
 * `ui` needs the display, `storage` the filesystem: each is a table another file hands the runtime, so this
 * file stays free of both. An app can ask only for what is registered (and `ui` only if its kind is ui). */
typedef void (*purr_catrt_ui_lifecycle_fn)(int slot, const char *app, int rt_event);   /* PURR_RT_EV_* */
void purr_catrt_set_ui(const void *table, purr_catrt_ui_lifecycle_fn lifecycle);
/* release(slot) runs when an app is unloaded, to close what it left open. */
void purr_catrt_set_storage(const void *table, void (*release)(int slot));

/* For those tables: who is calling (the app's slot 0..PURR_RT_MAX_APPS-1, and its name), or -1 when the
 * calling task is not an app. */
int purr_catrt_caller(char *name, size_t cap);
/* Queues a PURR_APPEV_UI event on an app's own task. Never blocks; safe from the UI task. */
int purr_catrt_post_ui(int slot, int handle, const char *what);
/* The app's own close request (what app->close_self does), for the shell's back arrow. */
void purr_catrt_close_slot(int slot);


/* What the power and permission code needs to know about a running app (plan10.md). slot is the same number
 * purr_catrt_caller returns. Returns 0, or -1 if no app is in that slot. */
typedef struct {
    char name[32];
    int foreground;             /* has the front of the screen now */
    int privileged;
    int background_ok;          /* may keep running when it leaves the front (a daemon, or a rule says so) */
    int kind;                   /* PURR_APP_KIND_* */
} purr_catrt_slot_info_t;
int purr_catrt_slot_info(int slot, purr_catrt_slot_info_t *out);
int purr_catrt_slot_of(const char *name);                      /* -1 if it is not running */
/* Queues an event to an app's own task, never blocking. type is PURR_APPEV_*. */
int purr_catrt_post_event(int slot, int type, int arg, const char *name);
/* The power code registers to hear when an app stops and when it leaves the front. */
void purr_catrt_set_files(const void *table);
void purr_catrt_set_power(const void *table, void (*stopped)(int slot, const char *name), void (*paused)(int slot));

#ifdef __cplusplus
}
#endif

#endif /* PURR_CATRT_H */
