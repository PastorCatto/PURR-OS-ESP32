/*
 * msgecho.c - Echo, a test messaging service (Messaging/SPEC.md). It registers with the `messaging` catcall as the service "echo", takes every message sent to it, marks it sent
 * and delivered, and a moment later answers "Echo: <what you said>" from a peer called "echo-bot". It carries no pictures and needs no radio. It is also the smallest example of a service app:
 * register, wait for "msg.out", drain next_outgoing(), report delivery, hand incoming messages to the OS.
 *
 * Open it once so the permission question ("read and send your messages") can be answered; it keeps running in the background.
 */
#include <stddef.h>
#include <stdint.h>

#include "purr_app_abi.h"
#include "purr_catcalls.h"

#define NO_OPT __attribute__((noinline, optimize("no-tree-loop-distribute-patterns")))

#define REPLY_MS 1500
#define PENDING 4

static const purr_cc_ui_t *ui;
static const purr_cc_sched_t *sc;
static const purr_cc_messaging_t *mq;

static int s_registered;
static int h_status;
static int s_timer;
static char s_reply[PENDING][PURR_MSGCC_TEXT];
static int s_nreply;
static int s_handled;

#ifndef PURR_MSGCC_TEXT

#endif

NO_OPT static void put(char *dst, const char *src, int cap)
{
    int i = 0;
    while (src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

NO_OPT static void say(const char *t)
{
    if (ui != NULL && h_status > 0) {
        ui->set_text(h_status, t);
    }
}

NO_OPT static void try_register(void)
{
    if (s_registered || mq == NULL) {
        return;
    }
    int id = mq->service_register("echo", "Echo", "ECHO", 200, PURR_MSGCC_IMG_NONE, 0, 0);
    if (id > 0) {
        s_registered = 1;
        mq->service_online(1);
        say("Running. Send it a message from Messages.");
        mq->incoming("echo-bot", "Echo bot", "Hello. Send me anything and I will say it back.", NULL);
    } else if (id == PURR_MSGCC_ASK) {
        say("Waiting for permission. Answer the question on the screen.");
    } else if (id == PURR_MSGCC_DENIED) {
        say("Permission was not given. Allow it in Settings > Apps.");
    } else {
        say("Could not register.");
    }
}

NO_OPT static void drain(void)
{
    purr_cc_msg_t m;
    char peer[40];
    while (mq->next_outgoing(&m, peer, sizeof(peer)) == 1) {
        mq->delivery(m.id, PURR_MSGCC_SENT);
        mq->delivery(m.id, PURR_MSGCC_DELIVERED);
        s_handled++;
        if (s_nreply < PENDING) {
            char *r = s_reply[s_nreply++];
            put(r, "Echo: ", 8);
            int n = 6;
            for (int i = 0; m.text[i] && n < PURR_MSGCC_TEXT - 1; i++) {
                r[n++] = m.text[i];
            }
            r[n] = '\0';
        }
    }
    if (s_nreply > 0 && s_timer <= 0 && sc != NULL) {
        s_timer = sc->timer_start(REPLY_MS, 0);
    }
}

NO_OPT static void send_reply(void)
{
    s_timer = 0;
    for (int i = 0; i < s_nreply; i++) {
        mq->incoming("echo-bot", "Echo bot", s_reply[i], NULL);
    }
    s_nreply = 0;
}

static int on_create(const purr_app_env_t *env, const void *state, uint32_t len)
{
    (void)state;
    (void)len;
    if (env == NULL || env->catcall_get == NULL) {
        return -1;
    }
    ui = (const purr_cc_ui_t *)env->catcall_get("ui", 1);
    sc = (const purr_cc_sched_t *)env->catcall_get("sched", 1);
    mq = (const purr_cc_messaging_t *)env->catcall_get("messaging", 1);
    if (ui == NULL || mq == NULL) {
        return -1;
    }
    ui->win_title("Echo service");
    ui->label("A test messaging service", PURR_UI_ROLE_TITLE);
    ui->label("It answers every message you send to it. Leave it running, then open Messages.", PURR_UI_ROLE_DIM);
    h_status = ui->label("Starting...", PURR_UI_ROLE_TEXT);
    try_register();
    return 0;
}

static void on_event(const purr_app_event_t *ev)
{
    if (mq == NULL) {
        return;
    }
    if (ev->type == PURR_APPEV_PERM) {
        if (ev->arg == 1) {
            try_register();
        } else {
            say("Permission was not given. Allow it in Settings > Apps.");
        }
        return;
    }
    if (ev->type == PURR_APPEV_TIMER) {
        send_reply();
        return;
    }
    if (ev->type == PURR_APPEV_USER && ev->name != NULL && s_registered) {
        const char *n = ev->name;
        if (n[0] == 'm' && n[1] == 's' && n[2] == 'g' && n[3] == '.' && n[4] == 'o' && n[5] == 'u' && n[6] == 't' && n[7] == '\0') {
            drain();
        }
    }
}

static void on_destroy(int reason)
{
    (void)reason;
}

static const purr_app_table2_t s_table = {
    .base = {.abi_version = PURR_APP_ABI_VERSION_2, .on_create = on_create, .on_destroy = on_destroy},
    .on_event = on_event,
    .memory_needed = 8192, .kind = 2, .flags = 0x01 | 0x04 /* may keep running in the background; never chosen as an out-of-memory victim */,
};

__attribute__((used))
const purr_app_table_t *msgecho_entry(void)
{
    return &s_table.base;
}
