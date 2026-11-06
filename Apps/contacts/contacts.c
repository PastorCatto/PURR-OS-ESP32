/*
 * contacts.c - Contacts, a sideloaded UI app: the address book (Messaging/SPEC.md). No libc: only the catcalls it is handed.
 *
 * A list sorted by name with a letter above each group and a search line; a card for one person (a favourite star, one row for each messaging service they are on, Edit and Delete); and an
 * editor (a name and an address for each service). Tapping a service row opens that conversation in Messages. Follows the Android 2.2 Contacts layouts as far as the `ui` catcall allows.
 * Needs the `contacts` and `messaging` permissions: the first use of each asks.
 */
#include <stddef.h>
#include <stdint.h>

#include "purr_app_abi.h"
#include "purr_catcalls.h"

#define NO_OPT __attribute__((noinline, optimize("no-tree-loop-distribute-patterns")))

#define MAX_LIST 24
#define NSVC 4

static const purr_cc_ui_t *ui;
static const purr_cc_contacts_t *ct;
static const purr_cc_messaging_t *mq;
static const purr_cc_app_t *app;

static const struct { const char *key, *name; } k_svc[NSVC] = {
    {"meshtastic", "Meshtastic"}, {"reticulum", "Reticulum"}, {"meshcore", "MeshCore"}, {"echo", "Echo (test)"},
};

enum { PG_LIST, PG_CARD, PG_EDIT, PG_WAIT, PG_NOTE };
static int s_page, s_retry;

static purr_cc_contact_t s_c;                       /* the contact on the card or in the editor */
static int s_editing_new;
static int s_armed;                                 /* Delete was tapped once */

static int h_search, h_fav, h_edit, h_del, h_save, h_cancel, h_name;
static int h_row[MAX_LIST];
static uint32_t s_row_id[MAX_LIST];
static int s_nrows;
static int h_hdr[MAX_LIST];
static int s_nhdr;
static int h_svc[NSVC], h_addr[NSVC];
static char s_filter[24];

NO_OPT static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
NO_OPT static int same(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

NO_OPT static void put(char *dst, const char *src, int cap)
{
    int i = 0;
    while (src && src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

NO_OPT static void append(char *dst, const char *src, int cap) { int n = slen(dst); put(dst + n, src, cap - n); }

NO_OPT static const char *svc_name(const char *key)
{
    for (int i = 0; i < NSVC; i++) {
        if (same(k_svc[i].key, key)) {
            return k_svc[i].name;
        }
    }
    return key;
}

NO_OPT static void note(const char *l1, const char *l2)
{
    s_page = PG_NOTE;
    ui->clear();
    ui->win_back(1);
    ui->win_action(PURR_UI_ACT_NONE);
    ui->win_title("Contacts");
    ui->label(l1, PURR_UI_ROLE_TITLE);
    if (l2) {
        ui->label(l2, PURR_UI_ROLE_DIM);
    }
}

NO_OPT static int explain(int rc, int retry)
{
    if (rc == PURR_MSGCC_ASK) {
        note("Waiting for permission...", "Answer the question on the screen.");
        s_page = PG_WAIT;
        s_retry = retry;
        return 1;
    }
    if (rc == PURR_MSGCC_DENIED) {
        note("Permission was not given.", "Allow it in Settings > Apps.");
        s_page = PG_WAIT;
        s_retry = 0;
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- the list */

NO_OPT static void fill_rows(void)
{
    for (int i = 0; i < s_nrows; i++) {
        ui->remove(h_row[i]);
    }
    for (int i = 0; i < s_nhdr; i++) {
        ui->remove(h_hdr[i]);
    }
    s_nrows = 0;
    s_nhdr = 0;
    purr_cc_contact_t c;
    char letter = 0;
    for (int i = 0; s_nrows < MAX_LIST; i++) {
        int rc = ct->list(s_filter, i, &c);
        if (rc < 0) {
            explain(rc, 2);
            return;
        }
        if (rc == 0) {
            break;
        }
        char first = c.name[0] >= 'a' && c.name[0] <= 'z' ? (char)(c.name[0] - 32) : c.name[0];
        if (first != letter && s_nhdr < MAX_LIST) {
            char l[2] = {first, 0};
            h_hdr[s_nhdr++] = ui->label(l, PURR_UI_ROLE_ACCENT);
            letter = first;
        }
        char sub[64];
        sub[0] = '\0';
        for (int k = 0; k < c.naddr; k++) {
            if (k) append(sub, ", ", 64);
            append(sub, svc_name(c.a[k].service), 64);
        }
        h_row[s_nrows] = ui->row(c.name, sub[0] ? sub : "No messaging address");
        s_row_id[s_nrows] = c.id;
        s_nrows++;
    }
    if (s_nrows == 0) {
        h_hdr[s_nhdr++] = ui->label(s_filter[0] ? "Nobody matches." : "No contacts yet. Tap + to add one.", PURR_UI_ROLE_DIM);
    }
}

NO_OPT static void show_list(void)
{
    s_page = PG_LIST;
    s_nrows = 0;
    s_nhdr = 0;
    ui->clear();
    ui->win_back(1);
    ui->win_action(PURR_UI_ACT_PLUS);
    ui->win_title("Contacts");
    h_search = ui->input_line(23, "Search contacts");
    if (s_filter[0]) {
        ui->set_text(h_search, s_filter);
    }
    fill_rows();
}

/* ---------------------------------------------------------------- the card */

NO_OPT static void show_card(uint32_t id)
{
    if (ct->get(id, &s_c) != 1) {
        show_list();
        return;
    }
    s_page = PG_CARD;
    s_armed = 0;
    ui->clear();
    ui->win_back(1);
    ui->win_action(PURR_UI_ACT_NONE);
    ui->win_title(s_c.name);
    h_fav = ui->row(s_c.fav ? "Favourite: yes" : "Favourite: no", "Tap to change");
    if (s_c.naddr == 0) {
        ui->label("No messaging addresses yet. Tap Edit to add one.", PURR_UI_ROLE_DIM);
    }
    for (int k = 0; k < s_c.naddr && k < NSVC; k++) {
        h_svc[k] = ui->row(svc_name(s_c.a[k].service), s_c.a[k].addr);
    }
    int g = ui->grid(2, 28);
    h_edit = ui->grid_button(g, "Edit");
    h_del = ui->grid_button(g, "Delete");
}

/* ---------------------------------------------------------------- the editor */

NO_OPT static void show_edit(int is_new)
{
    s_page = PG_EDIT;
    s_editing_new = is_new;
    ui->clear();
    ui->win_back(1);
    ui->win_action(PURR_UI_ACT_NONE);
    ui->win_title(is_new ? "New contact" : "Edit contact");
    ui->label("Name", PURR_UI_ROLE_DIM);
    h_name = ui->input_line(23, "Name");
    if (!is_new) {
        ui->set_text(h_name, s_c.name);
    }
    ui->label("Messaging addresses", PURR_UI_ROLE_DIM);
    for (int i = 0; i < NSVC; i++) {
        char ph[40];
        ph[0] = '\0';
        append(ph, k_svc[i].name, 40);
        h_addr[i] = ui->input_line(39, ph);
        if (!is_new) {
            for (int k = 0; k < s_c.naddr; k++) {
                if (same(s_c.a[k].service, k_svc[i].key)) {
                    ui->set_text(h_addr[i], s_c.a[k].addr);
                }
            }
        }
    }
    int g = ui->grid(2, 28);
    h_save = ui->grid_button(g, "Save");
    h_cancel = ui->grid_button(g, "Cancel");
    ui->focus(h_name);
}

NO_OPT static void do_save(void)
{
    char name[24];
    ui->get_text(h_name, name, sizeof(name));
    if (name[0] == '\0') {
        ui->status("Give the contact a name", 1);
        return;
    }
    int id = s_editing_new ? ct->add(name) : (int)s_c.id;
    if (id < 0) {
        if (!explain(id, 2)) {
            ui->status("Could not add the contact (the book is full?)", 1);
        }
        return;
    }
    if (!s_editing_new) {
        ct->rename(s_c.id, name);
    }
    for (int i = 0; i < NSVC; i++) {
        char a[40];
        ui->get_text(h_addr[i], a, sizeof(a));
        if (a[0]) {
            ct->set_addr((uint32_t)id, k_svc[i].key, a);
        } else if (!s_editing_new) {
            ct->remove_addr((uint32_t)id, k_svc[i].key);
        }
    }
    show_card((uint32_t)id);
}

NO_OPT static void go_back(void)
{
    switch (s_page) {
    case PG_CARD:
    case PG_NOTE:
    case PG_WAIT:
        show_list();
        break;
    case PG_EDIT:
        if (s_editing_new) show_list(); else show_card(s_c.id);
        break;
    default:
        app->close_self();
        break;
    }
}

NO_OPT static void on_click(int h)
{
    if (s_page == PG_LIST) {
        for (int i = 0; i < s_nrows; i++) {
            if (h_row[i] == h) {
                show_card(s_row_id[i]);
                return;
            }
        }
    } else if (s_page == PG_CARD) {
        if (h == h_fav) {
            ct->set_fav(s_c.id, !s_c.fav);
            show_card(s_c.id);
        } else if (h == h_edit) {
            show_edit(0);
        } else if (h == h_del) {
            if (!s_armed) {
                s_armed = 1;
                ui->set_text(h_del, "Sure?");
            } else {
                ct->remove(s_c.id);
                ui->status("Deleted", 0);
                show_list();
            }
        } else {
            for (int k = 0; k < s_c.naddr && k < NSVC; k++) {
                if (h_svc[k] == h) {
                    int th = mq != NULL ? mq->thread_for(s_c.a[k].service, s_c.a[k].addr, s_c.name) : PURR_MSGCC_BAD;
                    if (th > 0) {
                        mq->open_thread((uint32_t)th);
                    } else if (!explain(th, 2)) {
                        ui->status("Could not open that conversation", 1);
                    }
                    return;
                }
            }
        }
    } else if (s_page == PG_EDIT) {
        if (h == h_save) {
            do_save();
        } else if (h == h_cancel) {
            go_back();
        }
    }
}

static int on_create(const purr_app_env_t *env, const void *state, uint32_t len)
{
    (void)state;
    (void)len;
    if (env == NULL || env->catcall_get == NULL) {
        return -1;
    }
    app = (const purr_cc_app_t *)env->catcall_get("app", 1);
    ui = (const purr_cc_ui_t *)env->catcall_get("ui", 3);
    ct = (const purr_cc_contacts_t *)env->catcall_get("contacts", 1);
    mq = (const purr_cc_messaging_t *)env->catcall_get("messaging", 1);
    if (ui == NULL) {
        ui = (const purr_cc_ui_t *)env->catcall_get("ui", 1);
        if (ui != NULL) {
            ui->win_title("Contacts");
            ui->label("Contacts needs OS 1.0.27 or newer.", PURR_UI_ROLE_TITLE);
        }
        return ui != NULL ? 0 : -1;
    }
    if (ct == NULL || app == NULL) {
        ui->win_title("Contacts");
        ui->label("Contacts needs OS 1.0.27 or newer.", PURR_UI_ROLE_TITLE);
        return 0;
    }
    show_list();
    return 0;
}

static void on_event(const purr_app_event_t *ev)
{
    if (ui == NULL || ct == NULL) {
        return;
    }
    if (ev->type == PURR_APPEV_PERM) {
        if (s_page == PG_WAIT && ev->arg == 1) {
            show_list();
        } else if (s_page == PG_WAIT) {
            note("Permission was not given.", "Allow it in Settings > Apps.");
            s_page = PG_WAIT;
        }
        return;
    }
    if (ev->type != PURR_APPEV_UI || ev->name == NULL) {
        return;
    }
    if (same(ev->name, "click")) {
        on_click(ev->arg);
    } else if (same(ev->name, "changed") && s_page == PG_LIST && ev->arg == h_search) {
        ui->get_text(h_search, s_filter, sizeof(s_filter));
        fill_rows();
    } else if (same(ev->name, "action") && s_page == PG_LIST) {
        show_edit(1);
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
    .memory_needed = 16384, .kind = 2, .flags = 0,
};

__attribute__((used))
const purr_app_table_t *contacts_entry(void)
{
    return &s_table.base;
}
