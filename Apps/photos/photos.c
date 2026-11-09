/*
 * photos.c - Photos, a sideloaded UI app: browse the pictures in your home folder and look at them. No libc: only the catcalls it is handed.
 *
 * It opens in /home/<you>/Pictures when that folder exists, otherwise in your home folder. The list shows folders and picture files (JPEG, PNG, BMP); a
 * tap on a picture opens it full width with Prev, Play, Next and Delete under it. Prev and Next go round the pictures of that folder; Play turns the pictures
 * over by themselves every four seconds until Stop (or back); Delete asks once more ("Sure?") before it removes the file. The OS decodes and scales the picture (the
 * `ui` catcall's image widget, version 3), so a picture over about a megapixel or 2 MB is refused with a message, not shown. Reading your files needs the
 * shared-storage permission: the first call asks, and this app waits for the answer (PURR_APPEV_PERM).
 *
 * Back goes from a picture to its folder, up the folders to your home folder, then closes the app.
 */
#include <stddef.h>
#include <stdint.h>

#include "purr_app_abi.h"
#include "purr_catcalls.h"

#define MAX_ENTRIES 32
#define NAME_LEN 32
#define PATH_LEN 120
#define IMG_HEIGHT 128

#define NO_OPT __attribute__((noinline, optimize("no-tree-loop-distribute-patterns")))

static const purr_cc_ui_t *ui;
static const purr_cc_files_t *fl;
static const purr_cc_app_t *app;
static const purr_cc_sched_t *sc;

enum { PG_LIST, PG_VIEW, PG_WAIT };
static int s_page;
static int s_retry;                                 /* what to redo when the permission answer arrives: 1 = the start, 2 = the picture on show */

static char s_home[48];
static char s_cwd[PATH_LEN];

static char s_name[MAX_ENTRIES][NAME_LEN];
static uint8_t s_dir[MAX_ENTRIES];
static uint32_t s_size[MAX_ENTRIES];
static int s_row[MAX_ENTRIES];
static int s_n, s_more;
static int s_cur;                                   /* the picture on show: an index into s_name */
static int h_prev, h_play, h_next, h_del;
static int s_show;                                  /* 1 while the slideshow runs */
static int s_timer;                                 /* its timer id */
static int s_armed;                                 /* Delete was tapped once */
#define SHOW_MS 4000

/* ---------------------------------------------------------------- tiny helpers (no libc) */

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

NO_OPT static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Case-insensitive order of two names. */
NO_OPT static int cmp(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return (int)(unsigned char)lower(*a) - (int)(unsigned char)lower(*b);
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

NO_OPT static void append_num(char *dst, uint32_t v, int cap)
{
    char tmp[11];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0 && n < 10);
    char out[11];
    int k = 0;
    while (n > 0) {
        out[k++] = tmp[--n];
    }
    out[k] = '\0';
    append(dst, out, cap);
}

/* dir + "/" + name into out. */
NO_OPT static void join(char *out, const char *dir, const char *name, int cap)
{
    put(out, dir, cap);
    if (!same(dir, "/")) {
        append(out, "/", cap);
    }
    append(out, name, cap);
}

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
    out[n > 0 ? n : 0] = '\0';
    if (out[0] == '\0') {
        put(out, "/", cap);
    }
}

NO_OPT static const char *base_of(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' && p[1] != '\0') {
            b = p + 1;
        }
    }
    return b;
}

NO_OPT static void fmt_size(char *out, uint32_t b)
{
    out[0] = '\0';
    if (b < 1024) {
        append_num(out, b, 24);
        append(out, " B", 24);
    } else if (b < 1024u * 1024u) {
        append_num(out, b / 1024, 24);
        append(out, " KB", 24);
    } else {
        append_num(out, b / (1024u * 1024u), 24);
        append(out, ".", 24);
        append_num(out, (b % (1024u * 1024u)) * 10 / (1024u * 1024u), 24);
        append(out, " MB", 24);
    }
}

/* 1 if the name ends in .jpg, .jpeg, .png or .bmp (any case). */
NO_OPT static int is_picture(const char *name)
{
    int dot = -1;
    for (int i = 0; name[i]; i++) {
        if (name[i] == '.') {
            dot = i;
        }
    }
    if (dot < 0) {
        return 0;
    }
    const char *e = name + dot + 1;
    char t[5];
    int n = 0;
    while (e[n] && n < 4) {
        t[n] = lower(e[n]);
        n++;
    }
    t[n] = '\0';
    if (e[n] != '\0') {
        return 0;                                   /* an extension longer than four letters */
    }
    return same(t, "jpg") || same(t, "jpeg") || same(t, "png") || same(t, "bmp");
}

/* ---------------------------------------------------------------- the folder's entries */

NO_OPT static void list_cb(const char *name, int is_dir, uint32_t size, void *ctx)
{
    (void)ctx;
    if (!is_dir && !is_picture(name)) {
        return;
    }
    if (slen(name) >= NAME_LEN || s_n >= MAX_ENTRIES) {
        s_more++;                                   /* a name too long to open, or no room for it */
        return;
    }
    put(s_name[s_n], name, NAME_LEN);
    s_dir[s_n] = is_dir ? 1 : 0;
    s_size[s_n] = size;
    s_n++;
}

NO_OPT static void swap_entries(int a, int b)
{
    char tmp[NAME_LEN];
    put(tmp, s_name[a], NAME_LEN);
    put(s_name[a], s_name[b], NAME_LEN);
    put(s_name[b], tmp, NAME_LEN);
    uint8_t d = s_dir[a];
    s_dir[a] = s_dir[b];
    s_dir[b] = d;
    uint32_t z = s_size[a];
    s_size[a] = s_size[b];
    s_size[b] = z;
}

/* Folders first, then by name. */
NO_OPT static void sort_entries(void)
{
    for (int i = 1; i < s_n; i++) {
        for (int k = i; k > 0; k--) {
            int before = (s_dir[k] > s_dir[k - 1]) || (s_dir[k] == s_dir[k - 1] && cmp(s_name[k], s_name[k - 1]) < 0);
            if (!before) {
                break;
            }
            swap_entries(k, k - 1);
        }
    }
}

/* The next (step 1) or previous (step -1) picture after index `from`, going round; -1 if there is none. */
NO_OPT static int step_picture(int from, int step)
{
    for (int i = 1; i <= s_n; i++) {
        int k = ((from + step * i) % s_n + s_n) % s_n;
        if (!s_dir[k]) {
            return k;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------- the slideshow */

NO_OPT static void stop_show(void)
{
    if (s_timer > 0 && sc != NULL) {
        sc->timer_cancel(s_timer);
    }
    s_timer = 0;
    s_show = 0;
}

NO_OPT static void start_show(void)
{
    if (sc == NULL || s_show) {
        return;
    }
    s_timer = sc->timer_start(SHOW_MS, 1);
    s_show = s_timer > 0;
}

/* ---------------------------------------------------------------- pages */

static void show_list(const char *path);
static void show_view(int idx);
static void start(void);

NO_OPT static void show_message(const char *line1, const char *line2)
{
    ui->clear();
    ui->win_back(1);
    ui->win_action(PURR_UI_ACT_NONE);
    ui->win_title("Photos");
    ui->label(line1, PURR_UI_ROLE_TITLE);
    if (line2 != NULL) {
        ui->label(line2, PURR_UI_ROLE_DIM);
    }
}

/* A files call said PURR_FILES_ASK / DENIED. Returns 1 if it was handled here (a page was drawn). */
NO_OPT static int explain(int rc, int retry)
{
    if (rc == PURR_FILES_ASK) {
        s_page = PG_WAIT;
        s_retry = retry;
        show_message("Waiting for permission...", "Answer the question on the screen.");
        return 1;
    }
    if (rc == PURR_FILES_DENIED) {
        s_page = PG_WAIT;
        s_retry = 0;
        show_message("Permission was not given.", "Allow it in Settings > Apps.");
        return 1;
    }
    return 0;
}

NO_OPT static void show_list(const char *path)
{
    s_n = 0;
    s_more = 0;
    int rc = fl->list(path, list_cb, NULL);
    if (rc != PURR_FILES_OK) {
        if (!explain(rc, 1)) {
            s_page = PG_WAIT;
            s_retry = 0;
            show_message("Could not open that folder.", NULL);
        }
        return;
    }
    put(s_cwd, path, PATH_LEN);
    sort_entries();
    s_page = PG_LIST;
    ui->clear();
    ui->win_back(1);
    ui->win_action(PURR_UI_ACT_NONE);
    ui->win_title(same(path, s_home) ? "Photos" : base_of(path));
    if (s_n == 0) {
        ui->label("No pictures here.", PURR_UI_ROLE_DIM);
        ui->label("JPEG, PNG and BMP files show up.", PURR_UI_ROLE_DIM);
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

NO_OPT static void show_view(int idx)
{
    char path[PATH_LEN];
    s_cur = idx;
    s_page = PG_VIEW;
    s_armed = 0;
    join(path, s_cwd, s_name[idx], PATH_LEN);
    ui->clear();
    ui->win_back(1);
    ui->win_action(PURR_UI_ACT_NONE);
    ui->win_title("Photos");
    int img = ui->image(IMG_HEIGHT);
    int rc = img > 0 ? ui->set_image(img, path) : -1;

    int total = 0, pos = 0;
    for (int i = 0; i < s_n; i++) {
        if (!s_dir[i]) {
            total++;
            if (i == idx) {
                pos = total;
            }
        }
    }
    char line[96], sz[24];
    line[0] = '\0';
    append_num(line, (uint32_t)pos, 96);
    append(line, " of ", 96);
    append_num(line, (uint32_t)total, 96);
    append(line, "   ", 96);
    append(line, s_name[idx], 96);
    append(line, "   ", 96);
    fmt_size(sz, s_size[idx]);
    append(line, sz, 96);
    ui->label(line, PURR_UI_ROLE_DIM);

    if (rc == PURR_FILES_ASK || rc == PURR_FILES_DENIED) {
        explain(rc, 2);
        return;
    }
    if (rc == PURR_UI_IMG_TOO_BIG) {
        ui->label("Too big to show (over about a megapixel).", PURR_UI_ROLE_ACCENT);
    } else if (rc == PURR_UI_IMG_UNSUPPORTED) {
        ui->label("This kind of picture cannot be shown.", PURR_UI_ROLE_ACCENT);
    } else if (rc != 0) {
        ui->label("Could not open the picture.", PURR_UI_ROLE_ACCENT);
    }
    h_prev = h_play = h_next = h_del = 0;
    if (total > 1) {
        int g = ui->grid(4, 36);
        h_prev = ui->grid_button(g, "Prev");
        h_play = ui->grid_button(g, s_show ? "Stop" : "Play");
        h_next = ui->grid_button(g, "Next");
        h_del = ui->grid_button(g, "Delete");
    } else {
        int g = ui->grid(1, 36);
        h_del = ui->grid_button(g, "Delete");
    }
}

NO_OPT static void start(void)
{
    char p[PATH_LEN];
    uint32_t sz = 0;
    int isdir = 0;
    join(p, s_home, "Pictures", PATH_LEN);
    int rc = fl->stat(p, &sz, &isdir);
    if (rc == PURR_FILES_OK && isdir) {
        show_list(p);
    } else if (!explain(rc, 1)) {
        show_list(s_home);
    }
}

NO_OPT static void go_back(void)
{
    char up[PATH_LEN];
    if (s_page == PG_VIEW) {
        stop_show();
        show_list(s_cwd);
    } else if (s_page == PG_LIST && !same(s_cwd, s_home)) {
        parent_of(up, s_cwd, PATH_LEN);
        show_list(up);
    } else {
        app->close_self();
    }
}

/* Removes the picture on show, then shows the one that takes its place (or the folder, when that was the last picture). */
NO_OPT static void do_delete(void)
{
    char path[PATH_LEN];
    stop_show();
    join(path, s_cwd, s_name[s_cur], PATH_LEN);
    if (fl->remove(path) != PURR_FILES_OK) {
        s_armed = 0;
        ui->set_text(h_del, "Delete");
        ui->status("Could not delete it", 1);
        return;
    }
    for (int i = s_cur; i < s_n - 1; i++) {
        put(s_name[i], s_name[i + 1], NAME_LEN);
        s_dir[i] = s_dir[i + 1];
        s_size[i] = s_size[i + 1];
    }
    s_n--;
    ui->status("Deleted", 0);
    int k = -1;
    if (s_n > 0) {
        int at = s_cur < s_n ? s_cur : s_n - 1;
        k = s_dir[at] ? step_picture(at, -1) : at;
    }
    if (k >= 0 && !s_dir[k]) {
        show_view(k);
    } else {
        show_list(s_cwd);
    }
}

NO_OPT static void on_click(int h)
{
    if (s_page == PG_LIST) {
        for (int i = 0; i < s_n; i++) {
            if (s_row[i] == h) {
                if (s_dir[i]) {
                    char sub[PATH_LEN];
                    join(sub, s_cwd, s_name[i], PATH_LEN);
                    show_list(sub);
                } else {
                    show_view(i);
                }
                return;
            }
        }
    } else if (s_page == PG_VIEW) {
        if (h_next > 0 && h == h_next) {
            stop_show();
            int k = step_picture(s_cur, 1);
            if (k >= 0) {
                show_view(k);
            }
        } else if (h_prev > 0 && h == h_prev) {
            stop_show();
            int k = step_picture(s_cur, -1);
            if (k >= 0) {
                show_view(k);
            }
        } else if (h_play > 0 && h == h_play) {
            if (s_show) {
                stop_show();
            } else {
                start_show();
            }
            ui->set_text(h_play, s_show ? "Stop" : "Play");
            if (s_armed) {
                s_armed = 0;
                ui->set_text(h_del, "Delete");
            }
        } else if (h_del > 0 && h == h_del) {
            if (!s_armed) {
                s_armed = 1;
                ui->set_text(h_del, "Sure?");
            } else {
                do_delete();
            }
        }
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
    ui = (const purr_cc_ui_t *)env->catcall_get("ui", 3);
    fl = (const purr_cc_files_t *)env->catcall_get("files", 1);
    app = (const purr_cc_app_t *)env->catcall_get("app", 1);
    sc = (const purr_cc_sched_t *)env->catcall_get("sched", 1);          /* optional: without it there is no slideshow */
    if (fl == NULL || app == NULL) {
        return -1;
    }
    if (ui == NULL) {                               /* an OS older than 1.0.23 has no picture widget: say so (the version 1 table has the label) */
        ui = (const purr_cc_ui_t *)env->catcall_get("ui", 1);
        if (ui != NULL) {
            ui->win_title("Photos");
            ui->label("Photos needs OS 1.0.23 or newer.", PURR_UI_ROLE_TITLE);
        }
        return ui != NULL ? 0 : -1;
    }
    if (fl->home(s_home, sizeof(s_home)) < 0) {
        return -1;
    }
    start();
    return 0;
}

static void on_event(const purr_app_event_t *ev)
{
    if (ui == NULL || fl == NULL || s_home[0] == '\0') {
        return;                                     /* the "needs a newer OS" page has nothing to do */
    }
    if (ev->type == PURR_APPEV_TIMER) {
        if (s_show && ev->arg == s_timer && s_page == PG_VIEW) {
            int k = step_picture(s_cur, 1);
            if (k >= 0) {
                show_view(k);
            }
        }
        return;
    }
    if (ev->type == PURR_APPEV_PERM) {
        if (s_page == PG_WAIT && ev->arg == 1 && s_retry == 1) {
            start();
        } else if (s_page == PG_WAIT && ev->arg == 1 && s_retry == 2) {
            show_view(s_cur);
        } else if (s_page == PG_WAIT) {
            s_retry = 0;
            show_message("Permission was not given.", "Allow it in Settings > Apps.");
        }
        return;
    }
    if (ev->type != PURR_APPEV_UI || ev->name == NULL) {
        return;
    }
    if (same(ev->name, "click")) {
        on_click(ev->arg);
    } else if (same(ev->name, "back")) {
        go_back();
    }
}

static void on_destroy(int reason)
{
    (void)reason;
    stop_show();
}

static const purr_app_table2_t s_table = {
    .base = {.abi_version = PURR_APP_ABI_VERSION_2, .on_create = on_create, .on_destroy = on_destroy},
    .on_event = on_event,
    .memory_needed = 20480, .kind = 2, .flags = 0,
};

__attribute__((used))
const purr_app_table_t *photos_entry(void)
{
    return &s_table.base;
}
