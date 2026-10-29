/*
 * store.c - the Store, a UI app: a window onto the `store` catcall (Repositories/SPEC.md). No libc.
 *
 * It lists what the repositories you trust offer (apps, assets and extras), shows who made it, how far it is trusted and which permissions it asks for, and asks
 * the OS to install, update or remove it. The OS does the work and keeps going if this window closes; the Store's companion (storesvc) looks for updates in the
 * background. Adding a repository (administrators) shows its name, its maintainer and its fingerprint first, and only a tap on "Trust it" adds it.
 *
 * Every button's outcome goes to the status strip at the top (5 seconds); an error also sits under the button that failed. Back goes up one page, then closes.
 */
#include <stddef.h>
#include <stdint.h>

#include "purr_app_abi.h"
#include "purr_catcalls.h"

#define NO_OPT __attribute__((noinline, optimize("no-tree-loop-distribute-patterns")))
#define PAGE_ROWS 8
#define MAX_PAIRS 6

static const purr_cc_ui_t *ui;
static const purr_cc_store_t *st;
static const purr_cc_app_t *app;

enum { PG_HOME, PG_LIST, PG_DETAIL, PG_REPOS, PG_REPO, PG_ADD, PG_CONFIRM, PG_WAIT };
enum { J_NONE, J_REFRESH, J_CHECK, J_INSTALL, J_UPDATE_ALL, J_REMOVE, J_ADD, J_CONFIRM, J_REPO_REMOVE, J_BOOTSTRAP };

static int s_page = PG_HOME;
static int s_mode;                                   /* the list: 0 apps, 1 assets and extras, 2 updates */
static int s_off;                                    /* first row of the list page */
static int s_job;
static int s_armed;
static int s_cur_msg;
static int s_sel;                                    /* the item on the detail page: index into the OS's list */
static char s_repo_id[20], s_repo_name[40];          /* the repository on the repository page */
static purr_store_item_t s_item;

static int s_pair_btn[MAX_PAIRS], s_pair_msg[MAX_PAIRS], s_npair;
static int s_row[PAGE_ROWS + 4], s_row_val[PAGE_ROWS + 4], s_nrow;
static int h_home[6], h_check, h_set_up, h_act, h_remove, h_all, h_add, h_addr, h_go, h_trust, h_cancel, h_repo_remove;

static char s_retry_what[16], s_retry_a[40], s_retry_b[20];
static int s_retry_job;

/* ---------------------------------------------------------------- tiny helpers */

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

NO_OPT static void status(const char *t, int bad)
{
    if (ui->version >= 2 && ui->status != NULL) {
        ui->status(t, bad);
    }
}

NO_OPT static void fail(const char *t)
{
    status(t, 1);
    if (s_cur_msg > 0) {
        ui->set_text(s_cur_msg, t);
    }
}

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

NO_OPT static void begin_page(int page, const char *title)
{
    s_page = page;
    s_npair = 0;
    s_nrow = 0;
    s_armed = 0;
    s_cur_msg = 0;
    ui->clear();
    ui->win_back(1);
    ui->win_action(PURR_UI_ACT_NONE);
    ui->win_title(title);
}

static void show_home(void);
static void show_list(int mode, int off);
static void show_detail(int idx);
static void show_repos(void);
static void show_repo(const char *id, const char *name);
static void show_add(void);
static void show_confirm(void);

NO_OPT static void lines(const char *text, int role)
{
    char buf[48];
    int n = 0;
    for (const char *p = text;; p++) {
        if (*p == '\n' || *p == '\0') {
            buf[n] = '\0';
            if (n > 0) {
                ui->label(buf, role);
            }
            n = 0;
            if (*p == '\0') {
                break;
            }
        } else if (n < 47) {
            buf[n++] = *p;
        } else {                                   /* a long line: break it where the label is full */
            buf[n] = '\0';
            ui->label(buf, role);
            n = 0;
            buf[n++] = *p;
        }
    }
}

NO_OPT static void show_wait(const char *l1, const char *l2)
{
    begin_page(PG_WAIT, "Store");
    ui->label(l1, PURR_UI_ROLE_TITLE);
    if (l2) {
        ui->label(l2, PURR_UI_ROLE_DIM);
    }
}

NO_OPT static void begin_job(int job, const char *what, const char *a, const char *b, const char *busy_text)
{
    int rc = st->start(what, a ? a : "", b ? b : "");
    if (rc == PURR_STORE_ASK) {
        put(s_retry_what, what, sizeof(s_retry_what));
        put(s_retry_a, a ? a : "", sizeof(s_retry_a));
        put(s_retry_b, b ? b : "", sizeof(s_retry_b));
        s_retry_job = job;
        show_wait("Waiting for permission...", "Answer the question on the screen.");
        return;
    }
    if (rc == PURR_STORE_DENIED) {
        fail("Not allowed. The Store needs the permission to install apps, and only an administrator can add repositories.");
        return;
    }
    if (rc == PURR_STORE_BUSY) {
        fail("Still busy with the last request.");
        return;
    }
    if (rc != PURR_STORE_OK) {
        fail("Could not start that.");
        return;
    }
    s_job = job;
    status(busy_text, 0);
}

/* ---------------------------------------------------------------- pages */

NO_OPT static void show_home(void)
{
    begin_page(PG_HOME, "Store");
    int n = st->item_count();
    if (n == PURR_STORE_ASK) {
        s_retry_what[0] = '\0';
        show_wait("Waiting for permission...", "Answer the question on the screen.");
        return;
    }
    if (n < 0) {
        ui->label("The Store is not allowed to run for this account.", PURR_UI_ROLE_TITLE);
        return;
    }
    char name[40], info[64], id[20];
    int have_repo = st->repo(0, name, sizeof(name), info, sizeof(info), id, sizeof(id)) == 1;
    h_set_up = 0;
    if (!have_repo) {
        ui->label("No repository is trusted yet.", PURR_UI_ROLE_TITLE);
        ui->label("A repository is where apps come from. Set up the official one, or add your own.", PURR_UI_ROLE_DIM);
        if (st->is_admin()) {
            h_set_up = action("Set up the Store");
        } else {
            ui->label("Ask an administrator to set it up.", PURR_UI_ROLE_DIM);
        }
        h_home[3] = ui->row("Repositories", "Add one, or see what you trust");
        return;
    }
    int up = st->updates();
    char sub[48];
    h_home[0] = 0;
    if (up > 0) {
        put(sub, "", sizeof(sub));
        append_num(sub, up, sizeof(sub));
        append(sub, up == 1 ? " waiting" : " waiting", sizeof(sub));
        h_home[0] = ui->row("Updates", sub);
    }
    h_home[1] = ui->row("Apps", "From your repositories");
    h_home[2] = ui->row("Assets and extras", "Fonts, wallpapers, data");
    h_home[3] = ui->row("Repositories", "What you trust");
    h_check = action("Check for updates");
    if (!st->sd_present()) {
        ui->label("No SD card: items that need one are not offered to install.", PURR_UI_ROLE_DIM);
    }
}

NO_OPT static int visible(const purr_store_item_t *it, int mode)
{
    if (mode == 2) {
        return it->state == PURR_STORE_UPDATE;
    }
    if (mode == 1) {
        return it->type != 0;
    }
    return it->type == 0;
}

NO_OPT static void show_list(int mode, int off)
{
    s_mode = mode;
    s_off = off;
    begin_page(PG_LIST, mode == 2 ? "Updates" : mode == 1 ? "Assets and extras" : "Apps");
    int n = st->item_count();
    int seen = 0, shown = 0, more = 0;
    for (int i = 0; i < n; i++) {
        if (st->item(i, &s_item) != 0 || !visible(&s_item, mode)) {
            continue;
        }
        if (seen++ < off) {
            continue;
        }
        if (shown >= PAGE_ROWS) {
            more = 1;
            break;
        }
        char sub[64];
        put(sub, s_item.version, sizeof(sub));
        if (s_item.state == PURR_STORE_UPDATE) {
            append(sub, " (update from ", sizeof(sub));
            append(sub, s_item.installed, sizeof(sub));
            append(sub, ")", sizeof(sub));
        } else if (s_item.state == PURR_STORE_INSTALLED) {
            append(sub, "  installed", sizeof(sub));
        }
        append(sub, "  ", sizeof(sub));
        append(sub, s_item.tier, sizeof(sub));
        s_row[s_nrow] = ui->row(s_item.title, sub);
        s_row_val[s_nrow] = i;
        s_nrow++;
        shown++;
    }
    if (shown == 0) {
        ui->label(mode == 2 ? "Everything is up to date." : "Nothing here yet. Check for updates on the first page to fetch the latest lists.", PURR_UI_ROLE_DIM);
    }
    h_all = 0;
    if (more) {
        s_row[s_nrow] = ui->row("More...", "");
        s_row_val[s_nrow] = -1;
        s_nrow++;
    }
    if (mode == 2 && shown > 0) {
        h_all = action("Update all");
    }
}

NO_OPT static void show_detail(int idx)
{
    s_sel = idx;
    if (st->item(idx, &s_item) != 0) {
        show_list(s_mode, s_off);
        return;
    }
    begin_page(PG_DETAIL, s_item.title);
    char t[96];
    put(t, s_item.type == 0 ? "App " : s_item.type == 1 ? "Asset " : "Extra ", sizeof(t));
    append(t, s_item.version, sizeof(t));
    ui->label(t, PURR_UI_ROLE_TITLE);
    if (s_item.desc[0]) {
        lines(s_item.desc, PURR_UI_ROLE_TEXT);
    }
    put(t, "From ", sizeof(t));
    append(t, s_item.repo, sizeof(t));
    append(t, " (", sizeof(t));
    append(t, s_item.tier, sizeof(t));
    append(t, ")", sizeof(t));
    ui->label(t, PURR_UI_ROLE_DIM);
    if (s_item.perms[0]) {
        put(t, "Asks for: ", sizeof(t));
        append(t, s_item.perms, sizeof(t));
        lines(t, PURR_UI_ROLE_DIM);
    }
    put(t, "Size: ", sizeof(t));
    append_num(t, (int)((s_item.size + 1023) / 1024), sizeof(t));
    append(t, " KB", sizeof(t));
    if (s_item.needs_sd) {
        append(t, ", needs an SD card", sizeof(t));
    } else if (s_item.target == 1) {
        append(t, ", goes on an SD card", sizeof(t));
    }
    ui->label(t, PURR_UI_ROLE_DIM);
    if (s_item.installed[0]) {
        put(t, "Installed: ", sizeof(t));
        append(t, s_item.installed, sizeof(t));
        ui->label(t, PURR_UI_ROLE_DIM);
    }
    h_act = action(s_item.state == PURR_STORE_UPDATE ? "Update" : s_item.state == PURR_STORE_INSTALLED ? "Install again" : "Install");
    h_remove = s_item.installed[0] ? action("Remove") : 0;
}

NO_OPT static void show_repos(void)
{
    begin_page(PG_REPOS, "Repositories");
    char name[40], info[64], id[20];
    int n = 0;
    while (n < 6 && st->repo(n, name, sizeof(name), info, sizeof(info), id, sizeof(id)) == 1) {
        s_row[s_nrow] = ui->row(name, info);
        s_row_val[s_nrow] = n;
        s_nrow++;
        n++;
    }
    if (n == 0) {
        ui->label("None yet.", PURR_UI_ROLE_DIM);
    }
    h_add = 0;
    if (st->is_admin()) {
        h_add = ui->button("Add a repository");
    } else {
        ui->label("Only an administrator can add or remove repositories.", PURR_UI_ROLE_DIM);
    }
}

NO_OPT static void show_repo(const char *id, const char *name)
{
    put(s_repo_id, id, sizeof(s_repo_id));
    put(s_repo_name, name, sizeof(s_repo_name));
    begin_page(PG_REPO, name);
    ui->label("Apps it installed keep working until the next check, then ask to be reinstalled.", PURR_UI_ROLE_DIM);
    h_repo_remove = st->is_admin() ? action("Stop trusting it") : 0;
}

NO_OPT static void show_add(void)
{
    begin_page(PG_ADD, "Add a repository");
    ui->label("Type the address of the repository's release or web page, like https://github.com/name/repo/releases/download/tag", PURR_UI_ROLE_DIM);
    h_addr = ui->textarea(90, "https://...", 0);
    h_go = action("Look it up");
    if (h_addr > 0) {
        ui->focus(h_addr);
    }
}

NO_OPT static void show_confirm(void)
{
    begin_page(PG_CONFIRM, "Trust this repository?");
    char text[260];
    if (st->pending(text, sizeof(text)) > 0) {
        lines(text, PURR_UI_ROLE_TEXT);
    }
    ui->label("Compare the fingerprint with the one the maintainer publishes. Only trust a repository you know.", PURR_UI_ROLE_DIM);
    h_trust = action("Trust it");
    h_cancel = ui->button("Cancel");
}

/* ---------------------------------------------------------------- jobs ending */

NO_OPT static void on_job(int arg)
{
    char r[200];
    int n = st->result(r, sizeof(r));
    if (n < 0) {
        r[0] = '\0';
    }
    int job = s_job;
    s_job = J_NONE;
    if (arg == 1) {
        fail(r[0] ? r : "That did not work.");
        return;
    }
    status(r[0] ? r : "Done.", 0);
    switch (job) {
    case J_ADD:
        show_confirm();
        break;
    case J_CONFIRM:
    case J_REPO_REMOVE:
        show_repos();
        break;
    case J_BOOTSTRAP:
    case J_REFRESH:
    case J_CHECK:
        show_home();
        break;
    case J_INSTALL:
    case J_REMOVE:
        show_detail(s_sel);
        break;
    case J_UPDATE_ALL:
        show_list(2, 0);
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- clicks */

NO_OPT static void on_click(int h)
{
    mark_pressed(h);
    if (s_page == PG_HOME) {
        if (h_set_up > 0 && h == h_set_up) {
            begin_job(J_BOOTSTRAP, "bootstrap", "", "", "Setting up the Store...");
        } else if (h == h_home[0] && h_home[0] > 0) {
            show_list(2, 0);
        } else if (h == h_home[1]) {
            show_list(0, 0);
        } else if (h == h_home[2]) {
            show_list(1, 0);
        } else if (h == h_home[3]) {
            show_repos();
        } else if (h == h_check) {
            begin_job(J_CHECK, "check", "", "", "Checking...");
        }
    } else if (s_page == PG_LIST) {
        if (h_all > 0 && h == h_all) {
            begin_job(J_UPDATE_ALL, "update-all", "", "", "Updating...");
            return;
        }
        for (int i = 0; i < s_nrow; i++) {
            if (s_row[i] == h) {
                if (s_row_val[i] < 0) {
                    show_list(s_mode, s_off + PAGE_ROWS);
                } else {
                    show_detail(s_row_val[i]);
                }
                return;
            }
        }
    } else if (s_page == PG_DETAIL) {
        if (h == h_act) {
            begin_job(J_INSTALL, "install", s_item.name, s_item.repo_id, s_item.state == PURR_STORE_UPDATE ? "Updating..." : "Installing...");
        } else if (h_remove > 0 && h == h_remove) {
            if (!s_armed) {
                s_armed = 1;
                ui->set_text(s_cur_msg, "Tap again to remove it.");
            } else {
                begin_job(J_REMOVE, "remove", s_item.name, s_item.repo_id, "Removing...");
            }
        }
    } else if (s_page == PG_REPOS) {
        if (h_add > 0 && h == h_add) {
            show_add();
            return;
        }
        for (int i = 0; i < s_nrow; i++) {
            if (s_row[i] == h) {
                char name[40], info[64], id[20];
                if (st->repo(s_row_val[i], name, sizeof(name), info, sizeof(info), id, sizeof(id)) == 1) {
                    show_repo(id, name);
                }
                return;
            }
        }
    } else if (s_page == PG_REPO) {
        if (h_repo_remove > 0 && h == h_repo_remove) {
            if (!s_armed) {
                s_armed = 1;
                ui->set_text(s_cur_msg, "Tap again to stop trusting it.");
            } else {
                begin_job(J_REPO_REMOVE, "repo-remove", s_repo_id, "", "Removing...");
            }
        }
    } else if (s_page == PG_ADD) {
        if (h == h_go) {
            char addr[100];
            ui->get_text(h_addr, addr, sizeof(addr));
            if (addr[0] == '\0') {
                fail("Type an address first.");
                return;
            }
            begin_job(J_ADD, "repo-add", addr, "", "Looking it up...");
        }
    } else if (s_page == PG_CONFIRM) {
        if (h == h_trust) {
            begin_job(J_CONFIRM, "repo-confirm", "", "", "Adding...");
        } else if (h == h_cancel) {
            show_repos();
        }
    }
}

NO_OPT static void go_back(void)
{
    switch (s_page) {
    case PG_HOME:
        app->close_self();
        break;
    case PG_LIST:
    case PG_REPOS:
        show_home();
        break;
    case PG_DETAIL:
        show_list(s_mode, s_off);
        break;
    case PG_REPO:
    case PG_ADD:
    case PG_CONFIRM:
        show_repos();
        break;
    default:
        show_home();
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
    st = (const purr_cc_store_t *)env->catcall_get("store", 1);
    app = (const purr_cc_app_t *)env->catcall_get("app", 1);
    if (ui == NULL || st == NULL || app == NULL) {
        return -1;
    }
    show_home();
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
                char what[16], a[40], b[20];
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
const purr_app_table_t *store_entry(void)
{
    return &s_table.base;
}
