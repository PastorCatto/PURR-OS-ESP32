/*
 * devices.c - Device Manager, a sideloaded UI app. No libc: only the catcalls it is handed.
 *
 * Everything that used to be in Settings > Devices and About: find hardware and set it up, add a device by hand, see and remove found devices,
 * drivers and their updates, the developer server, and system updates and reinstall. It needs the "manage-devices" permission (administrators only);
 * the first call asks, and this app waits for the answer (PURR_APPEV_PERM).
 *
 * Every button's outcome goes to the status strip at the top (5 seconds), and an error also sits right under the button that failed.
 * Work that takes time runs in the OS; the app hears "job" when it ends. Back goes up one page, then closes the app.
 */
#include <stddef.h>
#include <stdint.h>

#include "purr_app_abi.h"
#include "purr_catcalls.h"

#define NO_OPT __attribute__((noinline, optimize("no-tree-loop-distribute-patterns")))
#define MAX_ROWS 12
#define MAX_PAIRS 6
#define TXT 96

static const purr_cc_ui_t *ui;
static const purr_cc_devices_t *dv;
static const purr_cc_app_t *app;

enum { PG_HOME, PG_FIND, PG_DEVICES, PG_ITEM, PG_ADD, PG_DRIVERS, PG_UPDATE, PG_SERVER, PG_WAIT };
enum { J_NONE, J_FIND, J_SETUP, J_DRV, J_ADD, J_READ, J_REMOVE, J_CHECK, J_UPDATE, J_DEVFIND, J_DEVUSE, J_DEVSET };

static int s_page = PG_HOME;
static int s_job;
static int s_dev_src;                                /* 1 = the developer server is the update source */
static int s_newer;                                  /* the last check found something */
static int s_armed;                                  /* a two-tap button was tapped once */
static int s_cur_msg;                                /* the message label under the button that was just pressed */
static char s_item[40];                              /* the device on the item page */
static char s_retry_what[16], s_retry_a[72], s_retry_b[72];
static int s_retry_job;

/* buttons and the message label under each */
static int s_pair_btn[MAX_PAIRS], s_pair_msg[MAX_PAIRS], s_npair;
static int s_row[MAX_ROWS], s_row_kind[MAX_ROWS], s_nrow;       /* kind 1: a found device (item page), 2: a developer server (use it) */
static int h_find, h_setup, h_drv, h_getdrv, h_add, h_read, h_remove, h_check, h_install, h_reinstall, h_src, h_devfind, h_devset;
static int h_driver, h_sda, h_scl, h_addr, h_srvaddr;
static int h_home_row[8];

/* ---------------------------------------------------------------- tiny string helpers */

NO_OPT static int slen(const char *s)
{
    int n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

NO_OPT static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

NO_OPT static void put(char *dst, const char *src, int cap)
{
    int i = 0;
    while (src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

NO_OPT static void append(char *dst, const char *src, int cap)
{
    int n = slen(dst);
    put(dst + n, src, cap - n);
}

NO_OPT static void append_num(char *dst, int v, int cap)
{
    char t[12], o[12];
    int k = 0, n = 0;
    if (v < 0) {
        append(dst, "-", cap);
        v = -v;
    }
    do {
        t[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0 && k < 11);
    while (k > 0) {
        o[n++] = t[--k];
    }
    o[n] = '\0';
    append(dst, o, cap);
}

NO_OPT static int to_int(const char *s)
{
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

/* ---------------------------------------------------------------- status and buttons */

NO_OPT static void status(const char *t, int bad)
{
    if (ui->version >= 2 && ui->status != NULL) {
        ui->status(t, bad);                          /* the strip at the top, 5 seconds */
    }
}

/* An error: the strip, and the line right under the button that failed. */
NO_OPT static void fail(const char *t)
{
    status(t, 1);
    if (s_cur_msg > 0) {
        ui->set_text(s_cur_msg, t);
    }
}

/* A button with its (empty) message line under it. */
NO_OPT static int action(const char *text)
{
    int b = ui->button(text);
    int m = ui->label("", PURR_UI_ROLE_DIM);
    if (s_npair < MAX_PAIRS) {
        s_pair_btn[s_npair] = b;
        s_pair_msg[s_npair] = m;
        s_npair++;
    }
    return b;
}

NO_OPT static void mark_pressed(int h)
{
    s_cur_msg = 0;
    for (int i = 0; i < s_npair; i++) {
        if (s_pair_btn[i] == h) {
            s_cur_msg = s_pair_msg[i];
        }
    }
}

NO_OPT static void begin_page(int page, const char *title, int plus)
{
    s_page = page;
    s_npair = 0;
    s_nrow = 0;
    s_armed = 0;
    s_cur_msg = 0;
    ui->clear();
    ui->win_back(1);
    ui->win_action(plus ? PURR_UI_ACT_PLUS : PURR_UI_ACT_NONE);
    ui->win_title(title);
}

static void show_home(void);
static void show_find(void);
static void show_devices(void);
static void show_item(const char *name);
static void show_add(void);
static void show_drivers(void);
static void show_update(void);
static void show_server(void);

NO_OPT static void show_wait(const char *l1, const char *l2)
{
    begin_page(PG_WAIT, "Device Manager", 0);
    ui->label(l1, PURR_UI_ROLE_TITLE);
    if (l2) {
        ui->label(l2, PURR_UI_ROLE_DIM);
    }
}

/* Starts a job. The first call may ask for permission: then the app waits and tries again when it is answered. */
NO_OPT static void begin_job(int job, const char *what, const char *a, const char *b, const char *busy_text)
{
    int rc = dv->start(what, a ? a : "", b ? b : "");
    if (rc == PURR_DEVICES_ASK) {
        put(s_retry_what, what, sizeof(s_retry_what));
        put(s_retry_a, a ? a : "", sizeof(s_retry_a));
        put(s_retry_b, b ? b : "", sizeof(s_retry_b));
        s_retry_job = job;
        show_wait("Waiting for permission...", "Answer the question on the screen.");
        return;
    }
    if (rc == PURR_DEVICES_DENIED) {
        fail("Not allowed. Managing devices needs an administrator and their OK.");
        return;
    }
    if (rc == PURR_DEVICES_BUSY) {
        fail("Still busy with the last request.");
        return;
    }
    if (rc != PURR_DEVICES_OK) {
        fail("Could not start that.");
        return;
    }
    s_job = job;
    status(busy_text, 0);
}

/* ---------------------------------------------------------------- pages */

NO_OPT static void show_home(void)
{
    begin_page(PG_HOME, "Device Manager", 0);
    char v[TXT];
    if (dv->info("versions", v, sizeof(v)) > 0) {
        ui->label(v, PURR_UI_ROLE_DIM);
    }
    h_home_row[0] = ui->row("Find hardware", "Look for attached devices");
    h_home_row[1] = ui->row("My devices", "Built in and found");
    h_home_row[2] = ui->row("Add by hand", "You know the wiring");
    h_home_row[3] = ui->row("Drivers", "Installed, and updates");
    h_home_row[4] = ui->row("System update", "Check, install, reinstall");
    char dm[4];
    h_home_row[5] = 0;
    if (dv->info("dev-mode", dm, sizeof(dm)) > 0 && dm[0] == '1') {
        h_home_row[5] = ui->row("Developer server", "Apps, drivers and updates from your computer");
    }
}

NO_OPT static void show_find(void)
{
    begin_page(PG_FIND, "Find hardware", 0);
    ui->label("Looks for hardware that is attached but not set up. It only reads: it never touches the flash, USB or boot pins.", PURR_UI_ROLE_DIM);
    h_find = action("Find hardware");
    char a[TXT], b[TXT];
    int n = 0;
    while (n < MAX_ROWS && dv->list("found", n, a, sizeof(a), b, sizeof(b)) == 1) {
        ui->row(a, b);
        n++;
    }
    if (n == 0) {
        ui->label("Nothing found yet.", PURR_UI_ROLE_DIM);
    }
    char c[8];
    h_setup = 0;
    if (dv->info("to-set-up", c, sizeof(c)) > 0 && to_int(c) > 0) {
        char t[TXT];
        put(t, "Set up ", sizeof(t));
        append_num(t, to_int(c), sizeof(t));
        append(t, to_int(c) == 1 ? " device" : " devices", sizeof(t));
        h_setup = action(t);
    }
}

NO_OPT static void show_devices(void)
{
    begin_page(PG_DEVICES, "My devices", 0);
    ui->label("Built in", PURR_UI_ROLE_TITLE);
    char a[TXT], b[TXT];
    for (int i = 0; i < 12 && dv->list("board", i, a, sizeof(a), b, sizeof(b)) == 1; i++) {
        ui->row(a, b);
    }
    ui->label("Found and added", PURR_UI_ROLE_TITLE);
    int n = 0;
    while (n < MAX_ROWS && dv->list("profile", n, a, sizeof(a), b, sizeof(b)) == 1) {
        s_row[s_nrow] = ui->row(a, b);
        s_row_kind[s_nrow] = 1;
        s_nrow++;
        n++;
    }
    if (n == 0) {
        ui->label("Nothing found or added yet.", PURR_UI_ROLE_DIM);
    } else {
        ui->label("Tap one to read it or remove it.", PURR_UI_ROLE_DIM);
    }
}

NO_OPT static void show_item(const char *name)
{
    put(s_item, name, sizeof(s_item));
    begin_page(PG_ITEM, name, 0);
    h_read = action("Take a reading");
    h_remove = action("Remove");
}

NO_OPT static void show_add(void)
{
    begin_page(PG_ADD, "Add by hand", 0);
    ui->label("For hardware you know the wiring of, or that sits on a bus Find hardware does not scan. Example: kbd-backlight, pins 18 and 8, address 0x55.", PURR_UI_ROLE_DIM);
    h_driver = ui->textarea(31, "Driver name", 0);
    h_sda = ui->textarea(3, "SDA pin", 0);
    h_scl = ui->textarea(3, "SCL pin", 0);
    h_addr = ui->textarea(5, "Address, like 0x55", 0);
    h_add = action("Add this device");
    if (h_driver > 0) {
        ui->focus(h_driver);
    }
}

NO_OPT static void show_drivers(void)
{
    begin_page(PG_DRIVERS, "Drivers", 0);
    char a[TXT], b[TXT];
    int n = 0;
    while (n < MAX_ROWS && dv->list("drivers", n, a, sizeof(a), b, sizeof(b)) == 1) {
        ui->row(a, b);
        n++;
    }
    if (n == 0) {
        ui->label("No downloaded drivers yet. The ones the system came with are part of the system.", PURR_UI_ROLE_DIM);
    }
    char mc[8];
    h_getdrv = 0;
    if (dv->info("missing", mc, sizeof(mc)) > 0 && to_int(mc) > 0) {
        char t[TXT];
        put(t, "Get ", sizeof(t));
        append_num(t, to_int(mc), sizeof(t));
        append(t, to_int(mc) == 1 ? " missing driver" : " missing drivers", sizeof(t));
        h_getdrv = action(t);
        ui->label("The built-in hardware below has no driver yet. They are downloaded from the official server, or from your developer server when one is in use.", PURR_UI_ROLE_DIM);
        for (int i = 0; i < 12 && dv->list("missing", i, a, sizeof(a), b, sizeof(b)) == 1; i++) {
            ui->row(a, b);
        }
    }
    h_drv = action("Update drivers");
    ui->label("Drivers update on their own, apart from the system. They must be signed; in developer mode with unsigned drivers allowed, they can be unsigned. Updated drivers start after a restart.", PURR_UI_ROLE_DIM);
}

NO_OPT static void source_text(char *out, int cap)
{
    char sv[TXT];
    char dm[4];
    if (s_dev_src && dv->info("devserver", sv, sizeof(sv)) > 0 && sv[0]) {
        put(out, "Source: ", cap);
        append(out, sv, cap);
    } else {
        s_dev_src = 0;
        put(out, "Source: the official server", cap);
    }
    (void)dm;
}

NO_OPT static void show_update(void)
{
    begin_page(PG_UPDATE, "System update", 0);
    char v[TXT];
    if (dv->info("versions", v, sizeof(v)) > 0) {
        ui->label(v, PURR_UI_ROLE_TEXT);
    }
    char dm[4], sv[TXT];
    h_src = 0;
    if (dv->info("dev-mode", dm, sizeof(dm)) > 0 && dm[0] == '1' && dv->info("devserver", sv, sizeof(sv)) > 0 && sv[0]) {
        char t[TXT];
        source_text(t, sizeof(t));
        h_src = ui->button(t);
    } else {
        s_dev_src = 0;
    }
    h_check = action("Check for update");
    h_install = 0;
    if (s_newer) {
        char c[4];
        h_install = action(dv->info("bootloader-update", c, sizeof(c)) > 0 && c[0] == '1' ? "Install update (includes the bootloader)" : "Install update");
    }
    ui->label("Reinstall", PURR_UI_ROLE_TITLE);
    ui->label("Puts the system back from the source above, kernel and recovery both, even if they look fine. Your files and accounts are kept.", PURR_UI_ROLE_DIM);
    h_reinstall = action("Reinstall");
}

NO_OPT static void show_server(void)
{
    begin_page(PG_SERVER, "Developer server", 0);
    ui->label("One server for apps, drivers, system updates and reinstall. It must be on this network.", PURR_UI_ROLE_DIM);
    char sv[TXT];
    if (dv->info("devserver", sv, sizeof(sv)) > 0 && sv[0]) {
        char t[TXT];
        put(t, "In use: ", sizeof(t));
        append(t, sv, sizeof(t));
        ui->label(t, PURR_UI_ROLE_TEXT);
    } else {
        ui->label("None chosen yet.", PURR_UI_ROLE_TEXT);
    }
    h_devfind = action("Find on this network");
    char a[TXT], b[TXT];
    for (int i = 0; i < 4 && dv->list("servers", i, a, sizeof(a), b, sizeof(b)) == 1; i++) {
        s_row[s_nrow] = ui->row(a, b);
        s_row_kind[s_nrow] = 2;
        s_nrow++;
    }
    h_srvaddr = ui->textarea(60, "Or type an address, like 192.168.1.20:47801", 0);
    h_devset = action("Use this address");
}

/* ---------------------------------------------------------------- what a finished job does */

NO_OPT static void on_job(int arg)
{
    char r[160];
    int n = dv->result(r, sizeof(r));
    if (n < 0) {
        r[0] = '\0';
    }
    int bad = arg == 1;
    int job = s_job;
    s_job = J_NONE;
    if (bad) {
        fail(r[0] ? r : "That did not work.");
        return;
    }
    status(r[0] ? r : "Done.", 0);
    switch (job) {
    case J_FIND:
    case J_SETUP:
        show_find();
        break;
    case J_DRV:
        show_drivers();
        break;
    case J_REMOVE:
        show_devices();
        break;
    case J_READ:
        if (s_cur_msg > 0) {
            ui->set_text(s_cur_msg, r);                 /* a reading belongs under its button */
        }
        break;
    case J_CHECK:
        s_newer = arg == 2;
        show_update();
        break;
    case J_UPDATE:
        status("Restarting into the updater...", 0);
        dv->restart();
        break;
    case J_DEVFIND:
    case J_DEVUSE:
    case J_DEVSET:
        show_server();
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- clicks */

NO_OPT static const char *src_arg(void)
{
    return s_dev_src ? "dev" : "official";
}

NO_OPT static void on_click(int h)
{
    mark_pressed(h);
    if (s_page == PG_HOME) {
        if (h == h_home_row[0]) {
            show_find();
        } else if (h == h_home_row[1]) {
            show_devices();
        } else if (h == h_home_row[2]) {
            show_add();
        } else if (h == h_home_row[3]) {
            show_drivers();
        } else if (h == h_home_row[4]) {
            s_newer = 0;
            show_update();
        } else if (h_home_row[5] > 0 && h == h_home_row[5]) {
            show_server();
        }
    } else if (s_page == PG_FIND) {
        if (h == h_find) {
            begin_job(J_FIND, "find", "", "", "Looking...");
        } else if (h_setup > 0 && h == h_setup) {
            begin_job(J_SETUP, "setup", "", "", "Setting up...");
        }
    } else if (s_page == PG_DEVICES) {
        for (int i = 0; i < s_nrow; i++) {
            if (s_row[i] == h && s_row_kind[i] == 1) {
                char a[TXT], b[TXT];
                int k = 0;
                for (int j = 0; j < s_nrow; j++) {
                    if (s_row[j] == h) {
                        k = j;
                    }
                }
                if (dv->list("profile", k, a, sizeof(a), b, sizeof(b)) == 1) {
                    show_item(a);
                }
                return;
            }
        }
    } else if (s_page == PG_ITEM) {
        if (h == h_read) {
            begin_job(J_READ, "read", s_item, "", "Reading...");
        } else if (h == h_remove) {
            if (!s_armed) {
                s_armed = 1;
                ui->set_text(s_cur_msg, "Tap again to remove it.");
            } else {
                begin_job(J_REMOVE, "remove", s_item, "", "Removing...");
            }
        }
    } else if (s_page == PG_ADD) {
        if (h == h_add) {
            char drv[40], sda[8], scl[8], addr[8], args[40];
            ui->get_text(h_driver, drv, sizeof(drv));
            ui->get_text(h_sda, sda, sizeof(sda));
            ui->get_text(h_scl, scl, sizeof(scl));
            ui->get_text(h_addr, addr, sizeof(addr));
            put(args, sda, sizeof(args));
            append(args, " ", sizeof(args));
            append(args, scl, sizeof(args));
            append(args, " ", sizeof(args));
            append(args, addr, sizeof(args));
            if (drv[0] == '\0') {
                fail("Give the driver a name first.");
                return;
            }
            begin_job(J_ADD, "add", drv, args, "Adding...");
        }
    } else if (s_page == PG_DRIVERS) {
        if (h == h_drv) {
            begin_job(J_DRV, "update-drivers", "", "", "Checking for driver updates...");
        } else if (h_getdrv > 0 && h == h_getdrv) {
            begin_job(J_DRV, "get-drivers", "", "", "Getting the drivers...");
        }
    } else if (s_page == PG_UPDATE) {
        if (h_src > 0 && h == h_src) {
            s_dev_src = !s_dev_src;
            s_newer = 0;
            show_update();
        } else if (h == h_check) {
            begin_job(J_CHECK, "check", src_arg(), "", "Checking...");
        } else if (h_install > 0 && h == h_install) {
            if (!s_armed) {
                s_armed = 1;
                ui->set_text(s_cur_msg, "Tap again to restart and update.");
            } else {
                begin_job(J_UPDATE, "update", src_arg(), "", "Asking the updater...");
            }
        } else if (h == h_reinstall) {
            if (!s_armed) {
                s_armed = 1;
                ui->set_text(s_cur_msg, "Tap again: reinstall (files are kept).");
            } else {
                begin_job(J_UPDATE, "reinstall", src_arg(), "", "Asking the updater...");
            }
        }
    } else if (s_page == PG_SERVER) {
        if (h == h_devfind) {
            begin_job(J_DEVFIND, "dev-find", "", "", "Looking for servers...");
        } else if (h == h_devset) {
            char addr[64];
            ui->get_text(h_srvaddr, addr, sizeof(addr));
            begin_job(J_DEVSET, "dev-set", addr, "", "Checking the address...");
        } else {
            for (int i = 0; i < s_nrow; i++) {
                if (s_row[i] == h && s_row_kind[i] == 2) {
                    char num[8];
                    num[0] = '\0';
                    append_num(num, i, sizeof(num));
                    begin_job(J_DEVUSE, "dev-use", num, "", "Using that server...");
                }
            }
        }
    }
}

NO_OPT static void go_back(void)
{
    switch (s_page) {
    case PG_HOME:
        app->close_self();
        break;
    case PG_ITEM:
        show_devices();
        break;
    default:
        show_home();                                 /* a page, or the permission wait */
        break;
    }
}

/* ---------------------------------------------------------------- the app */

static int on_create(const purr_app_env_t *env, const void *state, uint32_t len)
{
    (void)state;
    (void)len;
    if (env == NULL || env->catcall_get == NULL) {
        return -1;
    }
    ui = (const purr_cc_ui_t *)env->catcall_get("ui", 1);
    dv = (const purr_cc_devices_t *)env->catcall_get("devices", 1);
    app = (const purr_cc_app_t *)env->catcall_get("app", 1);
    if (ui == NULL || dv == NULL || app == NULL) {
        return -1;
    }
    char t[8];
    int rc = dv->info("dev-mode", t, sizeof(t));
    if (rc == PURR_DEVICES_ASK) {
        s_retry_what[0] = '\0';
        show_wait("Waiting for permission...", "Answer the question on the screen.");
    } else if (rc == PURR_DEVICES_DENIED) {
        show_wait("Device Manager is for administrators.", "Ask an administrator, or sign in as one.");
    } else {
        show_home();
    }
    return 0;
}

static void on_event(const purr_app_event_t *ev)
{
    if (ui == NULL) {
        return;
    }
    if (ev->type == PURR_APPEV_PERM) {
        if (s_page == PG_WAIT && ev->arg == 1) {
            if (s_retry_what[0]) {
                char what[16], a[72], b[72];
                put(what, s_retry_what, sizeof(what));
                put(a, s_retry_a, sizeof(a));
                put(b, s_retry_b, sizeof(b));
                s_retry_what[0] = '\0';
                show_home();
                begin_job(s_retry_job, what, a, b, "Working...");
            } else {
                show_home();
            }
        } else if (s_page == PG_WAIT) {
            show_wait("Permission was not given.", "Allow it in Settings > Apps.");
        }
        return;
    }
    if (ev->type != PURR_APPEV_UI || ev->name == NULL) {
        return;
    }
    if (same(ev->name, "click")) {
        on_click(ev->arg);
    } else if (same(ev->name, "job")) {
        on_job(ev->arg);
    } else if (same(ev->name, "back")) {
        go_back();
    }
}

static void on_destroy(int reason)
{
    (void)reason;
}

static const purr_app_table2_t s_table = {
    .base = {.abi_version = PURR_APP_ABI_VERSION_2, .on_create = on_create, .on_destroy = on_destroy},
    .on_event = on_event,
    .memory_needed = 24576, .kind = 2, .flags = 0,
};

__attribute__((used))
const purr_app_table_t *devices_entry(void)
{
    return &s_table.base;
}
