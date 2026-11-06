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
 * Catcall names, for catcall_get: "console", "app", "sched", "storage", "system", "event", "ctl", "ui", "power", "files", "devices", "store".
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
/* The time of day in the device's time zone (Settings > Date and time), broken down. */
typedef struct {
    int year;                                           /* 2026 */
    int mon;                                            /* 1 to 12 */
    int mday;                                           /* 1 to 31 */
    int hour, min, sec;
    int wday;                                           /* 0 = Sunday */
} purr_cc_tm_t;

typedef struct {
    uint32_t version;                                   /* 2 (1 had no local_time; ask for it with catcall_get("system", 2)) */
    uint32_t (*uptime_ms)(void);
    uint32_t (*free_heap)(void);
    uint32_t (*largest_free_block)(void);
    int      (*battery_percent)(void);                  /* -1 if the board has no gauge */
    int64_t  (*unix_time)(void);                        /* seconds, or 0 if the clock is unset */
    /* Version 2. Fills `out` with the local time and returns 0, or < 0 if the clock has not been set. */
    int      (*local_time)(purr_cc_tm_t *out);
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
/* Version 3 (OS 1.0.23) roles; an older OS draws them as PURR_UI_ROLE_TEXT. */
#define PURR_UI_ROLE_BIG    4     /* 40 pt digits and headings: a clock, a calculator's result */
#define PURR_UI_ROLE_MEDIUM 5     /* 28 pt */
#define PURR_UI_ROLE_MONO   6     /* an 8 x 16 monospace face, for a terminal */

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
    /* Version 2. A message for the status strip at the top of the screen: it takes the strip for 5 seconds (bad = 1: an error, in amber).
     * Use it for what a button did ("Copied", "Could not delete it"); an error also belongs in the page, next to the button that failed. */
    void (*status)(const char *msg, int bad);
    /* Version 3 (OS 1.0.23; ask for it with catcall_get("ui", 3), an older OS answers NULL). */
    /* A grid of buttons: `cols` across, each `row_h` pixels tall (20 to 80). Returns the grid's handle; grid_button() adds the next button to it (they fill
     * left to right and wrap) and returns the button's handle, which gets "click" like any button. The buttons can be reached with the trackball. */
    int  (*grid)(int cols, int row_h);
    int  (*grid_button)(int grid, const char *text);
    /* A one-line text field (typing, and the keyboard's Enter, arrive as "changed"; read it with get_text, fill it with set_text). */
    int  (*input_line)(int max_len, const char *placeholder);
    /* A bar from 0 to max, and a slider from min to max ("changed" while it moves). set_value / get_value work on both. */
    int  (*progress)(int value, int max);
    int  (*slider)(int min, int max, int value);
    void (*set_value)(int h, int value);
    int  (*get_value)(int h);
    /* A picture frame `height` pixels tall and as wide as the window; set_image() fills it from a JPEG, PNG or BMP under the user's files (the `files` catcall's
     * paths and its "shared-storage" permission: it answers like that catcall, PURR_FILES_*, and may ask). The picture is scaled to fit. Returns 0, or
     * PURR_UI_IMG_TOO_BIG (more than about a megapixel or 2 MB), PURR_UI_IMG_UNSUPPORTED, or a PURR_FILES_* code. */
    int  (*image)(int height);
    int  (*set_image)(int h, const char *path);
} purr_cc_ui_t;
#define PURR_UI_IMG_TOO_BIG      -4
#define PURR_UI_IMG_UNSUPPORTED  -5


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

/* -------------------------------------------------------------------- devices */
/* Finding and setting up hardware, keeping drivers and the system up to date, and the developer server (what the Device Manager app does).
 * One dangerous permission, "manage-devices", for administrators only: the first call asks, answers PURR_DEVICES_ASK until they decide, and the app then
 * gets PURR_APPEV_PERM and calls again.
 *
 * Work that takes time runs on its own task: start() returns at once, and when it ends the app gets PURR_APPEV_UI with the name "job" (arg 0 = it worked,
 * 1 = it did not, 2 = it worked and, for "check", a newer system is out); result() then holds a sentence for the person. One job at a time (start() says PURR_DEVICES_BUSY).
 *
 * start(what, a, b) with what =
 *   "find"            look for attached hardware (I2C), reading only
 *   "setup"           set up everything the last "find" recognised
 *   "update-drivers"  download newer versions of the installed drivers
 *   "get-drivers"     get the driver of every built-in device that has none (the SD card's, the GPS's...); they start at once
 *   "get-driver"      get one of them: a = the driver's name as list("missing") gives it in b (OS 1.0.23)
 *   "retry-driver"    put back a driver that stopped the device (list("crashed")); a = its name; it starts at the next restart (OS 1.0.23)
 *   "add"             add a device by hand: a = driver name, b = "<sda> <scl> <addr>"
 *   "read"            a reading from a running device: a = its name
 *   "remove"          remove a found device from the profile: a = its name
 *   "check"           is a newer system out? a = "official" or "dev"
 *   "update"          ask the loader to update the system: a = "official" or "dev"   (the job ends with the request set; the device restarts when told to)
 *   "reinstall"       the same, rewriting the kernel and KittenOS: a = "official" or "dev"
 *   "dev-find"        look for developer servers on the network
 *   "dev-use"         use developer server number a (from list("servers"))
 *   "dev-set"         use the developer server at the typed address a
 * list(what, i, a, acap, b, bcap): row i of "found", "profile", "board", "missing" (built-in devices with no driver; b is `needs the "<driver>" driver`), "crashed" (drivers set aside after the device stopped starting them, 1.0.23), "drivers" or "servers" (a = title, b = a line under it); 1 if there is such a row, 0 if not.
 * info(what, out, cap): "versions" (kernel and KittenOS), "devserver" (the one in use), "dev-mode" ("1" or "0"), "to-set-up" (how many found devices are not set up), "missing" (how many built-in devices have no driver).
 * restart() restarts the device (after an update or new drivers). */
#define PURR_DEVICES_OK       0
#define PURR_DEVICES_BAD     -1
#define PURR_DEVICES_DENIED  -2
#define PURR_DEVICES_ASK     -3
#define PURR_DEVICES_BUSY    -4

typedef struct {
    uint32_t version;                                   /* 1 */
    int  (*is_admin)(void);
    int  (*start)(const char *what, const char *a, const char *b);
    int  (*result)(char *out, size_t cap);              /* the last job's sentence; its length */
    int  (*list)(const char *what, int i, char *a, size_t acap, char *b, size_t bcap);
    int  (*info)(const char *what, char *out, size_t cap);
    void (*restart)(void);
} purr_cc_devices_t;

/* -------------------------------------------------------------------- store */
/* Repositories, and installing, updating and removing what they offer (Repositories/SPEC.md): what the Store app and its companion may do.
 * One dangerous permission, "install-apps": the first call asks (PURR_STORE_ASK until answered, then PURR_APPEV_PERM). Adding or removing a repository
 * also needs an administrator, because it decides what the device trusts.
 *
 * Work that takes time runs on its own task: start() returns at once, and when it ends the app gets PURR_APPEV_UI with the name "job" (arg 0 = it worked,
 * 1 = it did not, 2 = it worked and has something to confirm: after "repo-add"); result() holds a sentence. One job at a time (PURR_STORE_BUSY).
 *
 * start(what, a, b) with what =
 *   "repo-add"      fetch the release file at the address a (https), check it, and hold it for confirmation: pending() then describes it
 *   "repo-confirm"  trust the repository that is pending (the person has read its name and fingerprint)
 *   "repo-remove"   forget the repository whose id is a (its apps stay installed but stop being vouched for: they ask to be reinstalled)
 *   "refresh"       fetch the signed index of repository a (or all, if a is "") and check it
 *   "install"       install item a of repository b (an app, an asset or an extra); an update is an install of the newer version
 *   "update-all"    install every update that is offered
 *   "remove"        remove item a (b = its repository id)
 *   "check"         refresh, then count the updates; if there are any, a notification says so (what a schedule calls)
 *   "bootstrap"     add the official repository and install the Store and its companion (what setup calls)
 * item(i) lists what the trusted repositories offer, from their last refresh. updates() counts the items with a newer version. */
#define PURR_STORE_OK       0
#define PURR_STORE_BAD     -1
#define PURR_STORE_DENIED  -2
#define PURR_STORE_ASK     -3
#define PURR_STORE_BUSY    -4

#define PURR_STORE_NOT_INSTALLED 0
#define PURR_STORE_INSTALLED     1
#define PURR_STORE_UPDATE        2

typedef struct {
    char name[32];
    char title[32];
    char version[16];
    char installed[16];                 /* the installed version, "" if none */
    char desc[80];
    char repo[40];                      /* the repository's name */
    char repo_id[20];
    char tier[12];                      /* "official", "community" or "yours" */
    char perms[48];                     /* comma separated */
    char category[16];
    uint32_t size;
    uint8_t type;                       /* 0 app, 1 asset, 2 extra */
    uint8_t target;                     /* 0 flash, 1 SD card, 2 either */
    uint8_t needs_sd;
    uint8_t state;                      /* PURR_STORE_* */
} purr_store_item_t;

typedef struct {
    uint32_t version;                                   /* 1 */
    int  (*is_admin)(void);
    int  (*start)(const char *what, const char *a, const char *b);
    int  (*result)(char *out, size_t cap);              /* the last job's sentence; its length */
    int  (*pending)(char *out, size_t cap);             /* what "repo-add" found, to show before confirming: name, maintainer, fingerprint, tier */
    int  (*repo)(int i, char *name, size_t ncap, char *info, size_t icap, char *id, size_t idcap);   /* a trusted repository: 1 if there is a row i */
    int  (*item_count)(void);
    int  (*item)(int i, purr_store_item_t *out);        /* 0 or a negative PURR_STORE_* */
    int  (*updates)(void);
    int  (*sd_present)(void);
    /* A Store setting (keys starting "store."): "store.auto_update" (1 = install updates when a check finds them), "store.check_hours" (how often to check). */
    int  (*setting)(const char *key, int fallback);
} purr_cc_store_t;

/* ------------------------------------------------------------------ game
 * For an app of kind `game` only (PURR_APP_KIND_GAME; plan6.md, "The game surface"). While the game is in front the shell stops drawing and the game owns the
 * screen and the buttons; the OS takes both back when the game leaves the front or stops, and holding the menu key for a second sends the game back to the
 * start screen whatever it is doing. Nothing here works while the game is not in front: lcd_write() refuses and buttons() reads 0.
 * The game paces itself (a sched timer is the simplest way: one frame per PURR_APPEV_TIMER event, each finished well inside a callback's 3 seconds). */
#define PURR_GBTN_UP     (1u << 0)
#define PURR_GBTN_DOWN   (1u << 1)
#define PURR_GBTN_LEFT   (1u << 2)
#define PURR_GBTN_RIGHT  (1u << 3)
#define PURR_GBTN_A      (1u << 4)
#define PURR_GBTN_B      (1u << 5)
#define PURR_GBTN_X      (1u << 6)
#define PURR_GBTN_Y      (1u << 7)
#define PURR_GBTN_L      (1u << 8)
#define PURR_GBTN_R      (1u << 9)
#define PURR_GBTN_START  (1u << 10)
#define PURR_GBTN_SELECT (1u << 11)
#define PURR_GBTN_OPTION (1u << 12)
#define PURR_GBTN_MENU   (1u << 13)   /* held: the OS takes the game back to the start screen after a second */

typedef struct {
    uint32_t version;                                   /* 1 */
    uint16_t width, height;                             /* the panel, in the screen's current orientation */
    /* Copies a w by h rectangle of host-endian RGB565 to the panel (clipped); returns when the data is sent. 0, or < 0 if the game is not in front. */
    int      (*lcd_write)(int x, int y, int w, int h, const uint16_t *pixels);
    uint32_t (*buttons)(void);                          /* the PURR_GBTN_* bits held right now; 0 when the controls are not available */
    uint64_t (*ticks_us)(void);                         /* microseconds since boot */
    void     (*quit)(void);                             /* close this game */
} purr_cc_game_t;

#ifdef __cplusplus
}
#endif

#endif /* PURR_CATCALLS_H */
