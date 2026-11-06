/*
 * messages.c - Messages, a sideloaded UI app: the front end for the messaging services (Messaging/SPEC.md). No libc: only the catcalls it is handed.
 *
 * It never talks to a radio or a server. A messaging service app (Echo, and later Meshtastic, Reticulum, MeshCore) registers with the OS; this app lists the services, the threads (one
 * per service and peer) and the messages through the `messaging` catcall, and sends through it. Names come from the address book (`contacts`). A picture is attached from the user's
 * Pictures folder (`files`), and an emoji is added from a small picker; the emoji draw only once an emoji font is on the device (showtime1.md, Part D).
 *
 * Pages: the thread list, a thread, new message (from contacts, or by typing an address), attach a picture, emoji, services and storage, and the "storage is full" choice.
 * Back goes up one page, then closes the app. Needs `ui` version 4 (the bubble), OS 1.0.27 or newer.
 */
#include <stddef.h>
#include <stdint.h>

#include "purr_app_abi.h"
#include "purr_catcalls.h"

#define NO_OPT __attribute__((noinline, optimize("no-tree-loop-distribute-patterns")))

#define MAX_THREADS 10
#define MAX_SHOWN   6
#define MAX_PAIRS   8
#define DRAFT_MAX   200

static const purr_cc_ui_t *ui;
static const purr_cc_messaging_t *mq;
static const purr_cc_contacts_t *ct;
static const purr_cc_files_t *fl;
static const purr_cc_system_t *sys2;
static const purr_cc_app_t *app;

enum { PG_LIST, PG_THREAD, PG_NEW, PG_NEW_ADDR, PG_PAIRS, PG_ATTACH, PG_EMOJI, PG_SERVICES, PG_FULL, PG_WAIT, PG_NOTE };
static int s_page, s_prev;                          /* the page on show, and the one Back returns to from a side page */
static int s_retry;                                 /* what to redo when a permission answer arrives: 1 = start, 2 = the page on show */

/* the thread on show */
static uint32_t s_thread;
static char s_svc[16], s_peer[40], s_name[24];
static int s_skip;                                  /* messages newer than the window */
static char s_draft[DRAFT_MAX + 1];
static char s_attach[56];

static purr_cc_msg_thread_t s_th[MAX_THREADS];
static uint32_t s_row_thread[MAX_THREADS];
static int s_row[MAX_THREADS];
static int s_nrows;
static purr_cc_msg_t s_msgs[MAX_SHOWN];
static purr_cc_msg_service_t s_sv[4];
static int s_nsv;
static int h_more, h_older, h_input, h_send, h_emoji, h_attach, h_start, h_addr, h_name, h_a, h_b, h_c;
static int s_sel_svc;                               /* the service chosen on the New page */

/* a pair of (contact, service address) on the contact chooser */
static char s_pair_svc[MAX_PAIRS][16], s_pair_addr[MAX_PAIRS][40], s_pair_name[MAX_PAIRS][24];
static int s_pair_row[MAX_PAIRS], s_npairs;

/* the files on the attach page */
static char s_pic[MAX_PAIRS][32];
static int s_pic_row[MAX_PAIRS], s_npics;
static char s_home[48];

static const struct { const char *label, *utf8; } s_emoji[24] = {
    {"smile", "\xf0\x9f\x98\x80"}, {"laugh", "\xf0\x9f\x98\x82"}, {"love", "\xf0\x9f\x98\x8d"}, {"cool", "\xf0\x9f\x98\x8e"},
    {"think", "\xf0\x9f\xa4\x94"}, {"sad", "\xf0\x9f\x98\xa2"}, {"angry", "\xf0\x9f\x98\xa0"}, {"yes", "\xf0\x9f\x91\x8d"},
    {"no", "\xf0\x9f\x91\x8e"}, {"clap", "\xf0\x9f\x91\x8f"}, {"thanks", "\xf0\x9f\x99\x8f"}, {"strong", "\xf0\x9f\x92\xaa"},
    {"heart", "\xe2\x9d\xa4"}, {"fire", "\xf0\x9f\x94\xa5"}, {"sparkle", "\xe2\x9c\xa8"}, {"party", "\xf0\x9f\x8e\x89"},
    {"walk", "\xf0\x9f\x9a\xb6"}, {"tent", "\xe2\x9b\xba"}, {"peak", "\xf0\x9f\x8f\x94"}, {"tree", "\xf0\x9f\x8c\xb2"},
    {"sun", "\xe2\x98\x80"}, {"rain", "\xf0\x9f\x8c\xa7"}, {"photo", "\xf0\x9f\x93\xb7"}, {"battery", "\xf0\x9f\x94\x8b"},
};
static int s_emoji_h[24];

/* ---------------------------------------------------------------- tiny helpers (no libc) */

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

NO_OPT static void append(char *dst, const char *src, int cap)
{
    int n = slen(dst);
    put(dst + n, src, cap - n);
}

NO_OPT static void append_num(char *dst, uint32_t v, int cap)
{
    char tmp[11], out[11];
    int n = 0, k = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 10);
    while (n > 0) {
        out[k++] = (char)tmp[--n];
    }
    out[k] = '\0';
    append(dst, out, cap);
}

NO_OPT static const char *base_of(const char *p)
{
    const char *b = p;
    for (; *p; p++) {
        if (*p == '/' && p[1]) {
            b = p + 1;
        }
    }
    return b;
}

NO_OPT static const char *svc_tag(const char *key)
{
    for (int i = 0; i < s_nsv; i++) {
        if (same(s_sv[i].key, key)) {
            return s_sv[i].tag[0] ? s_sv[i].tag : s_sv[i].key;
        }
    }
    return key;
}

NO_OPT static int svc_index(const char *key)
{
    for (int i = 0; i < s_nsv; i++) {
        if (same(s_sv[i].key, key)) {
            return i;
        }
    }
    return -1;
}

NO_OPT static void load_services(void)
{
    s_nsv = 0;
    while (s_nsv < 4 && mq->services(s_nsv, &s_sv[s_nsv]) == 1) {
        s_nsv++;
    }
}

/* HH:MM of a message time in the user's local time (the offset is worked out from now). "" if the clock was not set. */
NO_OPT static void fmt_time(uint32_t t, char *out, int cap)
{
    out[0] = '\0';
    if (t == 0 || sys2 == NULL || sys2->unix_time == NULL) {
        return;
    }
    purr_cc_tm_t lt;
    int64_t now = sys2->unix_time();
    if (now <= 0 || sys2->local_time == NULL || sys2->local_time(&lt) < 0) {
        return;
    }
    int32_t off = (lt.hour * 3600 + lt.min * 60 + lt.sec) - (int32_t)(now % 86400);
    int32_t sec = (int32_t)((int64_t)t % 86400) + off;
    sec = ((sec % 86400) + 86400) % 86400;
    uint32_t h = (uint32_t)(sec / 3600), m = (uint32_t)((sec % 3600) / 60);
    if (h < 10) append(out, "0", cap);
    append_num(out, h, cap);
    append(out, ":", cap);
    if (m < 10) append(out, "0", cap);
    append_num(out, m, cap);
}

/* ---------------------------------------------------------------- pages */

static void show_list(void);
static void show_thread(uint32_t id);
static void show_new(void);
static void show_services(void);
static void show_full(void);

NO_OPT static void note(const char *l1, const char *l2)
{
    s_page = PG_NOTE;
    ui->clear();
    ui->win_back(1);
    ui->win_action(PURR_UI_ACT_NONE);
    ui->win_title("Messages");
    ui->label(l1, PURR_UI_ROLE_TITLE);
    if (l2) {
        ui->label(l2, PURR_UI_ROLE_DIM);
    }
}

/* A catcall said ASK or DENIED. Returns 1 if a page was drawn for it. */
NO_OPT static int explain(int rc, int retry)
{
    if (rc == PURR_MSGCC_ASK) {
        s_page = PG_WAIT;
        s_retry = retry;
        note("Waiting for permission...", "Answer the question on the screen.");
        s_page = PG_WAIT;
        return 1;
    }
    if (rc == PURR_MSGCC_DENIED) {
        s_page = PG_WAIT;
        s_retry = 0;
        note("Permission was not given.", "Allow it in Settings > Apps.");
        s_page = PG_WAIT;
        return 1;
    }
    return 0;
}

NO_OPT static void show_list(void)
{
    s_page = PG_LIST;
    s_nrows = 0;
    load_services();
    ui->clear();
    ui->win_back(1);
    ui->win_action(PURR_UI_ACT_PLUS);
    ui->win_title("Messages");
    int unread_total = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        int rc = mq->threads(i, &s_th[i]);
        if (rc < 0) {
            if (!explain(rc, 2)) {
                note("Messages could not start.", NULL);
            }
            return;
        }
        if (rc == 0) {
            break;
        }
        purr_cc_msg_thread_t *t = &s_th[i];
        char sub[64], tm[8];
        sub[0] = '\0';
        if (t->last_dir == PURR_MSGCC_OUT) {
            append(sub, t->last_state == PURR_MSGCC_FAILED ? "Not sent: " : "You: ", 64);
        }
        append(sub, t->last[0] ? t->last : "Picture", 64);
        fmt_time(t->last_time, tm, sizeof(tm));
        unread_total += (int)t->unread;
        s_row[s_nrows] = ui->conv_row(t->name[0] ? t->name : t->peer, sub, tm, svc_tag(t->service),
                                      (t->unread > 0 ? PURR_UI_CONV_UNREAD : 0) | (t->last_dir == PURR_MSGCC_OUT && t->last_state == PURR_MSGCC_FAILED ? PURR_UI_CONV_FAILED : 0));
        s_row_thread[s_nrows] = t->id;
        s_nrows++;
    }
    if (s_nrows == 0) {
        ui->label("No conversations yet.", PURR_UI_ROLE_DIM);
        if (s_nsv == 0) {
            ui->label("No messaging app is running. Open one, such as Echo, Meshtastic, Reticulum or MeshCore.", PURR_UI_ROLE_DIM);
        } else {
            ui->label("Tap + to start one.", PURR_UI_ROLE_DIM);
        }
    }
    (void)unread_total;
    h_more = ui->row("More", "Services and storage");
}

NO_OPT static int find_thread(uint32_t id)
{
    purr_cc_msg_thread_t t;
    for (int i = 0; i < 32 && mq->threads(i, &t) == 1; i++) {
        if (t.id == id) {
            put(s_svc, t.service, sizeof(s_svc));
            put(s_peer, t.peer, sizeof(s_peer));
            put(s_name, t.name, sizeof(s_name));
            return 1;
        }
    }
    return 0;
}

NO_OPT static const char *state_text(int dir, int state)
{
    if (dir == PURR_MSGCC_IN) return "";
    return state == PURR_MSGCC_QUEUED ? "Sending..." : state == PURR_MSGCC_SENDING ? "Sending..." : state == PURR_MSGCC_SENT ? "Sent"
         : state == PURR_MSGCC_DELIVERED ? "Delivered" : "Not sent";
}

NO_OPT static void show_thread(uint32_t id)
{
    if (id != s_thread) {
        s_skip = 0;
        s_draft[0] = '\0';
        s_attach[0] = '\0';
    }
    s_thread = id;
    if (!find_thread(id)) {
        s_thread = 0;
        show_list();
        return;
    }
    load_services();
    s_page = PG_THREAD;
    ui->clear();
    ui->win_back(1);
    ui->win_action(PURR_UI_ACT_NONE);
    ui->win_title(s_name[0] ? s_name : s_peer);
    int n = mq->messages(id, s_skip, s_msgs, MAX_SHOWN);
    h_older = 0;
    if (n >= MAX_SHOWN) {
        h_older = ui->row("Older messages", NULL);
    }
    int images = 0;
    for (int i = 0; i < n; i++) {
        char meta[40], tm[8];
        fmt_time(s_msgs[i].time, tm, sizeof(tm));
        meta[0] = '\0';
        append(meta, tm, sizeof(meta));
        const char *st = state_text(s_msgs[i].dir, s_msgs[i].state);
        if (st[0]) {
            if (meta[0]) append(meta, "  ", sizeof(meta));
            append(meta, st, sizeof(meta));
        }
        const char *text = s_msgs[i].text[0] ? s_msgs[i].text : "Picture";
        ui->bubble(text, s_msgs[i].dir == PURR_MSGCC_OUT ? 1 : 0, meta);
        if (s_msgs[i].attach[0] && images < 2) {
            int im = ui->image(70);
            if (im > 0) {
                ui->set_image(im, s_msgs[i].attach);
            }
            images++;
        }
    }
    if (n == 0) {
        ui->label("No messages yet. Say something.", PURR_UI_ROLE_DIM);
    }
    mq->mark_read(id);
    h_input = ui->input_line(DRAFT_MAX, "Type a message");
    if (s_draft[0]) {
        ui->set_text(h_input, s_draft);
    }
    int g = ui->grid(3, 28);
    h_send = ui->grid_button(g, "Send");
    h_emoji = ui->grid_button(g, "Emoji");
    h_attach = ui->grid_button(g, s_attach[0] ? "Remove picture" : "Picture");
    if (s_attach[0]) {
        char l[80];
        l[0] = '\0';
        append(l, "Picture: ", 80);
        append(l, base_of(s_attach), 80);
        ui->label(l, PURR_UI_ROLE_DIM);
    }
    ui->focus(h_input);
}

NO_OPT static void save_draft(void)
{
    if (s_page == PG_THREAD && h_input > 0) {
        ui->get_text(h_input, s_draft, sizeof(s_draft));
    }
}

NO_OPT static void show_new(void)
{
    s_page = PG_NEW;
    load_services();
    ui->clear();
    ui->win_back(1);
    ui->win_action(PURR_UI_ACT_NONE);
    ui->win_title("New message");
    h_a = ct != NULL ? ui->row("From contacts", "Pick a person you have saved") : 0;
    for (int i = 0; i < s_nsv; i++) {
        char t[48];
        t[0] = '\0';
        append(t, "On ", 48);
        append(t, s_sv[i].name, 48);
        s_emoji_h[i] = ui->row(t, "Type the other person's address");     /* reuse the array for the row handles */
    }
    if (s_nsv == 0) {
        ui->label("No messaging app is running.", PURR_UI_ROLE_DIM);
    }
}

NO_OPT static void show_new_addr(int svc)
{
    s_page = PG_NEW_ADDR;
    s_sel_svc = svc;
    ui->clear();
    ui->win_back(1);
    ui->win_action(PURR_UI_ACT_NONE);
    char t[48];
    t[0] = '\0';
    append(t, "New on ", 48);
    append(t, s_sv[svc].name, 48);
    ui->win_title(t);
    ui->label("Their address on this service", PURR_UI_ROLE_DIM);
    h_addr = ui->input_line(39, "address");
    ui->label("A name (optional)", PURR_UI_ROLE_DIM);
    h_name = ui->input_line(23, "name");
    h_start = ui->button("Start");
    ui->focus(h_addr);
}

NO_OPT static void show_pairs(void)
{
    s_page = PG_PAIRS;
    s_npairs = 0;
    load_services();
    ui->clear();
    ui->win_back(1);
    ui->win_action(PURR_UI_ACT_NONE);
    ui->win_title("From contacts");
    purr_cc_contact_t c;
    for (int i = 0; s_npairs < MAX_PAIRS; i++) {
        int rc = ct->list("", i, &c);
        if (rc < 0) {
            if (!explain(rc, 2)) {
                note("Could not read the contacts.", NULL);
            }
            return;
        }
        if (rc == 0) {
            break;
        }
        for (int k = 0; k < c.naddr && s_npairs < MAX_PAIRS; k++) {
            int si = svc_index(c.a[k].service);
            if (si < 0) {
                continue;                                   /* only services that are running */
            }
            int p = s_npairs++;
            put(s_pair_svc[p], c.a[k].service, 16);
            put(s_pair_addr[p], c.a[k].addr, 40);
            put(s_pair_name[p], c.name, 24);
            char sub[64];
            sub[0] = '\0';
            append(sub, s_sv[si].name, 64);
            append(sub, ": ", 64);
            append(sub, c.a[k].addr, 64);
            s_pair_row[p] = ui->row(c.name, sub);
        }
    }
    if (s_npairs == 0) {
        ui->label("No contacts on a running service yet.", PURR_UI_ROLE_DIM);
        ui->label("Add people in Contacts, or type an address on the New page.", PURR_UI_ROLE_DIM);
    }
}

NO_OPT static void list_pictures_cb(const char *name, int is_dir, uint32_t size, void *ctx)
{
    (void)size;
    (void)ctx;
    int n = slen(name);
    if (is_dir || s_npics >= MAX_PAIRS || n >= 32 || n < 5) {
        return;
    }
    const char *e = name + n - 4;
    char t[5];
    for (int i = 0; i < 4; i++) {
        char c = e[i];
        t[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    t[4] = '\0';
    if (same(t, ".jpg") || same(t, ".png") || same(t, ".bmp") || (n > 5 && same(name + n - 5, ".jpeg"))) {
        put(s_pic[s_npics], name, 32);
        s_npics++;
    }
}

NO_OPT static void show_attach(void)
{
    s_page = PG_ATTACH;
    s_npics = 0;
    ui->clear();
    ui->win_back(1);
    ui->win_action(PURR_UI_ACT_NONE);
    ui->win_title("Pick a picture");
    char dir[72];
    dir[0] = '\0';
    append(dir, s_home, 72);
    append(dir, "/Pictures", 72);
    int rc = fl != NULL ? fl->list(dir, list_pictures_cb, NULL) : PURR_FILES_BAD;
    if (rc == PURR_FILES_ASK || rc == PURR_FILES_DENIED) {
        ui->label(rc == PURR_FILES_ASK ? "Waiting for permission..." : "Permission was not given.", PURR_UI_ROLE_TITLE);
        s_page = PG_WAIT;
        s_retry = rc == PURR_FILES_ASK ? 3 : 0;
        return;
    }
    for (int i = 0; i < s_npics; i++) {
        s_pic_row[i] = ui->row(s_pic[i], NULL);
    }
    if (s_npics == 0) {
        ui->label("No pictures in your Pictures folder.", PURR_UI_ROLE_DIM);
    }
}

NO_OPT static void show_emoji(void)
{
    s_page = PG_EMOJI;
    ui->clear();
    ui->win_back(1);
    ui->win_action(PURR_UI_ACT_NONE);
    ui->win_title("Emoji");
    int g = ui->grid(4, 26);
    for (int i = 0; i < 24; i++) {
        s_emoji_h[i] = ui->grid_button(g, s_emoji[i].label);
    }
    ui->label("They show once an emoji font is installed.", PURR_UI_ROLE_DIM);
}

NO_OPT static void kb(char *out, uint32_t v, int cap)
{
    out[0] = '\0';
    append_num(out, (v + 512) / 1024, cap);
    append(out, " KB", cap);
}

NO_OPT static void show_services(void)
{
    s_page = PG_SERVICES;
    load_services();
    ui->clear();
    ui->win_back(1);
    ui->win_action(PURR_UI_ACT_NONE);
    ui->win_title("Services and storage");
    for (int i = 0; i < s_nsv; i++) {
        char t[48], sub[80];
        t[0] = '\0';
        append(t, s_sv[i].name, 48);
        append(t, "   ", 48);
        append(t, s_sv[i].tag, 48);
        sub[0] = '\0';
        append(sub, s_sv[i].online ? "Connected" : "Not connected", 80);
        append(sub, ", up to ", 80);
        append_num(sub, s_sv[i].text_max, 80);
        append(sub, " bytes", 80);
        append(sub, s_sv[i].images == PURR_MSGCC_IMG_NONE ? ", no pictures" : s_sv[i].images == PURR_MSGCC_IMG_PREVIEW ? ", small pictures" : ", pictures", 80);
        ui->label(t, PURR_UI_ROLE_TITLE);
        ui->label(sub, PURR_UI_ROLE_DIM);
    }
    if (s_nsv == 0) {
        ui->label("No messaging app is running.", PURR_UI_ROLE_DIM);
    }
    uint32_t used = 0, limit = 0;
    mq->usage(&used, &limit);
    char u[72], a[16], b[16];
    kb(a, used, sizeof(a));
    kb(b, limit, sizeof(b));
    u[0] = '\0';
    append(u, "Messages use ", 72);
    append(u, a, 72);
    append(u, " of ", 72);
    append(u, b, 72);
    ui->label(u, PURR_UI_ROLE_TEXT);
    h_a = ui->button("Use more space");
}

NO_OPT static void show_full(void)
{
    save_draft();
    s_prev = s_page;
    s_page = PG_FULL;
    ui->clear();
    ui->win_back(1);
    ui->win_action(PURR_UI_ACT_NONE);
    ui->win_title("Storage is full");
    ui->label("Message storage is full", PURR_UI_ROLE_TITLE);
    ui->label("Messages have used all the space set aside for them. What would you like to do?", PURR_UI_ROLE_DIM);
    h_a = ui->button("Use more device space");
    h_b = ui->button("Export everything, then delete the oldest");
    h_c = ui->button("Not now");
}

/* ---------------------------------------------------------------- actions */

NO_OPT static void go_thread_for(const char *svc, const char *addr, const char *name)
{
    int th = mq->thread_for(svc, addr, name);
    if (th > 0) {
        show_thread((uint32_t)th);
    } else if (th == PURR_MSGCC_FULL) {
        show_full();
    } else if (!explain(th, 2)) {
        ui->status("Could not start that conversation", 1);
    }
}

NO_OPT static void do_send(void)
{
    char text[DRAFT_MAX + 1];
    ui->get_text(h_input, text, sizeof(text));
    if (text[0] == '\0' && s_attach[0] == '\0') {
        ui->status("Nothing to send", 1);
        return;
    }
    int si = svc_index(s_svc);
    if (si >= 0 && s_attach[0] && s_sv[si].images == PURR_MSGCC_IMG_NONE) {
        ui->status("This service cannot carry pictures", 1);
        return;
    }
    uint32_t id = 0;
    int rc = mq->send(s_svc, s_peer, s_name, text, s_attach, &id);
    if (rc == PURR_MSGCC_OK) {
        s_draft[0] = '\0';
        s_attach[0] = '\0';
        s_skip = 0;
        show_thread(s_thread);
    } else if (rc == PURR_MSGCC_FULL) {
        put(s_draft, text, sizeof(s_draft));
        show_full();
    } else if (rc == PURR_MSGCC_NOSERVICE) {
        ui->status("That service is not running", 1);
    } else if (!explain(rc, 2)) {
        ui->status("Could not send", 1);
    }
}

NO_OPT static void do_export(void)
{
    if (mq->export_all("/sd/messages-export.txt") != PURR_MSGCC_OK) {
        ui->status("No SD card found to export to", 1);
        return;
    }
    uint32_t used = 0, limit = 0;
    mq->usage(&used, &limit);
    mq->trim(limit / 10 * 6);                               /* the export worked: now the oldest can go */
    ui->status("Exported to the SD card; the oldest messages are removed", 0);
    if (s_thread) show_thread(s_thread); else show_list();
}

NO_OPT static void do_more_space(void)
{
    uint32_t used = 0, limit = 0;
    mq->usage(&used, &limit);
    if (mq->set_limit(limit * 2) == PURR_MSGCC_OK) {
        ui->status("More space for messages", 0);
    } else {
        ui->status("That is the most it can use", 1);
    }
    if (s_page == PG_FULL && s_thread) show_thread(s_thread); else if (s_page == PG_FULL) show_list(); else show_services();
}

NO_OPT static void go_back(void)
{
    switch (s_page) {
    case PG_THREAD:
        save_draft();
        s_thread = 0;
        show_list();
        break;
    case PG_NEW:
    case PG_SERVICES:
    case PG_NOTE:
    case PG_WAIT:
        show_list();
        break;
    case PG_NEW_ADDR:
    case PG_PAIRS:
        show_new();
        break;
    case PG_ATTACH:
    case PG_EMOJI:
    case PG_FULL:
        if (s_thread) show_thread(s_thread); else show_list();
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
            if (s_row[i] == h) {
                show_thread(s_row_thread[i]);
                return;
            }
        }
        if (h == h_more) {
            show_services();
        }
    } else if (s_page == PG_THREAD) {
        if (h == h_send) {
            do_send();
        } else if (h == h_emoji) {
            save_draft();
            show_emoji();
        } else if (h == h_attach) {
            save_draft();
            if (s_attach[0]) {
                s_attach[0] = '\0';
                show_thread(s_thread);
            } else {
                show_attach();
            }
        } else if (h_older > 0 && h == h_older) {
            save_draft();
            s_skip += MAX_SHOWN;
            show_thread(s_thread);
        }
    } else if (s_page == PG_NEW) {
        if (h_a > 0 && h == h_a) {
            show_pairs();
            return;
        }
        for (int i = 0; i < s_nsv; i++) {
            if (s_emoji_h[i] == h) {
                show_new_addr(i);
                return;
            }
        }
    } else if (s_page == PG_NEW_ADDR) {
        if (h == h_start) {
            char addr[40], name[24];
            ui->get_text(h_addr, addr, sizeof(addr));
            ui->get_text(h_name, name, sizeof(name));
            if (addr[0] == '\0') {
                ui->status("Type their address first", 1);
                return;
            }
            go_thread_for(s_sv[s_sel_svc].key, addr, name);
        }
    } else if (s_page == PG_PAIRS) {
        for (int i = 0; i < s_npairs; i++) {
            if (s_pair_row[i] == h) {
                go_thread_for(s_pair_svc[i], s_pair_addr[i], s_pair_name[i]);
                return;
            }
        }
    } else if (s_page == PG_ATTACH) {
        for (int i = 0; i < s_npics; i++) {
            if (s_pic_row[i] == h) {
                s_attach[0] = '\0';
                append(s_attach, s_home, sizeof(s_attach));
                append(s_attach, "/Pictures/", sizeof(s_attach));
                append(s_attach, s_pic[i], sizeof(s_attach));
                show_thread(s_thread);
                return;
            }
        }
    } else if (s_page == PG_EMOJI) {
        for (int i = 0; i < 24; i++) {
            if (s_emoji_h[i] == h) {
                append(s_draft, s_emoji[i].utf8, sizeof(s_draft));
                show_thread(s_thread);
                return;
            }
        }
    } else if (s_page == PG_SERVICES) {
        if (h == h_a) {
            do_more_space();
        }
    } else if (s_page == PG_FULL) {
        if (h == h_a) {
            do_more_space();
        } else if (h == h_b) {
            do_export();
        } else if (h == h_c) {
            go_back();
        }
    }
}

/* ---------------------------------------------------------------- the app */

NO_OPT static void start(void)
{
    int rc = mq->subscribe();
    if (rc < 0) {
        if (!explain(rc, 1)) {
            note("Messages could not start.", NULL);
        }
        return;
    }
    uint32_t req = mq->take_open_request();
    if (req != 0) {
        show_thread(req);
    } else {
        show_list();
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
    ui = (const purr_cc_ui_t *)env->catcall_get("ui", 4);
    mq = (const purr_cc_messaging_t *)env->catcall_get("messaging", 1);
    ct = (const purr_cc_contacts_t *)env->catcall_get("contacts", 1);
    fl = (const purr_cc_files_t *)env->catcall_get("files", 1);
    sys2 = (const purr_cc_system_t *)env->catcall_get("system", 2);
    if (ui == NULL) {
        ui = (const purr_cc_ui_t *)env->catcall_get("ui", 1);
        if (ui != NULL) {
            ui->win_title("Messages");
            ui->label("Messages needs OS 1.0.27 or newer.", PURR_UI_ROLE_TITLE);
        }
        return ui != NULL ? 0 : -1;
    }
    if (mq == NULL || app == NULL) {
        ui->win_title("Messages");
        ui->label("Messages needs OS 1.0.27 or newer.", PURR_UI_ROLE_TITLE);
        return 0;
    }
    if (fl != NULL) {
        fl->home(s_home, sizeof(s_home));
    }
    start();
    return 0;
}

static void on_event(const purr_app_event_t *ev)
{
    if (ui == NULL || mq == NULL || ui->bubble == NULL) {
        return;
    }
    if (ev->type == PURR_APPEV_PERM) {
        if (s_page == PG_WAIT && ev->arg == 1) {
            if (s_retry == 1) start();
            else if (s_retry == 3) show_attach();
            else if (s_thread) show_thread(s_thread);
            else show_list();
        } else if (s_page == PG_WAIT) {
            s_retry = 0;
            note("Permission was not given.", "Allow it in Settings > Apps.");
            s_page = PG_WAIT;
        }
        return;
    }
    if (ev->type == PURR_APPEV_USER && ev->name != NULL) {
        const char *n = ev->name;
        if (same(n, "msg.changed")) {
            if (s_page == PG_LIST) {
                show_list();
            } else if (s_page == PG_THREAD && (ev->arg == 0 || (uint32_t)ev->arg == s_thread)) {
                save_draft();
                show_thread(s_thread);
            }
        } else if (same(n, "msg.open") && ev->arg > 0) {
            mq->take_open_request();
            s_thread = 0;
            show_thread((uint32_t)ev->arg);
        }
        return;
    }
    if (ev->type != PURR_APPEV_UI || ev->name == NULL) {
        return;
    }
    if (same(ev->name, "click")) {
        on_click(ev->arg);
    } else if (same(ev->name, "action") && s_page == PG_LIST) {
        show_new();
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
const purr_app_table_t *messages_entry(void)
{
    return &s_table.base;
}
