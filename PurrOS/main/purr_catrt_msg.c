/*
 * purr_catrt_msg.c - the `messaging` and `contacts` catcalls (purr_catcalls.h; Messaging/SPEC.md).
 *
 * Both work on the signed-in user's own files: /home/<user>/messages/ (one file per thread) and /home/<user>/contacts.txt. They are opened the first time an app asks after a
 * sign-in and again when the user changes. Every call needs a permission ("messaging" or "contacts") and answers ASK/DENIED like the other gated catcalls. One mutex covers the
 * store, the address book and the service registry: callers are app tasks, and nothing in here waits for an app.
 *
 * A messaging SERVICE app registers itself (its name is the app's); a FRONT END (Messages) lists services and threads, and sends. The OS wakes a service with the event "msg.out" and a
 * front end with "msg.changed" (arg = the thread). A service that stops disappears from the list.
 */
#include "purr_catrt_msg.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "commands.h"
#include "login.h"
#include "purr_catcalls.h"
#include "purr_catrt.h"
#include "purr_catrt_power.h"
#include "purr_contacts.h"
#include "purr_fs.h"
#include "purr_msgstore.h"
#include "purr_perm.h"

static const char *TAG = "msg";

#define MAX_SVC   4
#define MAX_SUBS  4
#define QUEUE_MAX 16
#define SCRATCH_BYTES 12288
#define BOOK_TEXT_MAX 16384

typedef struct {
    int used, id;
    char app[32];
    char key[16], name[24], tag[8];
    uint16_t text_max;
    uint8_t images, radio, online;
    uint32_t image_max;
    uint32_t q[QUEUE_MAX];
    int nq;
} svc_t;

static SemaphoreHandle_t s_mx;
static svc_t s_svc[MAX_SVC];
static int s_next_svc = 1;
static char s_subs[MAX_SUBS][32];
static int s_nsubs;
static uint32_t s_open_req;

static purr_msgstore_t *s_store;
static uint8_t *s_scratch;
static char s_store_user[32];
static purr_contacts_t *s_book;
static char s_book_user[32];

__attribute__((weak)) void purr_msg_notify(const char *title, const char *text) { (void)title; (void)text; }

static void lock(void) { if (s_mx) xSemaphoreTake(s_mx, portMAX_DELAY); }
static void unlock(void) { if (s_mx) xSemaphoreGive(s_mx); }

static void copy(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static uint32_t now_s(void)
{
    time_t t = time(NULL);
    return t > 1000000000 ? (uint32_t)t : 0;                 /* 0 while the clock has not been set */
}

/* ---------------------------------------------------------------- the file system, as the store wants it */

static purr_fs_t *fsys(void) { return purr_login_fs(); }

static int io_read(void *ctx, const char *path, uint32_t off, void *buf, size_t cap, size_t *len, uint32_t *total)
{
    (void)ctx;
    uint32_t got = 0;
    int e = purr_fs_read_at(fsys(), path, off, buf, (uint32_t)cap, &got, total);
    *len = got;
    return e;
}
static int io_append(void *ctx, const char *path, const void *d, size_t n) { (void)ctx; return purr_fs_append(fsys(), path, d, (uint32_t)n); }
static int io_write(void *ctx, const char *path, const void *d, size_t n) { (void)ctx; return purr_fs_write(fsys(), path, d, (uint32_t)n); }
static int io_remove(void *ctx, const char *path) { (void)ctx; return purr_fs_remove(fsys(), path); }

typedef struct { void (*cb)(void *, const char *, uint32_t); void *c2; } list_adapt_t;
static void list_thunk(void *ctx, const char *name, int is_dir, uint32_t size)
{
    list_adapt_t *a = ctx;
    if (!is_dir) {
        a->cb(a->c2, name, size);
    }
}
static int io_list(void *ctx, const char *dir, void (*cb)(void *, const char *, uint32_t), void *c2)
{
    (void)ctx;
    list_adapt_t a = {cb, c2};
    return purr_fs_list(fsys(), dir, list_thunk, &a);
}

static const purr_msgio_t s_io = {NULL, io_read, io_append, io_write, io_remove, io_list};

typedef struct { char *buf; uint32_t cap, len; } acc_t;
static int acc_cb(void *ctx, const void *data, uint32_t len)
{
    acc_t *a = ctx;
    if (a->len + len > a->cap) {
        len = a->cap - a->len;
    }
    memcpy(a->buf + a->len, data, len);
    a->len += len;
    return 0;
}

/* The store for the signed-in user, opened on first use; NULL if nobody is signed in or there is no memory. Call with the mutex held. */
static purr_msgstore_t *store(void)
{
    const char *user = purr_login_current()->name;
    if (user[0] == '\0') {
        return NULL;
    }
    if (s_store != NULL && strcmp(s_store_user, user) == 0) {
        return s_store;
    }
    if (s_store == NULL) {
        s_store = heap_caps_calloc(1, sizeof(*s_store), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_scratch = heap_caps_malloc(SCRATCH_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_store == NULL || s_scratch == NULL) {
            free(s_store);
            free(s_scratch);
            s_store = NULL;
            s_scratch = NULL;
            return NULL;
        }
    }
    char dir[48], limf[56];
    snprintf(dir, sizeof(dir), "/home/%.24s/messages", user);
    snprintf(limf, sizeof(limf), "/home/%.24s/messages.limit", user);
    purr_fs_mkdir(fsys(), dir);
    uint32_t limit = 0;
    char lt[16];
    acc_t a = {lt, sizeof(lt) - 1, 0};
    if (purr_fs_read(fsys(), limf, acc_cb, &a) >= 0 && a.len > 0) {
        lt[a.len] = '\0';
        limit = (uint32_t)strtoul(lt, NULL, 10);
    }
    int n = purr_msgstore_open(s_store, &s_io, dir, limit, s_scratch, SCRATCH_BYTES);
    ESP_LOGI(TAG, "messages of %s: %d thread(s), %u bytes", user, n, (unsigned)purr_msgstore_used(s_store));
    copy(s_store_user, sizeof(s_store_user), user);
    return s_store;
}

static purr_contacts_t *book(void)
{
    const char *user = purr_login_current()->name;
    if (user[0] == '\0') {
        return NULL;
    }
    if (s_book != NULL && strcmp(s_book_user, user) == 0) {
        return s_book;
    }
    if (s_book == NULL) {
        s_book = heap_caps_calloc(1, sizeof(*s_book), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_book == NULL) {
            return NULL;
        }
    }
    char path[56];
    snprintf(path, sizeof(path), "/home/%.24s/contacts.txt", user);
    char *text = heap_caps_malloc(BOOK_TEXT_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    purr_ct_init(s_book);
    if (text != NULL) {
        acc_t a = {text, BOOK_TEXT_MAX, 0};
        if (purr_fs_read(fsys(), path, acc_cb, &a) >= 0) {
            text[a.len] = '\0';
            purr_ct_parse(s_book, text);
        }
        free(text);
    }
    copy(s_book_user, sizeof(s_book_user), user);
    return s_book;
}

static void book_save(void)
{
    char path[56];
    snprintf(path, sizeof(path), "/home/%.24s/contacts.txt", s_book_user);
    char *text = heap_caps_malloc(BOOK_TEXT_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (text != NULL) {
        purr_ct_format(s_book, text, BOOK_TEXT_MAX);
        purr_fs_write(fsys(), path, text, (uint32_t)strlen(text));
        free(text);
    }
}

/* ---------------------------------------------------------------- the registry and the events */

static int caller_app(char *name, size_t cap) { return purr_catrt_caller(name, cap); }

static svc_t *svc_by_app(const char *app)
{
    for (int i = 0; i < MAX_SVC; i++) {
        if (s_svc[i].used && strcmp(s_svc[i].app, app) == 0 && purr_catrt_slot_of(app) >= 0) {
            return &s_svc[i];
        }
    }
    return NULL;
}

static svc_t *svc_by_key(const char *key)
{
    for (int i = 0; key && i < MAX_SVC; i++) {
        if (s_svc[i].used && strcmp(s_svc[i].key, key) == 0 && purr_catrt_slot_of(s_svc[i].app) >= 0) {
            return &s_svc[i];
        }
    }
    return NULL;
}

static void notify_front_ends(uint32_t thread)
{
    for (int i = 0; i < s_nsubs; i++) {
        int slot = purr_catrt_slot_of(s_subs[i]);
        if (slot >= 0) {
            purr_catrt_post_event(slot, PURR_APPEV_USER, (int)thread, "msg.changed");
        } else {
            memmove(s_subs[i], s_subs[s_nsubs - 1], sizeof(s_subs[0]));
            s_nsubs--;
            i--;
        }
    }
}

static int gate_msg(void) { return purr_catrt_perm_gate(PURR_PERM_MESSAGING); }
static int gate_ct(void) { return purr_catrt_perm_gate(PURR_PERM_CONTACTS); }

/* ---------------------------------------------------------------- the messaging catcall: for a service app */

static int cc_service_register(const char *key, const char *name, const char *tag, int text_max, int images, uint32_t image_max, int radio)
{
    int g = gate_msg();
    char app[32];
    if (g != 0) {
        return g;
    }
    if (caller_app(app, sizeof(app)) < 0 || key == NULL || key[0] == '\0' || strlen(key) >= sizeof(s_svc[0].key) || name == NULL || text_max < 1) {
        return PURR_MSGCC_BAD;
    }
    for (const char *k = key; *k; k++) {
        if (!((*k >= 'a' && *k <= 'z') || (*k >= '0' && *k <= '9'))) {
            return PURR_MSGCC_BAD;
        }
    }
    lock();
    svc_t *s = NULL;
    for (int i = 0; i < MAX_SVC; i++) {
        if (s_svc[i].used && (strcmp(s_svc[i].key, key) == 0 || strcmp(s_svc[i].app, app) == 0)) {
            s = &s_svc[i];
            break;
        }
    }
    for (int i = 0; s == NULL && i < MAX_SVC; i++) {
        if (!s_svc[i].used || purr_catrt_slot_of(s_svc[i].app) < 0) {     /* a free place, or one whose app has stopped */
            s = &s_svc[i];
        }
    }
    int id = PURR_MSGCC_BAD;
    if (s != NULL) {
        memset(s, 0, sizeof(*s));
        s->used = 1;
        s->id = s_next_svc++;
        copy(s->app, sizeof(s->app), app);
        copy(s->key, sizeof(s->key), key);
        copy(s->name, sizeof(s->name), name);
        copy(s->tag, sizeof(s->tag), tag ? tag : "");
        s->text_max = (uint16_t)(text_max > PURR_MSG_TEXT_MAX ? PURR_MSG_TEXT_MAX : text_max);
        s->images = (uint8_t)images;
        s->image_max = image_max;
        s->radio = radio ? 1 : 0;
        id = s->id;
        notify_front_ends(0);
    }
    unlock();
    return id;
}

static void cc_service_online(int online)
{
    char app[32];
    if (gate_msg() != 0 || caller_app(app, sizeof(app)) < 0) {
        return;
    }
    lock();
    svc_t *s = svc_by_app(app);
    if (s != NULL) {
        s->online = online ? 1 : 0;
        notify_front_ends(0);
    }
    unlock();
}

static int cc_next_outgoing(purr_cc_msg_t *out, char *peer, size_t peer_cap)
{
    char app[32];
    int g = gate_msg();
    if (g != 0) {
        return g;
    }
    if (out == NULL || caller_app(app, sizeof(app)) < 0) {
        return PURR_MSGCC_BAD;
    }
    int got = 0;
    lock();
    svc_t *s = svc_by_app(app);
    purr_msgstore_t *st = store();
    while (s != NULL && st != NULL && s->nq > 0 && !got) {
        uint32_t id = s->q[0];
        memmove(&s->q[0], &s->q[1], (size_t)(s->nq - 1) * sizeof(s->q[0]));
        s->nq--;
        purr_msg_t m;
        if (purr_msgstore_message(st, id, &m) == 0) {
            const purr_msgthread_t *t = purr_msgstore_thread_by_id(st, id >> 20);
            purr_msgstore_set_state(st, id, PURR_MSGS_SENDING);
            memset(out, 0, sizeof(*out));
            out->id = m.id;
            out->dir = m.dir;
            out->state = PURR_MSGS_SENDING;
            out->time = m.time;
            copy(out->attach, sizeof(out->attach), m.attach);
            copy(out->text, sizeof(out->text), m.text);
            if (peer != NULL && peer_cap > 0) {
                copy(peer, peer_cap, t != NULL ? t->peer : "");
            }
            notify_front_ends(id >> 20);
            got = 1;
        }
    }
    unlock();
    return got;
}

static int cc_delivery(uint32_t msg_id, int state)
{
    char app[32];
    int g = gate_msg();
    if (g != 0) {
        return g;
    }
    if (state < PURR_MSGS_SENT || state > PURR_MSGS_FAILED || caller_app(app, sizeof(app)) < 0) {
        return PURR_MSGCC_BAD;
    }
    lock();
    purr_msgstore_t *st = store();
    int rc = st != NULL && svc_by_app(app) != NULL ? purr_msgstore_set_state(st, msg_id, state) : PURR_MSGCC_BAD;
    if (rc == 0) {
        notify_front_ends(msg_id >> 20);
    }
    unlock();
    return rc == 0 ? PURR_MSGCC_OK : rc == PURR_MSG_FULL ? PURR_MSGCC_FULL : PURR_MSGCC_BAD;
}

static int cc_incoming(const char *peer, const char *peer_name, const char *text, const char *attach)
{
    char app[32];
    int g = gate_msg();
    if (g != 0) {
        return g;
    }
    if (peer == NULL || text == NULL || caller_app(app, sizeof(app)) < 0) {
        return PURR_MSGCC_BAD;
    }
    char title[32] = "", shown[PURR_MSG_TEXT_MAX + 1];
    int rc = PURR_MSGCC_BAD;
    lock();
    svc_t *s = svc_by_app(app);
    purr_msgstore_t *st = store();
    if (s != NULL && st != NULL) {
        int32_t th = purr_msgstore_thread(st, s->key, peer, peer_name);
        if (th > 0 && purr_msgstore_append(st, (uint32_t)th, PURR_MSG_IN, PURR_MSGS_RECEIVED, now_s(), attach ? attach : "", text, NULL) == 0) {
            rc = PURR_MSGCC_OK;
            const purr_msgthread_t *t = purr_msgstore_thread_by_id(st, (uint32_t)th);
            purr_contacts_t *b = book();
            uint32_t cid = b != NULL ? purr_ct_find_addr(b, s->key, peer) : 0;
            const purr_contact_t *c = cid ? purr_ct_get(b, cid) : NULL;
            copy(title, sizeof(title), c != NULL ? c->name : (t != NULL && t->name[0] ? t->name : peer));
            copy(shown, sizeof(shown), text[0] ? text : "Picture");
            notify_front_ends((uint32_t)th);
        } else if (th > 0) {
            rc = PURR_MSGCC_FULL;
        }
    }
    unlock();
    if (rc == PURR_MSGCC_OK) {
        purr_msg_notify(title, shown);
    }
    return rc;
}

/* ---------------------------------------------------------------- the messaging catcall: for a front end */

static int cc_services(int i, purr_cc_msg_service_t *out)
{
    int g = gate_msg();
    if (g != 0) {
        return g;
    }
    int n = 0, found = 0;
    lock();
    for (int k = 0; k < MAX_SVC && out != NULL; k++) {
        if (s_svc[k].used && purr_catrt_slot_of(s_svc[k].app) >= 0) {
            if (n == i) {
                memset(out, 0, sizeof(*out));
                out->id = s_svc[k].id;
                copy(out->key, sizeof(out->key), s_svc[k].key);
                copy(out->name, sizeof(out->name), s_svc[k].name);
                copy(out->tag, sizeof(out->tag), s_svc[k].tag);
                out->text_max = s_svc[k].text_max;
                out->images = s_svc[k].images;
                out->radio = s_svc[k].radio;
                out->online = s_svc[k].online;
                out->image_max = s_svc[k].image_max;
                found = 1;
                break;
            }
            n++;
        }
    }
    unlock();
    return found;
}

static int cc_threads(int i, purr_cc_msg_thread_t *out)
{
    int g = gate_msg();
    if (g != 0) {
        return g;
    }
    int found = 0;
    lock();
    purr_msgstore_t *st = store();
    uint32_t ids[PURR_MSG_THREADS_MAX];
    if (st != NULL && out != NULL && i >= 0) {
        int n = purr_msgstore_order(st, ids, PURR_MSG_THREADS_MAX);
        if (i < n) {
            const purr_msgthread_t *t = purr_msgstore_thread_by_id(st, ids[i]);
            memset(out, 0, sizeof(*out));
            out->id = t->id;
            copy(out->service, sizeof(out->service), t->service);
            copy(out->peer, sizeof(out->peer), t->peer);
            copy(out->name, sizeof(out->name), t->name);
            purr_contacts_t *b = book();
            uint32_t cid = b != NULL ? purr_ct_find_addr(b, t->service, t->peer) : 0;
            if (cid != 0) {                                    /* the address book's name wins */
                copy(out->name, sizeof(out->name), purr_ct_get(b, cid)->name);
            }
            out->last_time = t->last_time;
            out->unread = t->unread;
            out->last_dir = t->last_dir;
            out->last_state = t->last_state;
            copy(out->last, sizeof(out->last), t->last);
            found = 1;
        }
    }
    unlock();
    return found;
}

static int cc_messages(uint32_t thread, int skip, purr_cc_msg_t *out, int max)
{
    int g = gate_msg();
    if (g != 0) {
        return g;
    }
    int n = 0;
    lock();
    purr_msgstore_t *st = store();
    if (st != NULL && out != NULL && max > 0 && max <= 16) {
        purr_msg_t *tmp = heap_caps_malloc(sizeof(purr_msg_t) * (size_t)max, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (tmp != NULL) {
            n = purr_msgstore_messages(st, thread, skip, tmp, max);
            for (int k = 0; k < n; k++) {
                out[k].id = tmp[k].id;
                out[k].dir = tmp[k].dir;
                out[k].state = tmp[k].state;
                out[k].time = tmp[k].time;
                copy(out[k].attach, sizeof(out[k].attach), tmp[k].attach);
                copy(out[k].text, sizeof(out[k].text), tmp[k].text);
            }
            free(tmp);
        }
    }
    unlock();
    return n;
}

static int cc_thread_for(const char *service, const char *peer, const char *peer_name)
{
    int g = gate_msg();
    if (g != 0) {
        return g;
    }
    lock();
    purr_msgstore_t *st = store();
    int32_t th = st != NULL ? purr_msgstore_thread(st, service, peer, peer_name) : PURR_MSGCC_BAD;
    unlock();
    return th == PURR_MSG_FULL ? PURR_MSGCC_FULL : th < 0 ? PURR_MSGCC_BAD : th;
}

static int cc_send(const char *service, const char *peer, const char *peer_name, const char *text, const char *attach, uint32_t *msg_id)
{
    int g = gate_msg();
    if (g != 0) {
        return g;
    }
    if (service == NULL || peer == NULL || text == NULL || (text[0] == '\0' && (attach == NULL || attach[0] == '\0'))) {
        return PURR_MSGCC_BAD;
    }
    int rc = PURR_MSGCC_BAD;
    lock();
    svc_t *s = svc_by_key(service);
    purr_msgstore_t *st = store();
    if (s == NULL) {
        rc = PURR_MSGCC_NOSERVICE;
    } else if (st != NULL && strlen(text) <= s->text_max && s->nq < QUEUE_MAX) {
        int32_t th = purr_msgstore_thread(st, service, peer, peer_name);
        uint32_t id = 0;
        if (th > 0) {
            int a = purr_msgstore_append(st, (uint32_t)th, PURR_MSG_OUT, PURR_MSGS_QUEUED, now_s(), attach ? attach : "", text, &id);
            if (a == 0) {
                s->q[s->nq++] = id;
                if (msg_id) {
                    *msg_id = id;
                }
                int slot = purr_catrt_slot_of(s->app);
                if (slot >= 0) {
                    purr_catrt_post_event(slot, PURR_APPEV_USER, (int)id, "msg.out");
                }
                notify_front_ends((uint32_t)th);
                rc = PURR_MSGCC_OK;
            } else if (a == PURR_MSG_FULL) {
                rc = PURR_MSGCC_FULL;
            }
        } else if (th == PURR_MSG_FULL) {
            rc = PURR_MSGCC_FULL;
        }
    }
    unlock();
    return rc;
}

static int cc_mark_read(uint32_t thread)
{
    int g = gate_msg();
    if (g != 0) {
        return g;
    }
    lock();
    purr_msgstore_t *st = store();
    int rc = st != NULL ? purr_msgstore_mark_read(st, thread, now_s()) : PURR_MSG_ERR;
    unlock();
    return rc == 0 ? PURR_MSGCC_OK : PURR_MSGCC_BAD;
}

static int cc_remove_thread(uint32_t thread)
{
    int g = gate_msg();
    if (g != 0) {
        return g;
    }
    lock();
    purr_msgstore_t *st = store();
    int rc = st != NULL ? purr_msgstore_delete_thread(st, thread) : PURR_MSG_ERR;
    if (rc == 0) {
        notify_front_ends(0);
    }
    unlock();
    return rc == 0 ? PURR_MSGCC_OK : PURR_MSGCC_BAD;
}

static int cc_subscribe(void)
{
    char app[32];
    int g = gate_msg();
    if (g != 0) {
        return g;
    }
    if (caller_app(app, sizeof(app)) < 0) {
        return PURR_MSGCC_BAD;
    }
    lock();
    int have = 0;
    for (int i = 0; i < s_nsubs; i++) {
        have |= strcmp(s_subs[i], app) == 0;
    }
    if (!have && s_nsubs < MAX_SUBS) {
        copy(s_subs[s_nsubs++], sizeof(s_subs[0]), app);
    }
    unlock();
    return PURR_MSGCC_OK;
}

static int cc_usage(uint32_t *used, uint32_t *limit)
{
    int g = gate_msg();
    if (g != 0) {
        return g;
    }
    lock();
    purr_msgstore_t *st = store();
    if (used) *used = st ? purr_msgstore_used(st) : 0;
    if (limit) *limit = st ? purr_msgstore_limit(st) : PURR_MSG_LIMIT_DEFAULT;
    unlock();
    return PURR_MSGCC_OK;
}

static int cc_set_limit(uint32_t bytes)
{
    int g = gate_msg();
    if (g != 0) {
        return g;
    }
    if (bytes < 65536 || bytes > 4u * 1024u * 1024u) {
        return PURR_MSGCC_BAD;
    }
    lock();
    purr_msgstore_t *st = store();
    if (st != NULL) {
        purr_msgstore_set_limit(st, bytes);
        char f[56], t[16];
        snprintf(f, sizeof(f), "/home/%.24s/messages.limit", s_store_user);
        snprintf(t, sizeof(t), "%u", (unsigned)bytes);
        purr_fs_write(fsys(), f, t, (uint32_t)strlen(t));
    }
    unlock();
    return PURR_MSGCC_OK;
}

/* A path an app may export to: the user's own folder, or the SD card. */
static int export_path_ok(const char *p)
{
    char home[40];
    snprintf(home, sizeof(home), "/home/%.24s/", purr_login_current()->name);
    return p != NULL && strstr(p, "..") == NULL && strlen(p) < 96 && (strncmp(p, home, strlen(home)) == 0 || strncmp(p, "/sd/", 4) == 0);
}

static int cc_export_all(const char *path)
{
    int g = gate_msg();
    if (g != 0) {
        return g;
    }
    if (!export_path_ok(path)) {
        return PURR_MSGCC_BAD;
    }
    lock();
    purr_msgstore_t *st = store();
    int rc = st != NULL ? purr_msgstore_export(st, path) : PURR_MSG_ERR;
    unlock();
    return rc == 0 ? PURR_MSGCC_OK : PURR_MSGCC_BAD;
}

static int cc_trim(uint32_t target)
{
    int g = gate_msg();
    if (g != 0) {
        return g;
    }
    lock();
    purr_msgstore_t *st = store();
    uint32_t freed = st != NULL ? purr_msgstore_trim(st, target) : 0;
    if (freed > 0) {
        notify_front_ends(0);
    }
    unlock();
    return (int)(freed > 0x7fffffff ? 0x7fffffff : freed);
}

static void open_task(void *arg)
{
    (void)arg;
    char out[96];
    if (purr_catrt_dispatch("open messages", out, sizeof(out)) < 0) {
        purr_catrt_dispatch("focus messages", out, sizeof(out));
    }
    vTaskDelete(NULL);
}

static int cc_open_thread(uint32_t thread)
{
    int g = gate_msg();
    if (g != 0) {
        return g;
    }
    s_open_req = thread;
    int slot = purr_catrt_slot_of("messages");
    if (slot >= 0) {
        purr_catrt_post_event(slot, PURR_APPEV_USER, (int)thread, "msg.open");
    }
    if (xTaskCreate(open_task, "msg-open", 4096, NULL, 4, NULL) != pdPASS) {
        return PURR_MSGCC_BAD;
    }
    return PURR_MSGCC_OK;
}

static uint32_t cc_take_open_request(void)
{
    uint32_t t = s_open_req;
    s_open_req = 0;
    return t;
}

static const purr_cc_messaging_t s_cc_msg = {
    .version = 1,
    .service_register = cc_service_register, .service_online = cc_service_online, .next_outgoing = cc_next_outgoing, .delivery = cc_delivery, .incoming = cc_incoming,
    .services = cc_services, .threads = cc_threads, .messages = cc_messages, .send = cc_send, .mark_read = cc_mark_read, .remove_thread = cc_remove_thread,
    .subscribe = cc_subscribe, .usage = cc_usage, .set_limit = cc_set_limit, .export_all = cc_export_all, .trim = cc_trim,
    .open_thread = cc_open_thread, .take_open_request = cc_take_open_request, .thread_for = cc_thread_for,
};

/* ---------------------------------------------------------------- the contacts catcall */

static void fill_contact(purr_cc_contact_t *out, const purr_contact_t *c)
{
    memset(out, 0, sizeof(*out));
    out->id = c->id;
    copy(out->name, sizeof(out->name), c->name);
    out->fav = c->fav;
    out->naddr = c->naddr;
    for (int i = 0; i < c->naddr && i < 4; i++) {
        copy(out->a[i].service, sizeof(out->a[i].service), c->a[i].service);
        copy(out->a[i].addr, sizeof(out->a[i].addr), c->a[i].addr);
    }
}

static int ct_list(const char *filter, int i, purr_cc_contact_t *out)
{
    int g = gate_ct();
    if (g != 0) {
        return g;
    }
    int found = 0;
    lock();
    purr_contacts_t *b = book();
    uint32_t ids[PURR_CT_MAX];
    if (b != NULL && out != NULL && i >= 0) {
        int n = purr_ct_order(b, ids, PURR_CT_MAX, filter);
        if (i < n) {
            fill_contact(out, purr_ct_get(b, ids[i]));
            found = 1;
        }
    }
    unlock();
    return found;
}

static int ct_get(uint32_t id, purr_cc_contact_t *out)
{
    int g = gate_ct();
    if (g != 0) {
        return g;
    }
    int found = 0;
    lock();
    purr_contacts_t *b = book();
    const purr_contact_t *c = b != NULL && out != NULL ? purr_ct_get(b, id) : NULL;
    if (c != NULL) {
        fill_contact(out, c);
        found = 1;
    }
    unlock();
    return found;
}

static int ct_count(void)
{
    int g = gate_ct();
    if (g != 0) {
        return g;
    }
    lock();
    purr_contacts_t *b = book();
    int n = b != NULL ? b->n : 0;
    unlock();
    return n;
}

#define CT_CHANGE(call)                                                \
    do {                                                               \
        int g = gate_ct();                                             \
        if (g != 0) {                                                  \
            return g;                                                  \
        }                                                              \
        lock();                                                        \
        purr_contacts_t *b = book();                                   \
        int rc = b != NULL ? (call) : -1;                              \
        if (rc >= 0) {                                                 \
            book_save();                                               \
        }                                                              \
        unlock();                                                      \
        return rc;                                                     \
    } while (0)

static int ct_add(const char *name) { CT_CHANGE(purr_ct_add(b, name)); }
static int ct_rename(uint32_t id, const char *name) { CT_CHANGE(purr_ct_rename(b, id, name)); }
static int ct_set_fav(uint32_t id, int fav) { CT_CHANGE(purr_ct_set_fav(b, id, fav)); }
static int ct_set_addr(uint32_t id, const char *svc, const char *addr) { CT_CHANGE(purr_ct_set_addr(b, id, svc, addr)); }
static int ct_remove_addr(uint32_t id, const char *svc) { CT_CHANGE(purr_ct_remove_addr(b, id, svc)); }
static int ct_remove(uint32_t id) { CT_CHANGE(purr_ct_remove(b, id)); }

static uint32_t ct_find(const char *service, const char *addr)
{
    if (gate_ct() != 0) {
        return 0;
    }
    lock();
    purr_contacts_t *b = book();
    uint32_t id = b != NULL ? purr_ct_find_addr(b, service, addr) : 0;
    unlock();
    return id;
}

static const purr_cc_contacts_t s_cc_contacts = {
    .version = 1, .list = ct_list, .get = ct_get, .add = ct_add, .rename = ct_rename, .set_fav = ct_set_fav, .set_addr = ct_set_addr,
    .remove_addr = ct_remove_addr, .remove = ct_remove, .find = ct_find, .count = ct_count,
};

void purr_catrt_msg_init(void)
{
    if (s_mx == NULL) {
        s_mx = xSemaphoreCreateMutex();
    }
    purr_catrt_set_messaging(&s_cc_msg);
    purr_catrt_set_contacts(&s_cc_contacts);
}
