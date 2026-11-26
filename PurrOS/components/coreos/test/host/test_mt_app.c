/* The Meshtastic service app (Apps/meshtastic/meshtastic.c) run as built, against fake catcalls: a scripted radio and a fake Messages. A second node (the protocol core, as the
 * test's "other device") talks to it over the fake air. */
#include <stdio.h>
#include <string.h>

#include "testkit.h"
#include "purr_app_abi.h"
#include "purr_catcalls.h"
#include "../../../../../Apps/meshtastic/meshtastic.c"

/* ---------------------------------------------------------------- fakes */

static uint32_t clock_ms = 5000;
static char console_log[512];
/* two files in the app's folder: config.bin and nodes.bin; anything else (join.txt, link.txt) reads as missing and writes go nowhere */
static uint8_t file_data[2][sizeof(config_t) + 64 + PURR_MT_MAX_NODES * (sizeof(purr_mt_nodeinfo_t) + 8)];
static int file_len[2], file_pos[2], file_which[8];
static int file_open;

static int which_file(const char *p) { return strcmp(p, "config.bin") == 0 ? 0 : (strcmp(p, "nodes.bin") == 0 ? 1 : -1); }

static void f_print(const char *s) { strncat(console_log, s, sizeof(console_log) - strlen(console_log) - 1); }
static uint32_t f_uptime(void) { return clock_ms; }
static void f_sleep(uint32_t ms) { clock_ms += ms; }
static int timer_id = 7;
static int f_timer_start(uint32_t ms, int repeat) { (void)ms; (void)repeat; return timer_id; }
static void f_timer_cancel(int id) { (void)id; }

static int f_open(const char *p, int flags)
{
    int w = which_file(p);
    if (w < 0) {
        return (flags & PURR_CC_O_WRITE) ? 4 : -1;
    }
    file_pos[w] = 0;
    if (flags & PURR_CC_O_TRUNC) {
        file_len[w] = 0;
    }
    if (!(flags & PURR_CC_O_WRITE) && file_len[w] == 0) {
        return -1;
    }
    file_open = 1;
    file_which[0] = w;
    return 3 + w * 10;                                                  /* 3 for config.bin, 13 for nodes.bin */
}
static int f_read(int h, void *b, size_t n)
{
    int w = h == 13;
    size_t k = (size_t)(file_len[w] - file_pos[w]) < n ? (size_t)(file_len[w] - file_pos[w]) : n;
    memcpy(b, file_data[w] + file_pos[w], k);
    file_pos[w] += (int)k;
    return (int)k;
}
static int f_write(int h, const void *b, size_t n)
{
    if (h == 4) {
        return (int)n;
    }
    int w = h == 13;
    memcpy(file_data[w] + file_len[w], b, n);
    file_len[w] += (int)n;
    return (int)n;
}
static int f_close(int h) { (void)h; file_open = 0; return 0; }
static int f_remove(const char *p) { (void)p; return 0; }

/* the radio */
static int radio_present = 1, lease_rc, lease_calls, release_calls;
static purr_cc_radio_cfg_t last_cfg;
static uint8_t air_sent[16][255];
static int air_sent_len[16], air_sent_n;
static uint8_t air_in[16][255];
static int air_in_len[16], air_in_r, air_in_w;

static int r_count(void) { return radio_present; }
static int r_lease(const purr_cc_radio_cfg_t *c) { lease_calls++; if (lease_rc == 0) last_cfg = *c; return lease_rc; }
static void r_release(void) { release_calls++; }
static int r_send(const uint8_t *d, size_t n, uint32_t t) { (void)t; if (air_sent_n < 16) { memcpy(air_sent[air_sent_n], d, n); air_sent_len[air_sent_n++] = (int)n; } return 0; }
static int r_receive(uint8_t *b, size_t cap, purr_cc_radio_rx_t *info, uint32_t t)
{
    (void)t;
    (void)cap;
    if (air_in_r == air_in_w) {
        return 0;
    }
    int n = air_in_len[air_in_r];
    memcpy(b, air_in[air_in_r], (size_t)n);
    air_in_r++;
    info->rssi_dbm = -90;
    info->snr_db = 6;
    return n;
}
static int r_holder(char *a, size_t c) { (void)a; (void)c; return 0; }
static uint32_t r_id(void) { return 0xA1B2C3D4; }
static uint32_t rnd_state = 12345;
static uint32_t r_rand(void) { rnd_state = rnd_state * 1664525u + 1013904223u; return rnd_state >> 8; }

/* messaging */
static int service_calls, online_state = -1;
static char in_peer[8][40], in_name[8][24], in_text[8][PURR_MSGCC_TEXT];
static int in_n;
static struct { uint32_t id; char peer[40]; char text[241]; } outq[8];
static int out_r, out_w;
static uint32_t deliv_id[16];
static int deliv_state[16], deliv_n;

static int m_register(const char *k, const char *n, const char *t, int tm, int im, uint32_t imm, int r)
{
    (void)n; (void)t; (void)tm; (void)im; (void)imm; (void)r;
    service_calls++;
    return strcmp(k, "meshtastic") == 0 ? 1 : -1;
}
static void m_online(int o) { online_state = o; }
static int m_next(purr_cc_msg_t *out, char *peer, size_t cap)
{
    if (out_r == out_w) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    out->id = outq[out_r].id;
    strcpy(out->text, outq[out_r].text);
    snprintf(peer, cap, "%s", outq[out_r].peer);
    out_r++;
    return 1;
}
static int m_delivery(uint32_t id, int st) { if (deliv_n < 16) { deliv_id[deliv_n] = id; deliv_state[deliv_n++] = st; } return 0; }
static int m_incoming(const char *peer, const char *name, const char *text, const char *attach)
{
    (void)attach;
    if (in_n < 8) {
        snprintf(in_peer[in_n], sizeof(in_peer[0]), "%s", peer);
        snprintf(in_name[in_n], sizeof(in_name[0]), "%s", name ? name : "");
        snprintf(in_text[in_n], sizeof(in_text[0]), "%s", text);
        in_n++;
    }
    return 0;
}


/* the ui and app catcalls: every widget gets the next number; the test reads what was made and presses what it wants */
static int ui_next = 100, ui_rows_made, ui_buttons_made, ui_clears, closed;
static char ui_title[40], ui_input[400];
static int ui_last_row, ui_last_btn;
static int ui_label_n;
static char ui_labels[16][120];
static void u_clear(void) { ui_clears++; ui_rows_made = ui_buttons_made = ui_label_n = 0; }
static void u_title(const char *s) { snprintf(ui_title, sizeof(ui_title), "%s", s); }
static void u_action(int a) { (void)a; }
static void u_back(int a) { (void)a; }
static int u_label(const char *s, int r) { (void)r; if (ui_label_n < 16) snprintf(ui_labels[ui_label_n++], 120, "%s", s); return ++ui_next; }
static int u_button(const char *s) { (void)s; ui_buttons_made++; return ui_last_btn = ++ui_next; }
static int u_row(const char *a, const char *b) { (void)a; (void)b; ui_rows_made++; return ui_last_row = ++ui_next; }
static int u_spacer(int h) { (void)h; return ++ui_next; }
static int u_input(int m, const char *p) { (void)m; (void)p; return ++ui_next; }
static void u_set_text(int h, const char *s) { (void)h; snprintf(ui_input, sizeof(ui_input), "%s", s); }
static int u_get_text(int h, char *b, size_t cap) { (void)h; snprintf(b, cap, "%s", ui_input); return (int)strlen(b); }
static void u_focus(int h) { (void)h; }
static void u_status(const char *m, int bad) { (void)m; (void)bad; }
static purr_cc_ui_t s_ui = {.version = 5};
static void a_close(void) { closed++; }
static int went_home;
static void a_home(void) { went_home++; }
static purr_cc_app_t s_app = {.version = 1};
static purr_cc_contacts_t s_con_tab = {.version = 1};
static uint32_t contact_found;
static int contacts_added, addr_set;
static uint32_t c_find(const char *s, const char *a) { (void)s; (void)a; return contact_found; }
static int c_add(const char *n) { (void)n; contacts_added++; return 7; }
static int c_set_addr(uint32_t id, const char *s, const char *a) { (void)id; (void)s; (void)a; addr_set++; contact_found = 7; return 0; }
static int thread_for_calls, open_thread_calls;
static int m_thread_for(const char *svc, const char *peer, const char *name) { (void)svc; (void)peer; (void)name; thread_for_calls++; return 42; }
static int m_open_thread(uint32_t id) { (void)id; open_thread_calls++; return 0; }

static const purr_cc_console_t s_con = {.version = 1, .print = f_print};
static const purr_cc_sched_t s_sch = {.version = 1, .uptime_ms = f_uptime, .sleep_ms = f_sleep, .timer_start = f_timer_start, .timer_cancel = f_timer_cancel};
static const purr_cc_storage_t s_stg = {.version = 1, .open = f_open, .read = f_read, .write = f_write, .close = f_close, .remove = f_remove};
static purr_cc_messaging_t s_msg = {.version = 1};
static purr_cc_radio_t s_rad = {.version = 1};

static const void *catcall_get(const char *name, uint32_t v)
{
    (void)v;
    if (strcmp(name, "console") == 0) return &s_con;
    if (strcmp(name, "sched") == 0) return &s_sch;
    if (strcmp(name, "storage") == 0) return &s_stg;
    if (strcmp(name, "messaging") == 0) return &s_msg;
    if (strcmp(name, "radio") == 0) return &s_rad;
    if (strcmp(name, "ui") == 0) return &s_ui;
    if (strcmp(name, "app") == 0) return &s_app;
    if (strcmp(name, "contacts") == 0) return &s_con_tab;
    return NULL;
}

/* ---------------------------------------------------------------- the other device */

static purr_mt_t other;
static uint32_t other_rng = 777;
static uint32_t other_rand(void) { other_rng = other_rng * 22695477u + 1; return other_rng >> 8; }

static void other_init(void)
{
    purr_mt_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.my_num = 0x0000BEEF;
    strcpy(c.long_name, "Other Device");
    strcpy(c.short_name, "OTHR");
    c.hop_limit = 3;
    c.rebroadcast = 1;
    c.region = purr_mt_region_by_name("US");
    c.preset = purr_mt_preset_by_name("LongFast");
    c.rand32 = other_rand;
    purr_mt_node_init(&other, &c, clock_ms);
    uint8_t k = 1;
    purr_mt_channel_set(&other, 0, "LongFast", &k, 1, 1);
}

static void other_sends(uint32_t to, const char *text)
{
    purr_mt_node_send_text(&other, clock_ms, to, 0, text);
    for (int i = 0; i < 400; i++) {                                     /* run its clock until the frame is due */
        uint8_t f[255];
        size_t n;
        clock_ms += 10;
        if (purr_mt_node_tick(&other, clock_ms, f, sizeof(f), &n)) {
            memcpy(air_in[air_in_w], f, n);
            air_in_len[air_in_w++ % 16] = (int)n;
            return;
        }
    }
}

static purr_app_event_t timer_ev(void)
{
    purr_app_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = PURR_APPEV_TIMER;
    e.arg = (uint32_t)timer_id;
    return e;
}

static void ticks(int n)
{
    for (int i = 0; i < n; i++) {
        clock_ms += 20;
        purr_app_event_t e = timer_ev();
        on_event(&e);
    }
}

/* what the app has put on the air, handed to the other device, which acts on it */
static void air_to_other(void)
{
    for (int i = 0; i < air_sent_n; i++) {
        purr_mt_node_rx(&other, clock_ms, air_sent[i], (size_t)air_sent_len[i], -80, 5);
    }
    air_sent_n = 0;
}

int main(void)
{
    s_msg.service_register = m_register;
    s_msg.service_online = m_online;
    s_msg.next_outgoing = m_next;
    s_msg.delivery = m_delivery;
    s_msg.incoming = m_incoming;
    s_rad.count = r_count;
    s_rad.lease = r_lease;
    s_rad.release = r_release;
    s_rad.send = r_send;
    s_rad.receive = r_receive;
    s_rad.holder = r_holder;
    s_rad.device_id = r_id;
    s_rad.random32 = r_rand;
    s_ui.win_title = u_title; s_ui.win_action = u_action; s_ui.win_back = u_back; s_ui.clear = u_clear; s_ui.label = u_label; s_ui.button = u_button;
    s_ui.row = u_row; s_ui.spacer = u_spacer; s_ui.input_line = u_input; s_ui.set_text = u_set_text; s_ui.get_text = u_get_text; s_ui.focus = u_focus; s_ui.status = u_status;
    s_app.close_self = a_close;
    s_app.go_home = a_home;
    s_app.version = 2;
    s_con_tab.find = c_find;
    s_con_tab.add = c_add;
    s_con_tab.set_addr = c_set_addr;
    s_msg.thread_for = m_thread_for;
    s_msg.open_thread = m_open_thread;
    purr_app_env_t env;
    memset(&env, 0, sizeof(env));
    env.catcall_get = catcall_get;

    /* no radio: the service says so and does not start */
    radio_present = 0;
    CHECK_EQ(on_create(&env, NULL, 0), -1);
    CHECK(strstr(console_log, "no LoRa radio") != NULL);
    radio_present = 1;

    /* a first start: it registers, writes its defaults and takes the radio on US LongFast (906.875 MHz, SF11, 250 kHz, sync 0x24B4) */
    CHECK_EQ(on_create(&env, NULL, 0), 0);
    CHECK_EQ(service_calls, 1);
    CHECK(file_len[0] == (int)sizeof(config_t));
    CHECK_EQ(lease_calls, 1);
    CHECK(last_cfg.freq_hz == 906875000u && last_cfg.bw_hz == 250000 && last_cfg.sf == 11 && last_cfg.cr == 5 && last_cfg.sync_word == 0x24B4 && last_cfg.crc_on == 1 && last_cfg.preamble == 16);
    CHECK(last_cfg.tx_dbm == 22);
    CHECK_EQ(online_state, 1);
    CHECK_EQ(s_node.cfg.my_num, 0xA1B2C3D4u);
    CHECK(strcmp(s_cfg.long_name, "PURR c3d4") == 0 && strcmp(s_cfg.short_name, "c3d4") == 0);

    other_init();

    /* a text on the public channel arrives: Messages gets it on the channel's thread, with who said it */
    other_sends(PURR_MT_BROADCAST, "hello from afar");
    ticks(4);
    CHECK_EQ(in_n, 1);
    CHECK(strcmp(in_peer[0], "#LongFast") == 0 && strcmp(in_name[0], "LongFast") == 0);
    CHECK(strncmp(in_text[0], "!0000beef: hello from afar", 26) == 0);          /* the sender is not known by name yet: its number */

    /* a text typed in Messages for the channel goes out and the other device reads it */
    out_w = 1;
    outq[0].id = 501;
    strcpy(outq[0].peer, "#LongFast");
    strcpy(outq[0].text, "hi everyone");
    ticks(60);
    CHECK(air_sent_n >= 1);
    CHECK_EQ(deliv_n, 1);
    CHECK(deliv_id[0] == 501 && deliv_state[0] == PURR_MSGCC_SENT);
    purr_mt_event_t ev;
    air_to_other();
    int got = 0;
    while (purr_mt_node_event(&other, &ev)) {
        if (ev.kind == PURR_MT_EV_TEXT && strcmp(ev.text, "hi everyone") == 0 && ev.from == s_node.cfg.my_num && ev.to == PURR_MT_BROADCAST) {
            got = 1;
        }
    }
    CHECK(got);

    /* a direct message in: its own thread, named by the sender's node info; the app acknowledges it */
    in_n = 0;
    air_sent_n = 0;
    other_sends(s_node.cfg.my_num, "psst");
    ticks(100);
    CHECK_EQ(in_n, 1);
    CHECK(strcmp(in_peer[0], "!0000beef") == 0 && strcmp(in_text[0], "psst") == 0);
    CHECK(air_sent_n >= 1);                                                     /* the acknowledgement */
    air_to_other();

    /* a direct message out: delivered once the other device acknowledges it (it does, by itself, when it hears it) */
    deliv_n = 0;
    out_r = 0;
    out_w = 1;
    outq[0].id = 502;
    strcpy(outq[0].peer, "!0000beef");
    strcpy(outq[0].text, "got a minute?");
    air_sent_n = 0;
    ticks(80);
    CHECK(air_sent_n >= 1);
    air_to_other();
    for (int round = 0; round < 40; round++) {                                  /* the other device's acknowledgement goes back over the air */
        uint8_t f[255];
        size_t n;
        clock_ms += 20;
        if (purr_mt_node_tick(&other, clock_ms, f, sizeof(f), &n)) {
            memcpy(air_in[air_in_w % 16], f, n);
            air_in_len[air_in_w % 16] = (int)n;
            air_in_w++;
        }
        ticks(1);
    }
    int delivered = 0;
    for (int i = 0; i < deliv_n; i++) {
        delivered |= deliv_id[i] == 502 && deliv_state[i] == PURR_MSGCC_DELIVERED;
    }
    CHECK(delivered);

    /* a message for a channel or person that does not exist fails at once and says so */
    deliv_n = 0;
    out_r = 0;
    out_w = 1;
    outq[0].id = 503;
    strcpy(outq[0].peer, "#Nope");
    strcpy(outq[0].text, "x");
    ticks(2);
    CHECK(deliv_n == 1 && deliv_id[0] == 503 && deliv_state[0] == PURR_MSGCC_FAILED);

    /* the user switches services: the radio is handed back and the service says it is offline */
    purr_app_event_t y;
    memset(&y, 0, sizeof(y));
    y.type = PURR_APPEV_USER;
    y.name = "radio.yield";
    on_event(&y);
    CHECK_EQ(release_calls, 1);
    CHECK_EQ(online_state, 0);
    int leases = lease_calls;
    ticks(100);                                                                 /* not grabbed back at once */
    CHECK_EQ(lease_calls, leases);

    /* joining a private group from a share link: the channel is added next to LongFast, and a text on it is read */
    on_destroy(0);
    {
        purr_mt_url_t u;
        memset(&u, 0, sizeof(u));
        strcpy(u.ch[0].name, "Cats");
        for (int i = 0; i < 16; i++) u.ch[0].psk[i] = (uint8_t)(0x40 + i);
        u.ch[0].psk_len = 16;
        u.count = 1;
        char link[300];
        CHECK(purr_mt_url_make(&u, link, sizeof(link)) > 0);
        CHECK_EQ(apply_url(link), 1);
        CHECK_EQ(apply_url("not a link"), -1);
        CHECK(s_cfg.ch[0].role == 1 && strcmp(s_cfg.ch[0].name, "LongFast") == 0);   /* the primary is untouched */
        CHECK(s_cfg.ch[1].role == 2 && strcmp(s_cfg.ch[1].name, "Cats") == 0 && s_cfg.ch[1].keylen == 16);
        save_config();
        lease_rc = 0;
        CHECK_EQ(on_create(&env, NULL, 0), 0);
        CHECK(channel_by_name("Cats") == 1);
        purr_mt_channel_set(&other, 1, "Cats", u.ch[0].psk, 16, 2);
        in_n = 0;
        purr_mt_node_send_text(&other, clock_ms, PURR_MT_BROADCAST, 1, "private hello");
        for (int i = 0; i < 400; i++) {
            uint8_t f[255];
            size_t fl;
            clock_ms += 10;
            if (purr_mt_node_tick(&other, clock_ms, f, sizeof(f), &fl)) {
                memcpy(air_in[air_in_w % 16], f, fl);
                air_in_len[air_in_w % 16] = (int)fl;
                air_in_w++;
                break;
            }
        }
        ticks(6);
        CHECK(in_n == 1 && strcmp(in_peer[0], "#Cats") == 0 && strstr(in_text[0], "private hello") != NULL);
    }

    /* the app's own screens: home, nodes, a node's thread, settings (the region changes the radio), joining from a link, back */
    CHECK_EQ(on_create(&env, NULL, 0), 0);
    CHECK(strcmp(ui_title, "Meshtastic") == 0 && ui_buttons_made == 5);
    purr_mt_node_announce(&other, clock_ms);                            /* the other device says who it is, so there is a node to list */
    for (int i = 0; i < 400; i++) {
        uint8_t f[255];
        size_t fl;
        clock_ms += 10;
        if (purr_mt_node_tick(&other, clock_ms, f, sizeof(f), &fl)) {
            memcpy(air_in[air_in_w % 16], f, fl);
            air_in_len[air_in_w % 16] = (int)fl;
            air_in_w++;
            break;
        }
    }
    ticks(6);
    CHECK(purr_mt_node_find(&s_node, 0xBEEF) != NULL && strcmp(purr_mt_node_find(&s_node, 0xBEEF)->long_name, "Other Device") == 0);
    show_home();
    int b_nodes = s_h_btn[0];
    purr_app_event_t c;
    memset(&c, 0, sizeof(c));
    c.type = PURR_APPEV_UI;
    c.name = "click";
    c.arg = b_nodes;
    on_event(&c);
    CHECK(strcmp(ui_title, "Nodes") == 0 && ui_rows_made >= 1);
    purr_app_event_t bk;
    memset(&bk, 0, sizeof(bk));
    bk.type = PURR_APPEV_UI;
    bk.name = "back";
    c.arg = s_h_row[0];
    on_event(&c);                                                       /* a node: its page */
    CHECK(strcmp(ui_title, "OTHR") == 0 && thread_for_calls == 0);
    c.arg = s_h_btn[0];
    on_event(&c);                                                       /* Message: its thread opens in Messages */
    CHECK(thread_for_calls == 1 && open_thread_calls == 1);
    c.arg = s_h_btn[1];
    on_event(&c);                                                       /* Save to contacts */
    CHECK(contacts_added == 1 && addr_set == 1);
    c.arg = s_h_btn[1];
    on_event(&c);                                                       /* already there: nothing added twice */
    CHECK(contacts_added == 1);
    on_event(&bk);
    CHECK(strcmp(ui_title, "Nodes") == 0);
    on_event(&bk);
    CHECK(strcmp(ui_title, "Meshtastic") == 0);
    c.arg = s_h_btn[1];
    on_event(&c);
    CHECK(strcmp(ui_title, "Channels") == 0 && ui_rows_made >= 1);
    c.arg = s_h_row[0];
    on_event(&c);                                                       /* a channel: its share link */
    CHECK(strcmp(ui_title, "LongFast") == 0);
    int has_link = 0;
    for (int i = 0; i < ui_label_n; i++) has_link |= strncmp(ui_labels[i], "https://meshtastic.org/e/#", 26) == 0;
    CHECK(has_link);
    on_event(&bk);
    on_event(&bk);
    c.arg = s_h_btn[3];
    on_event(&c);
    CHECK(strcmp(ui_title, "Settings") == 0);
    uint32_t f0 = last_cfg.freq_hz;
    c.arg = settings_row[1];                                            /* region: the next one on the list */
    on_event(&c);
    CHECK(strcmp(s_cfg.region, "US") != 0 && last_cfg.freq_hz != f0);   /* and the radio was asked for again with the new frequency */
    for (int i = 0; i < purr_mt_region_count() - 1; i++) { c.arg = settings_row[1]; on_event(&c); }
    CHECK(strcmp(s_cfg.region, "US") == 0 && last_cfg.freq_hz == f0);   /* round the list and back */
    c.arg = settings_row[3];
    uint8_t hl = s_cfg.hop_limit;
    on_event(&c);
    CHECK(s_cfg.hop_limit == hl + 1 && s_node.cfg.hop_limit == hl + 1);
    c.arg = settings_row[5];
    on_event(&c);
    CHECK(s_cfg.pkc_send == 1);
    on_event(&bk);
    c.arg = s_h_btn[2];
    on_event(&c);
    CHECK(strcmp(ui_title, "Join a group") == 0);
    {
        purr_mt_url_t u;
        memset(&u, 0, sizeof(u));
        strcpy(u.ch[0].name, "Birds");
        memset(u.ch[0].psk, 0x33, 16);
        u.ch[0].psk_len = 16;
        u.count = 1;
        purr_mt_url_make(&u, ui_input, sizeof(ui_input));
        c.arg = s_h_btn[0];
        on_event(&c);
        CHECK(strcmp(ui_title, "Channels") == 0 && channel_by_name("Birds") >= 0);
        strcpy(ui_input, "garbage");
        c.arg = s_h_btn[1];
        on_event(&bk);
        c.arg = s_h_btn[2];
    }
    CHECK(closed == 0 && went_home == 0);
    on_event(&bk);                                                      /* Back from the first page steps to the start screen and leaves the service running */
    CHECK(closed == 0 && went_home == 1);
    c.arg = s_h_btn[4];
    on_event(&c);                                                       /* "Stop Meshtastic" is how it is switched off */
    CHECK(closed == 1);

    /* the node list is kept in a file and comes back after a restart, with how long ago each was heard */
    CHECK(file_len[1] > 8);
    int known = purr_mt_node_count(&s_node);
    on_destroy(0);
    CHECK(file_len[1] > 8);
    lease_rc = 0;
    CHECK_EQ(on_create(&env, NULL, 0), 0);
    CHECK_EQ(purr_mt_node_count(&s_node), known);
    CHECK(purr_mt_node_find(&s_node, 0xBEEF) != NULL && purr_mt_node_find(&s_node, 0xBEEF)->long_name[0] != '\0');

    /* a busy radio at the start is retried */
    on_destroy(0);
    lease_rc = PURR_RADIO_BUSY;
    leases = lease_calls = 0;
    CHECK_EQ(on_create(&env, NULL, 0), 0);
    CHECK_EQ(online_state, 0);
    lease_rc = 0;
    ticks(300);                                                                 /* more than five seconds of ticks */
    CHECK(lease_calls >= 2);
    CHECK_EQ(online_state, 1);

    TK_DONE("test_mt_app");
}
