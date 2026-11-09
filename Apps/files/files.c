/*
 * files.c - Files, a sideloaded UI app: a small file manager. No libc: only the catcalls it is handed.
 *
 * Two places. "Home" is the signed-in user's folder (needs the shared-storage permission): browse, make a folder, rename, copy and paste,
 * delete. "System files" is the rest of the file system, read-only, and only offered to an administrator (system-files permission). The
 * first call into a place asks the user; this app waits for the answer (PURR_APPEV_PERM) and tries again.
 *
 * Back goes up a folder, then to the two places, then closes the app. A file shows as text (its first 2000 bytes; other bytes as dots).
 */
#include <stddef.h>
#include <stdint.h>

#include "purr_app_abi.h"
#include "purr_catcalls.h"

#define MAX_ENTRIES 24
#define NAME_LEN 32
#define PATH_LEN 120
#define VIEW_MAX 2000

#define NO_OPT __attribute__((noinline, optimize("no-tree-loop-distribute-patterns")))

static const purr_cc_ui_t *ui;
static const purr_cc_files_t *fl;
static const purr_cc_app_t *app;

enum { PG_PLACES, PG_DIR, PG_ITEM, PG_VIEW, PG_NAME, PG_WAIT };
static int s_page;
static int s_sys;                                   /* 1 = system files (read-only), 0 = home */
static int s_admin;
static int s_armed;                                 /* delete was tapped once */
static int s_name_kind;                             /* 0 new folder, 1 rename */
static int s_retry_page;                            /* what to redo when the permission answer arrives */

static char s_home[48];
static char s_cwd[PATH_LEN];
static char s_item[PATH_LEN];                       /* the file or folder on the item page */
static char s_clip[PATH_LEN];                       /* copied file, to paste */

static char s_name[MAX_ENTRIES][NAME_LEN];
static uint8_t s_dir[MAX_ENTRIES];
static uint32_t s_size[MAX_ENTRIES];
static int s_row[MAX_ENTRIES];
static int s_n, s_more;

static int h_home, h_sys, h_open, h_copy, h_rename, h_delete, h_paste, h_ok, h_ta;
static int s_pair_btn[6], s_pair_msg[6], s_npair;       /* each action button and the message line under it */
static int s_cur_msg;                                   /* the line under the button that was just pressed */
static char s_view[VIEW_MAX + 1];

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

NO_OPT static int cmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
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

/* dir + "/" + name into out ("/" + name for the root). */
NO_OPT static void join(char *out, const char *dir, const char *name, int cap)
{
    put(out, dir, cap);
    if (!same(dir, "/")) {
        append(out, "/", cap);
    }
    append(out, name, cap);
}

/* The folder above path ("/" for a top-level one). */
NO_OPT static void parent_of(char *out, const char *path, int cap)
{
    put(out, path, cap);
    int n = slen(out);
    while (n > 1 && out[n - 1] != '/') {
        n--;
    }
    if (n > 1) {
        n--;
    }
    out[n > 0 ? n : 1] = '\0';
    if (out[0] == '\0') {
        put(out, "/", cap);
    }
}

NO_OPT static const char *base_of(const char *path)
{
    int n = slen(path);
    while (n > 0 && path[n - 1] != '/') {
        n--;
    }
    return path + n;
}

NO_OPT static void fmt_size(char *out, uint32_t b)
{
    char tmp[12];
    int t = 0;
    uint32_t v = b;
    const char *unit = " bytes";
    if (b >= 1024 * 1024) {
        v = b / (1024 * 1024);
        unit = " MB";
    } else if (b >= 1024) {
        v = b / 1024;
        unit = " KB";
    }
    if (v == 0) {
        tmp[t++] = '0';
    }
    while (v > 0 && t < 10) {
        tmp[t++] = (char)('0' + v % 10);
        v /= 10;
    }
    int o = 0;
    while (t > 0) {
        out[o++] = tmp[--t];
    }
    out[o] = '\0';
    append(out, unit, 24);
}

/* ---------------------------------------------------------------- messages */

/* The status strip at the top of the screen, 5 seconds (ui version 2). */
NO_OPT static void status(const char *t, int bad)
{
    if (ui->version >= 2 && ui->status != NULL) {
        ui->status(t, bad);
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
    if (s_npair < 6) {
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

/* ---------------------------------------------------------------- the directory */

NO_OPT static void list_cb(const char *name, int is_dir, uint32_t size, void *ctx)
{
    (void)ctx;
    if (s_n >= MAX_ENTRIES) {
        s_more++;
        return;
    }
    put(s_name[s_n], name, NAME_LEN);
    s_dir[s_n] = (uint8_t)(is_dir != 0);
    s_size[s_n] = size;
    s_n++;
}

NO_OPT static void sort_entries(void)
{
    for (int i = 1; i < s_n; i++) {
        char nm[NAME_LEN];
        put(nm, s_name[i], NAME_LEN);
        uint8_t d = s_dir[i];
        uint32_t sz = s_size[i];
        int k = i - 1;
        while (k >= 0 && (s_dir[k] < d || (s_dir[k] == d && cmp(s_name[k], nm) > 0))) {
            put(s_name[k + 1], s_name[k], NAME_LEN);
            s_dir[k + 1] = s_dir[k];
            s_size[k + 1] = s_size[k];
            k--;
        }
        put(s_name[k + 1], nm, NAME_LEN);
        s_dir[k + 1] = d;
        s_size[k + 1] = sz;
    }
}

/* ---------------------------------------------------------------- pages */

static void show_places(void);
static void show_dir(const char *path);
static void show_item(const char *path);

NO_OPT static void show_message(const char *title, const char *line1, const char *line2)
{
    ui->clear();
    ui->win_back(1);
    ui->win_action(PURR_UI_ACT_NONE);
    ui->win_title(title);
    ui->label(line1, PURR_UI_ROLE_TITLE);
    if (line2 != NULL) {
        ui->label(line2, PURR_UI_ROLE_DIM);
    }
}

/* A call said PURR_FILES_ASK / DENIED / BAD. Returns 1 if it was handled here (a page was drawn). */
NO_OPT static int explain(int rc, int retry_page)
{
    if (rc == PURR_FILES_ASK) {
        s_page = PG_WAIT;
        s_retry_page = retry_page;
        show_message("Files", "Waiting for permission...", "Answer the question on the screen.");
        return 1;
    }
    if (rc == PURR_FILES_DENIED) {
        s_page = PG_WAIT;
        s_retry_page = -1;
        show_message("Files", "Permission was not given.", s_sys ? "System files need an administrator and their OK." : "Allow it in Settings > Apps.");
        return 1;
    }
    return 0;
}

NO_OPT static void show_places(void)
{
    s_page = PG_PLACES;
    s_sys = 0;
    s_armed = 0;
    ui->clear();
    ui->win_back(1);
    ui->win_action(PURR_UI_ACT_NONE);
    ui->win_title("Files");
    h_home = ui->row("Home", "Your files");
    h_sys = s_admin ? ui->row("System files", "Read only") : 0;
}

NO_OPT static void show_dir(const char *path)
{
    s_n = 0;
    s_more = 0;
    int rc = fl->list(path, list_cb, NULL);
    if (rc != PURR_FILES_OK) {
        if (!explain(rc, PG_DIR)) {
            s_page = PG_WAIT;
            s_retry_page = -1;
            show_message("Files", "Could not open that folder.", NULL);
        }
        return;
    }
    put(s_cwd, path, PATH_LEN);
    sort_entries();
    s_page = PG_DIR;
    s_armed = 0;
    s_npair = 0;
    s_cur_msg = 0;
    ui->clear();
    ui->win_back(1);
    ui->win_action(s_sys ? PURR_UI_ACT_NONE : PURR_UI_ACT_PLUS);
    ui->win_title(same(path, "/") ? "System files" : base_of(path));
    h_paste = 0;
    if (!s_sys && s_clip[0]) {
        h_paste = action("Paste here");
    }
    if (s_n == 0) {
        ui->label("This folder is empty.", PURR_UI_ROLE_DIM);
    }
    for (int i = 0; i < s_n; i++) {
        char sub[24];
        if (s_dir[i]) {
            put(sub, "Folder", 24);
        } else {
            fmt_size(sub, s_size[i]);
        }
        s_row[i] = ui->row(s_name[i], sub);
    }
    if (s_more > 0) {
        ui->label("More files are not shown.", PURR_UI_ROLE_DIM);
    }
}

NO_OPT static void show_item(const char *path)
{
    uint32_t size = 0;
    int is_dir = 0;
    fl->stat(path, &size, &is_dir);
    put(s_item, path, PATH_LEN);
    s_page = PG_ITEM;
    s_armed = 0;
    s_npair = 0;
    s_cur_msg = 0;
    ui->clear();
    ui->win_back(1);
    ui->win_action(PURR_UI_ACT_NONE);
    ui->win_title(base_of(path));
    ui->label(path, PURR_UI_ROLE_DIM);
    char info[32];
    if (is_dir) {
        put(info, "Folder", 32);
    } else {
        fmt_size(info, size);
    }
    ui->label(info, PURR_UI_ROLE_TEXT);
    ui->spacer(6);
    h_open = ui->button(is_dir ? "Open" : "View");
    h_copy = h_rename = h_delete = 0;
    if (!s_sys) {
        if (!is_dir) {
            h_copy = action("Copy");
        }
        h_rename = action("Rename");
        h_delete = action("Delete");
    }
}

NO_OPT static void show_view(const char *path)
{
    int n = fl->read(path, 0, s_view, VIEW_MAX);
    if (n < 0) {
        if (!explain(n, PG_VIEW)) {
            s_page = PG_WAIT;
            s_retry_page = -1;
            show_message("Files", "Could not read that file.", NULL);
        }
        return;
    }
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s_view[i];
        if (c != '\n' && c != '\t' && (c < 32 || c > 126)) {
            s_view[i] = '.';
        }
    }
    s_view[n] = '\0';
    s_page = PG_VIEW;
    ui->clear();
    ui->win_back(1);
    ui->win_action(PURR_UI_ACT_NONE);
    ui->win_title(base_of(path));
    h_ta = ui->textarea(VIEW_MAX, "", 1);
    if (h_ta > 0) {
        ui->set_text(h_ta, s_view);
    }
}

NO_OPT static void show_name(int kind)
{
    s_name_kind = kind;
    s_page = PG_NAME;
    s_npair = 0;
    s_cur_msg = 0;
    ui->clear();
    ui->win_back(1);
    ui->win_action(PURR_UI_ACT_NONE);
    ui->win_title(kind ? "Rename" : "New folder");
    h_ta = ui->textarea(NAME_LEN - 1, kind ? "New name" : "Folder name", 0);
    if (h_ta > 0) {
        if (kind) {
            ui->set_text(h_ta, base_of(s_item));
        }
        ui->focus(h_ta);
    }
    h_ok = action("OK");
}

/* ---------------------------------------------------------------- actions */

/* A name with no slash and not empty or "." / "..": the only names this app lets a person type. */
NO_OPT static int name_ok(const char *n)
{
    if (n[0] == '\0' || same(n, ".") || same(n, "..")) {
        return 0;
    }
    for (int i = 0; n[i]; i++) {
        if (n[i] == '/' || n[i] == '\\') {
            return 0;
        }
    }
    return 1;
}

NO_OPT static void do_name_ok(void)
{
    char typed[NAME_LEN];
    ui->get_text(h_ta, typed, sizeof(typed));
    if (!name_ok(typed)) {
        fail("Not a good name: no slashes, and not empty.");
        return;
    }
    char path[PATH_LEN];
    int rc;
    if (s_name_kind == 0) {
        join(path, s_cwd, typed, PATH_LEN);
        rc = fl->mkdir(path);
    } else {
        char dir[PATH_LEN];
        parent_of(dir, s_item, PATH_LEN);
        join(path, dir, typed, PATH_LEN);
        rc = fl->rename(s_item, path);
    }
    if (rc != PURR_FILES_OK) {
        fail(rc == PURR_FILES_BAD ? "That did not work (is the name taken?)" : "Not allowed");
        return;
    }
    status(s_name_kind ? "Renamed" : "Folder made", 0);
    show_dir(s_cwd);
}

NO_OPT static void do_paste(void)
{
    char dst[PATH_LEN];
    join(dst, s_cwd, base_of(s_clip), PATH_LEN);
    uint32_t sz;
    int d;
    if (fl->stat(dst, &sz, &d) == PURR_FILES_OK) {          /* never overwrite: make the user rename or delete first */
        fail("That name is already here. Rename or delete it first.");
        return;
    }
    int rc = fl->copy(s_clip, dst);
    if (rc != PURR_FILES_OK) {
        fail("Could not copy it (too big, or no room).");
        return;
    }
    status("Pasted", 0);
    show_dir(s_cwd);
}

NO_OPT static void go_back(void)
{
    char up[PATH_LEN];
    switch (s_page) {
    case PG_PLACES:
        app->close_self();
        break;
    case PG_DIR:
        if (same(s_cwd, s_sys ? "/" : s_home)) {
            show_places();
        } else {
            parent_of(up, s_cwd, PATH_LEN);
            show_dir(up);
        }
        break;
    case PG_ITEM:
    case PG_NAME:
        show_dir(s_cwd);
        break;
    case PG_VIEW:
        show_item(s_item);
        break;
    default:
        show_places();                                          /* waiting or refused */
        break;
    }
}

NO_OPT static void on_click(int h)
{
    mark_pressed(h);
    if (s_page == PG_PLACES) {
        if (h == h_home) {
            s_sys = 0;
            show_dir(s_home);
        } else if (s_admin && h == h_sys) {
            s_sys = 1;
            show_dir("/");
        }
    } else if (s_page == PG_DIR) {
        if (h_paste > 0 && h == h_paste) {
            do_paste();
            return;
        }
        for (int i = 0; i < s_n; i++) {
            if (s_row[i] == h) {
                char p[PATH_LEN];
                join(p, s_cwd, s_name[i], PATH_LEN);
                show_item(p);
                return;
            }
        }
    } else if (s_page == PG_ITEM) {
        if (h == h_open) {
            uint32_t sz;
            int d = 0;
            fl->stat(s_item, &sz, &d);
            if (d) {
                show_dir(s_item);
            } else {
                show_view(s_item);
            }
        } else if (h_copy > 0 && h == h_copy) {
            put(s_clip, s_item, PATH_LEN);
            status("Copied. Open a folder to paste it.", 0);
        } else if (h_rename > 0 && h == h_rename) {
            show_name(1);
        } else if (h_delete > 0 && h == h_delete) {
            if (!s_armed) {
                s_armed = 1;
                status("Delete? Tap again to be sure.", 0);
                if (s_cur_msg > 0) {
                    ui->set_text(s_cur_msg, "Tap Delete again to delete it.");
                }
            } else {
                int rc = fl->remove(s_item);
                if (rc != PURR_FILES_OK) {
                    s_armed = 0;
                    fail(rc == PURR_FILES_BAD ? "Folder not empty, or in use." : "Not allowed.");
                } else {
                    status("Deleted", 0);
                    show_dir(s_cwd);
                }
            }
        }
    } else if (s_page == PG_NAME && h == h_ok) {
        do_name_ok();
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
    fl = (const purr_cc_files_t *)env->catcall_get("files", 1);
    app = (const purr_cc_app_t *)env->catcall_get("app", 1);
    if (ui == NULL || fl == NULL || app == NULL || fl->home(s_home, sizeof(s_home)) < 0) {
        return -1;
    }
    s_admin = fl->is_admin();
    s_clip[0] = '\0';
    show_places();
    return 0;
}

static void on_event(const purr_app_event_t *ev)
{
    if (ui == NULL) {
        return;
    }
    if (ev->type == PURR_APPEV_PERM) {
        if (s_page == PG_WAIT && ev->arg == 1 && s_retry_page >= 0) {
            if (s_retry_page == PG_VIEW) {
                show_view(s_item);
            } else {
                show_dir(s_sys ? "/" : s_home);
            }
        } else if (s_page == PG_WAIT) {
            s_retry_page = -1;
            show_message("Files", "Permission was not given.", "Allow it in Settings > Apps.");
        }
        return;
    }
    if (ev->type != PURR_APPEV_UI || ev->name == NULL) {
        return;
    }
    if (same(ev->name, "click")) {
        on_click(ev->arg);
    } else if (same(ev->name, "action") && s_page == PG_DIR && !s_sys) {
        show_name(0);
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
const purr_app_table_t *files_entry(void)
{
    return &s_table.base;
}
