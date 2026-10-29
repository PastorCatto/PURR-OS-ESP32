/*
 * purr_catcalls.h - the catcalls an app can ask for through env->catcall_get() (plan6.md,
 * AppRuntime/SPEC.md section 8, Catcalls/SPEC.md). Included by both sides: the runtime that
 * implements them, and an app's own compile.
 *
 * Each catcall is a table whose first field is its own version. An app asks for a name and a
 * minimum version and gets the table or NULL; functions an older table lacks are simply not
 * there, so an app checks the version it was handed before touching a field added later.
 * Every call is made on the calling app's own task, and everything an app creates through a
 * catcall (a timer, an open file, a subscription) is owned by that app and released when it
 * stops (AppRuntime/SPEC.md section 9).
 *
 * Catcall names, for catcall_get: "console", "app", "sched", "storage", "system", "event", "ctl", "ui", "power", "files".
 */
#ifndef PURR_CATCALLS_H
#define PURR_CATCALLS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ console */
typedef struct {
    uint32_t version;                                   /* 1 */
    /* Write bytes to the app's console: the terminal while it has console focus, the system log
     * (prefixed with the app's id and name) while it does not. */
    void (*write)(const char *buf, size_t len);
    void (*print)(const char *s);
    /* Read one line from the console, waiting up to timeout_ms (-1 forever). Only the app with
     * console focus receives input; any other gets 0 bytes. Returns the length, 0 if none. */
    int  (*read_line)(char *buf, size_t cap, int timeout_ms);
} purr_cc_console_t;

/* ---------------------------------------------------------------------- app */
typedef struct {
    int      id;                  /* the instance id, as in `purrctl apps` */
    char     name[32];
    uint8_t  state;               /* purr_rt_state_t */
    uint8_t  kind;                /* 0 command-line, 1 daemon, 2 ui */
    uint8_t  flags;               /* PURR_APP_F_* (purr_runtime.h) */
    uint8_t  privileged;
    uint32_t memory;              /* bytes the instance holds */
    uint32_t uptime_ms;
} purr_app_info_t;

typedef struct {
    uint32_t version;                                   /* 1 */
    int  (*info)(purr_app_info_t *out);
    /* Ask to be closed. on_destroy runs with PURR_APP_DESTROY_USER. */
    void (*close_self)(void);
    /* Ask for the foreground. The manager may refuse (a `nofocus` rule, or a daemon). */
    int  (*request_focus)(void);
    /* Post a bus event to this app's own queue (a way to defer work out of a callback). */
    int  (*post_self)(const char *name, const char *data);
} purr_cc_app_t;

/* -------------------------------------------------------------------- sched */
typedef struct {
    uint32_t version;                                   /* 1 */
    uint32_t (*uptime_ms)(void);
    void     (*sleep_ms)(uint32_t ms);                  /* never call this inside a callback for long */
    /* A timer owned by the app; it fires as a PURR_APPEV_TIMER event with arg = the id returned.
     * repeat != 0 re-arms it. Returns the timer id (>0) or <0 if the app has no timers left. */
    int      (*timer_start)(uint32_t ms, int repeat);
    void     (*timer_cancel)(int id);
} purr_cc_sched_t;

/* ------------------------------------------------------------------ storage */
#define PURR_CC_O_READ   1
#define PURR_CC_O_WRITE  2
#define PURR_CC_O_CREATE 4
#define PURR_CC_O_TRUNC  8
#define PURR_CC_O_APPEND 16

typedef struct {
    uint32_t version;                                   /* 1 */
    /* All paths are relative to the app's own data folder; ".." and absolute paths are refused. */
    int  (*open)(const char *path, int flags);          /* a handle >= 0, or < 0 */
    int  (*read)(int h, void *buf, size_t len);
    int  (*write)(int h, const void *buf, size_t len);
    int  (*seek)(int h, long offset, int whence);
    int  (*close)(int h);
    int  (*mkdir)(const char *path);
    int  (*remove)(const char *path);
    int  (*stat)(const char *path, uint32_t *size, int *is_dir);
    /* Calls cb(name, is_dir, size, ctx) for each entry. */
    int  (*list)(const char *path,
                 void (*cb)(const char *name, int is_dir, uint32_t size, void *ctx), void *ctx);
} purr_cc_storage_t;

/* ------------------------------------------------------------------- system */
typedef struct {
    uint32_t version;                                   /* 1 */
    uint32_t (*uptime_ms)(void);
    uint32_t (*free_heap)(void);
    uint32_t (*largest_free_block)(void);
    int      (*battery_percent)(void);                  /* -1 if the board has no gauge */
    int64_t  (*unix_time)(void);                        /* seconds, or 0 if the clock is unset */
} purr_cc_system_t;

/* -------------------------------------------------------------------- event */
/* The bus behind `purrctl events`. Events are `name>>data` lines. */
typedef struct {
    uint32_t version;                                   /* 1 */
    /* Publish an event. The name is namespaced for you: "custom.<yourname>.<name>". */
    int  (*emit)(const char *name, const char *data);
    /* Receive events whose full name starts with prefix, as PURR_APPEV_USER events. Up to four
     * subscriptions per app. */
    int  (*subscribe)(const char *prefix);
    void (*unsubscribe)(const char *prefix);
} purr_cc_event_t;

/* ---------------------------------------------------------------------- ctl */
/* Control of the app system itself: the dispatchers and queries of `purrctl`. Privileged apps
 * only (it is how a launcher or a task manager is written). */
typedef struct {
    uint32_t version;                                   /* 1 */
    /* Run a dispatcher line, e.g. "focus notes". The reply (or error text) goes to out.
     * Returns 0 on success, <0 on failure. */
    int  (*dispatch)(const char *line, char *out, size_t cap);
    /* Run a query, e.g. "apps -j". */
    int  (*query)(const char *line, char *out, size_t cap);
} purr_cc_ctl_t;

/* ----------------------------------------------------------------------- ui */
/* The window an app draws in. The OS owns every widget; the app holds small integer handles (> 0), never
 * pointers, and everything it makes is deleted when it stops. Colours and fonts come only from the system
 * theme (accent, light or dark, transparency), so an app looks like the rest of the OS without styling code.
 * Taps and typing come back as PURR_APPEV_UI events on the app's own task. Only an app whose kind is "ui"
 * is granted this catcall. */
#define PURR_UI_ROLE_TEXT   0     /* normal text */
#define PURR_UI_ROLE_DIM    1     /* secondary text */
#define PURR_UI_ROLE_TITLE  2     /* a heading in the accent colour */
#define PURR_UI_ROLE_ACCENT 3     /* emphasis in the accent colour */

/* The header's action button (win_action), by name of the symbol: */
#define PURR_UI_ACT_NONE    0
#define PURR_UI_ACT_PLUS    1
#define PURR_UI_ACT_TRASH   2
#define PURR_UI_ACT_CHECK   3

#define PURR_CC_UI_MAX_WIDGETS 48    /* per app */
#define PURR_CC_UI_MAX_TEXT    2048  /* bytes in one text area */

typedef struct {
    uint32_t version;                                   /* 1 */
    /* The window header: a title, and one optional action button on the right (PURR_UI_ACT_*). */
    void (*win_title)(const char *title);
    void (*win_action)(int action);
    /* Who handles the back arrow: 0 (the default) closes the app; 1 sends the app a "back" event and leaves
     * it to the app (it calls app->close_self when there is nothing left to go back to). */
    void (*win_back)(int app_handles);
    /* Deletes every widget the app has made. The window stays. */
    void (*clear)(void);
    /* Each creates a widget at the end of the window's column and returns its handle, or < 0 (too many
     * widgets, out of memory, no window). */
    int  (*label)(const char *text, int role);          /* PURR_UI_ROLE_* */
    int  (*button)(const char *text);                   /* "click" */
    int  (*row)(const char *title, const char *sub);    /* a tappable list row, "click"; sub may be NULL */
    int  (*textarea)(int max_len, const char *placeholder, int fill);  /* "changed"; fill != 0 takes the
                                                            * rest of the window's height */
    int  (*spacer)(int height);
    void (*set_text)(int h, const char *text);
    /* Copies a text area's text (or a label's) into buf, always NUL-terminated; returns the length. */
    int  (*get_text)(int h, char *buf, size_t cap);
    void (*focus)(int h);                               /* the keyboard types into this text area */
    void (*remove)(int h);
} purr_cc_ui_t;


/* -------------------------------------------------------------------- power */
/* Keeping the device awake, and waking it on a schedule (plan10.md). Both are dangerous permissions
 * ("keep-awake", "wake-alarm", Permissions/SPEC.md): the first call asks the user, and answers PURR_POWER_ASK
 * until they decide; the app then gets PURR_APPEV_PERM (arg 1 = allowed, 0 = not, name = the permission) and
 * calls again. A background app is never asked: it is denied unless it was allowed earlier.
 *
 * A CPU lock has a lease and expires unless renewed. An alarm wakes the app every interval with PURR_APPEV_WAKE
 * (arg = the alarm index, name "alarm") and keeps the device awake for window_ms, or until the app calls done(),
 * whichever comes first. Alarms are for apps allowed to run in the background. Three windows in a row that
 * ran out switch the alarm off; PURR_APPEV_WAKE then arrives with the name "alarm-off". */
#define PURR_CC_WAKE_SCREEN  1      /* the screen stays on while the app is in front */
#define PURR_CC_WAKE_CPU     2      /* the CPU stays out of light sleep, for a lease */
#define PURR_CC_ALARM_ALIGN  1      /* the OS may move the wake up to a fifth of the interval early, to share it */

#define PURR_POWER_OK         0
#define PURR_POWER_BAD       -1     /* a bad argument, or not allowed for this kind of app */
#define PURR_POWER_DENIED    -2
#define PURR_POWER_ASK       -3     /* the user is being asked: wait for PURR_APPEV_PERM, then call again */
#define PURR_POWER_NO_ALARM  -4     /* both of the app's alarms are in use */

typedef struct {
    uint32_t version;                                   /* 1 */
    /* kind: PURR_CC_WAKE_*. lease_ms applies to the CPU lock (0 = 30 s, at most 120 s). */
    int  (*hold)(int kind, uint32_t lease_ms);
    int  (*renew)(int kind, uint32_t lease_ms);
    void (*release)(int kind);
    /* interval_ms >= 5000. window_ms 0 = 10 s, at most 30 s. Returns the alarm index (0 or 1), or < 0. */
    int  (*alarm_set)(uint32_t interval_ms, uint32_t window_ms, int flags);
    void (*alarm_cancel)(int index);
    /* Finished the work of this wake: the device may go back to sleep. */
    void (*done)(void);
    int  (*screen_is_on)(void);
} purr_cc_power_t;

/* -------------------------------------------------------------------- files */
/* Files outside the app's own folder, for a file manager. Paths are absolute, in the signed-in user's view ("/home/<user>/..."); ".." and
 * "." are refused. Two dangerous permissions (Permissions/SPEC.md), asked at first use like `power`: a call answers PURR_FILES_ASK until the
 * user decides, then the app gets PURR_APPEV_PERM and calls again.
 *   "shared-storage": read and change files under the user's home folder.
 *   "system-files":   read (never change) files anywhere else; administrators only.
 * The permission file /home/<user>/permissions.conf can be read but never changed through this. A file is copied whole (at most 64 KB). */
#define PURR_FILES_OK      0
#define PURR_FILES_BAD    -1    /* a bad path, a missing file, or the file system refused */
#define PURR_FILES_DENIED -2
#define PURR_FILES_ASK    -3    /* the user is being asked: wait for PURR_APPEV_PERM, then call again */

typedef struct {
    uint32_t version;                                   /* 1 */
    int  (*home)(char *buf, size_t cap);                /* "/home/<user>"; returns its length, or < 0 */
    int  (*is_admin)(void);                             /* 1 if "system-files" can ever be granted to this user */
    int  (*list)(const char *path, void (*cb)(const char *name, int is_dir, uint32_t size, void *ctx), void *ctx);
    int  (*stat)(const char *path, uint32_t *size, int *is_dir);
    /* Up to len bytes of the file starting at offset; returns the bytes read (0 at the end), or < 0. */
    int  (*read)(const char *path, uint32_t offset, void *buf, size_t len);
    int  (*mkdir)(const char *path);
    int  (*remove)(const char *path);                   /* a file or an empty folder */
    int  (*rename)(const char *from, const char *to);
    int  (*copy)(const char *from, const char *to);     /* a file, not a folder */
} purr_cc_files_t;

#ifdef __cplusplus
}
#endif

#endif /* PURR_CATCALLS_H */
