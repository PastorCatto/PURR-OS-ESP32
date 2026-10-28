/*
 * notes.c - Notes, a sideloaded UI app (plan9.md). No libc: only the catcalls it is handed.
 *
 * A list of notes (newest first) and an editor. A note is a file in the app's own data folder
 * (NNNN.txt, through the `storage` catcall); its first line is its title. The window, its colours and its
 * widgets come from the `ui` catcall, so the app carries no styling and follows the system theme.
 *
 * Back from the list closes the app (the OS does that); back from the editor saves and returns to the list.
 */
#include <stddef.h>
#include <stdint.h>

#include "purr_app_abi.h"
#include "purr_catcalls.h"

#define MAX_NOTES 12
#define NOTE_MAX 2000
#define HEAD_READ 120

#define NO_OPT __attribute__((noinline, optimize("no-tree-loop-distribute-patterns")))

static const purr_cc_ui_t *ui;
static const purr_cc_storage_t *st;
static const purr_cc_console_t *con;

enum { PAGE_LIST, PAGE_EDIT };
static int s_page;
static int s_armed;                         /* the delete action was tapped once */

static char s_file[MAX_NOTES][16];
static char s_title[MAX_NOTES][32];
static char s_sub[MAX_NOTES][40];
static int s_row[MAX_NOTES];                /* the row widget's handle */
static int s_n;

static char s_cur[16];                      /* the file being edited, "" for a new note */
static int s_ta;                            /* the editor's text area */
static char s_buf[NOTE_MAX + 1];

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

NO_OPT static int ends_txt(const char *s)
{
    int n = slen(s);
    return n >= 5 && s[n - 4] == '.' && s[n - 3] == 't' && s[n - 2] == 'x' && s[n - 1] == 't';
}

NO_OPT static int number_of(const char *file)
{
    int v = 0;
    for (int i = 0; file[i] >= '0' && file[i] <= '9'; i++) {
        v = v * 10 + (file[i] - '0');
    }
    return v;
}

NO_OPT static void name_for(char *out, int n)
{
    out[0] = (char)('0' + (n / 1000) % 10);
    out[1] = (char)('0' + (n / 100) % 10);
    out[2] = (char)('0' + (n / 10) % 10);
    out[3] = (char)('0' + n % 10);
    out[4] = '.';
    out[5] = 't';
    out[6] = 'x';
    out[7] = 't';
    out[8] = '\0';
}

/* The first and the second non-empty line of text. */
NO_OPT static void lines_of(const char *t, int len, char *first, int fcap, char *second, int scap)
{
    first[0] = second[0] = '\0';
    int which = 0, i = 0;
    while (i < len && which < 2) {
        while (i < len && (t[i] == '\n' || t[i] == '\r' || t[i] == ' ')) {
            i++;
        }
        int start = i;
        while (i < len && t[i] != '\n' && t[i] != '\r') {
            i++;
        }
        if (i > start) {
            char *dst = which == 0 ? first : second;
            int cap = which == 0 ? fcap : scap;
            int n = i - start < cap - 1 ? i - start : cap - 1;
            for (int k = 0; k < n; k++) {
                dst[k] = t[start + k];
            }
            dst[n] = '\0';
            which++;
        }
    }
}

/* ---------------------------------------------------------------- storage */

static void list_cb(const char *name, int is_dir, uint32_t size, void *ctx)
{
    (void)size;
    (void)ctx;
    if (!is_dir && ends_txt(name) && slen(name) < 16 && s_n < MAX_NOTES) {
        put(s_file[s_n++], name, 16);
    }
}

/* Reads up to cap-1 bytes of a note into out; returns the length. */
static int read_note(const char *file, char *out, int cap)
{
    int h = st->open(file, PURR_CC_O_READ);
    if (h < 0) {
        out[0] = '\0';
        return 0;
    }
    int n = st->read(h, out, (size_t)(cap - 1));
    st->close(h);
    if (n < 0) {
        n = 0;
    }
    out[n] = '\0';
    return n;
}

NO_OPT static void load_list(void)
{
    s_n = 0;
    st->list(".", list_cb, NULL);
    for (int i = 1; i < s_n; i++) {                     /* newest (highest number) first */
        char t[16];
        put(t, s_file[i], 16);
        int k = i - 1;
        while (k >= 0 && cmp(s_file[k], t) < 0) {
            put(s_file[k + 1], s_file[k], 16);
            k--;
        }
        put(s_file[k + 1], t, 16);
    }
    for (int i = 0; i < s_n; i++) {
        char head[HEAD_READ];
        int n = read_note(s_file[i], head, HEAD_READ);
        lines_of(head, n, s_title[i], 32, s_sub[i], 40);
        if (s_title[i][0] == '\0') {
            put(s_title[i], "(empty)", 32);
        }
    }
}

static int next_number(void)
{
    int top = 0;
    for (int i = 0; i < s_n; i++) {
        int v = number_of(s_file[i]);
        if (v > top) {
            top = v;
        }
    }
    return top + 1;
}

/* ---------------------------------------------------------------- pages */

static void show_list(void)
{
    s_page = PAGE_LIST;
    s_armed = 0;
    ui->clear();
    ui->win_back(0);                                    /* back from the list closes the app */
    ui->win_title("Notes");
    ui->win_action(PURR_UI_ACT_PLUS);
    load_list();
    if (s_n == 0) {
        ui->label("No notes yet.", PURR_UI_ROLE_TITLE);
        ui->label("Tap + to write one.", PURR_UI_ROLE_DIM);
        return;
    }
    for (int i = 0; i < s_n; i++) {
        s_row[i] = ui->row(s_title[i], s_sub[i][0] ? s_sub[i] : NULL);
    }
}

static void show_editor(const char *file)
{
    s_page = PAGE_EDIT;
    s_armed = 0;
    put(s_cur, file, 16);
    ui->clear();
    ui->win_back(1);                                    /* back from the editor is ours: save, then the list */
    ui->win_title(file[0] ? "Note" : "New note");
    ui->win_action(PURR_UI_ACT_TRASH);
    s_ta = ui->textarea(NOTE_MAX, "Write something...", 1);
    if (s_ta > 0 && file[0]) {
        read_note(file, s_buf, NOTE_MAX + 1);
        ui->set_text(s_ta, s_buf);
    }
    if (s_ta > 0) {
        ui->focus(s_ta);
    }
}

/* Saves the editor (an empty note is not kept). */
static void save_editor(void)
{
    if (s_page != PAGE_EDIT || s_ta <= 0) {
        return;
    }
    int n = ui->get_text(s_ta, s_buf, sizeof(s_buf));
    if (n <= 0) {
        if (s_cur[0]) {
            st->remove(s_cur);
        }
        return;
    }
    char file[16];
    if (s_cur[0]) {
        put(file, s_cur, 16);
    } else {
        s_n = 0;
        st->list(".", list_cb, NULL);
        name_for(file, next_number());
    }
    int h = st->open(file, PURR_CC_O_WRITE | PURR_CC_O_CREATE | PURR_CC_O_TRUNC);
    if (h < 0) {
        if (con) {
            con->print("notes: could not save\n");
        }
        return;
    }
    st->write(h, s_buf, (size_t)n);
    st->close(h);
}

static void delete_current(void)
{
    if (s_cur[0]) {
        st->remove(s_cur);
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
    con = (const purr_cc_console_t *)env->catcall_get("console", 1);
    ui = (const purr_cc_ui_t *)env->catcall_get("ui", 1);
    st = (const purr_cc_storage_t *)env->catcall_get("storage", 1);
    if (ui == NULL || st == NULL) {
        if (con) {
            con->print("notes: needs the ui and storage catcalls\n");
        }
        return -1;
    }
    show_list();
    return 0;
}

static void on_pause(void)
{
    if (ui != NULL && s_page == PAGE_EDIT) {            /* leaving the front: do not lose what was typed */
        save_editor();
    }
}

static void on_event(const purr_app_event_t *ev)
{
    if (ui == NULL || ev->type != PURR_APPEV_UI || ev->name == NULL) {
        return;
    }
    if (same(ev->name, "click") && s_page == PAGE_LIST) {
        for (int i = 0; i < s_n; i++) {
            if (s_row[i] == ev->arg) {
                show_editor(s_file[i]);
                return;
            }
        }
    } else if (same(ev->name, "action")) {
        if (s_page == PAGE_LIST) {
            show_editor("");
        } else if (s_cur[0] == '\0' && ui->get_text(s_ta, s_buf, sizeof(s_buf)) == 0) {
            show_list();                                /* nothing was written: just close it */
        } else if (!s_armed) {
            s_armed = 1;
            ui->win_title("Delete? Tap again");
        } else {
            delete_current();
            show_list();
        }
    } else if (same(ev->name, "changed") && s_armed) {
        s_armed = 0;                                    /* typing cancels a pending delete */
        ui->win_title(s_cur[0] ? "Note" : "New note");
    } else if (same(ev->name, "back") && s_page == PAGE_EDIT) {
        save_editor();
        show_list();
    }
}

static void on_destroy(int reason)
{
    (void)reason;
}

static const purr_app_table2_t s_table = {
    .base = {.abi_version = PURR_APP_ABI_VERSION_2, .on_create = on_create, .on_destroy = on_destroy},
    .on_pause = on_pause, .on_event = on_event,
    .memory_needed = 16384, .kind = 2, .flags = 0,
};

__attribute__((used))
const purr_app_table_t *notes_entry(void)
{
    return &s_table.base;
}
