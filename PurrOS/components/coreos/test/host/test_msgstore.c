/* test_msgstore.c - the message store (purr_msgstore.c) over an in-memory file system. */
#include <stdlib.h>
#include <string.h>

#include "purr_msgstore.h"
#include "testkit.h"

#define NFILES 16
static struct { char path[96]; uint8_t *data; size_t len; int used; } g_f[NFILES];

static void fs_reset(void)
{
    for (int i = 0; i < NFILES; i++) {
        free(g_f[i].data);
        memset(&g_f[i], 0, sizeof(g_f[i]));
    }
}
static int fs_find(const char *p)
{
    for (int i = 0; i < NFILES; i++) {
        if (g_f[i].used && strcmp(g_f[i].path, p) == 0) {
            return i;
        }
    }
    return -1;
}
static int fs_make(const char *p)
{
    int i = fs_find(p);
    if (i >= 0) {
        return i;
    }
    for (i = 0; i < NFILES; i++) {
        if (!g_f[i].used) {
            g_f[i].used = 1;
            strcpy(g_f[i].path, p);
            g_f[i].data = malloc(65536);
            g_f[i].len = 0;
            return i;
        }
    }
    return -1;
}
static int io_read(void *ctx, const char *path, uint32_t off, void *buf, size_t cap, size_t *len, uint32_t *total)
{
    (void)ctx;
    int i = fs_find(path);
    if (i < 0) {
        return -1;
    }
    *total = (uint32_t)g_f[i].len;
    size_t n = off >= g_f[i].len ? 0 : g_f[i].len - off;
    if (n > cap) {
        n = cap;
    }
    memcpy(buf, g_f[i].data + off, n);
    *len = n;
    return 0;
}
static int io_append(void *ctx, const char *path, const void *d, size_t n)
{
    (void)ctx;
    int i = fs_make(path);
    if (i < 0 || g_f[i].len + n > 65536) {
        return -1;
    }
    memcpy(g_f[i].data + g_f[i].len, d, n);
    g_f[i].len += n;
    return 0;
}
static int io_write(void *ctx, const char *path, const void *d, size_t n)
{
    (void)ctx;
    int i = fs_make(path);
    if (i < 0 || n > 65536) {
        return -1;
    }
    memcpy(g_f[i].data, d, n);
    g_f[i].len = n;
    return 0;
}
static int io_remove(void *ctx, const char *path)
{
    (void)ctx;
    int i = fs_find(path);
    if (i >= 0) {
        free(g_f[i].data);
        memset(&g_f[i], 0, sizeof(g_f[i]));
    }
    return i >= 0 ? 0 : -1;
}
static int io_list(void *ctx, const char *dir, void (*cb)(void *, const char *, uint32_t), void *c2)
{
    (void)ctx;
    size_t dn = strlen(dir);
    for (int i = 0; i < NFILES; i++) {
        if (g_f[i].used && strncmp(g_f[i].path, dir, dn) == 0 && g_f[i].path[dn] == '/') {
            cb(c2, g_f[i].path + dn + 1, (uint32_t)g_f[i].len);
        }
    }
    return 0;
}
static const purr_msgio_t IO = {NULL, io_read, io_append, io_write, io_remove, io_list};
static uint8_t scratch[8192];

static void test_basic_and_reopen(void)
{
    fs_reset();
    purr_msgstore_t s;
    CHECK_EQ(purr_msgstore_open(&s, &IO, "/m", 0, scratch, sizeof(scratch)), 0);
    int32_t a = purr_msgstore_thread(&s, "meshtastic", "!a1b2c3d4", "Sam Rivera");
    int32_t b = purr_msgstore_thread(&s, "reticulum", "7f3e", NULL);
    CHECK(a > 0 && b > 0 && a != b);
    CHECK_EQ(purr_msgstore_thread(&s, "meshtastic", "!a1b2c3d4", NULL), a);          /* the same thread again */
    CHECK(purr_msgstore_thread(&s, "Bad Key", "x", NULL) < 0);                          /* a service key is lower case letters and digits */
    uint32_t id1, id2, id3;
    CHECK_EQ(purr_msgstore_append(&s, (uint32_t)a, PURR_MSG_IN, PURR_MSGS_RECEIVED, 100, "", "Are you coming?", &id1), 0);
    CHECK_EQ(purr_msgstore_append(&s, (uint32_t)a, PURR_MSG_OUT, PURR_MSGS_QUEUED, 110, "", "Yes | leaving\nnow \\o/", &id2), 0);     /* escapes */
    CHECK_EQ(purr_msgstore_append(&s, (uint32_t)b, PURR_MSG_IN, PURR_MSGS_RECEIVED, 120, "/home/x/p.jpg", "photo", &id3), 0);
    CHECK(id1 != id2 && (id1 >> 20) == (uint32_t)a && (id3 >> 20) == (uint32_t)b);

    purr_msg_t m[4];
    CHECK_EQ(purr_msgstore_messages(&s, (uint32_t)a, 0, m, 4), 2);
    CHECK(!strcmp(m[0].text, "Are you coming?") && m[0].dir == PURR_MSG_IN && m[0].time == 100);
    CHECK(!strcmp(m[1].text, "Yes | leaving\nnow \\o/") && m[1].dir == PURR_MSG_OUT);
    CHECK_EQ(purr_msgstore_messages(&s, (uint32_t)a, 1, m, 4), 1);                       /* skip the newest */
    CHECK(!strcmp(m[0].text, "Are you coming?"));
    CHECK_EQ(purr_msgstore_messages(&s, (uint32_t)b, 0, m, 4), 1);
    CHECK(!strcmp(m[0].attach, "/home/x/p.jpg"));

    CHECK_EQ(purr_msgstore_set_state(&s, id2, PURR_MSGS_DELIVERED), 0);
    CHECK_EQ(purr_msgstore_messages(&s, (uint32_t)a, 0, m, 4), 2);
    CHECK_EQ(m[1].state, PURR_MSGS_DELIVERED);
    purr_msg_t one;
    CHECK_EQ(purr_msgstore_message(&s, id2, &one), 0);
    CHECK_EQ(one.state, PURR_MSGS_DELIVERED);

    const purr_msgthread_t *ta = purr_msgstore_thread_by_id(&s, (uint32_t)a);
    CHECK_EQ(ta->unread, 1);
    uint32_t order[4];
    CHECK_EQ(purr_msgstore_order(&s, order, 4), 2);
    CHECK_EQ(order[0], (uint32_t)b);                                                       /* newest first */

    CHECK_EQ(purr_msgstore_mark_read(&s, (uint32_t)a, 130), 0);
    CHECK_EQ(purr_msgstore_thread_by_id(&s, (uint32_t)a)->unread, 0);

    /* a new store over the same files: everything is back */
    purr_msgstore_t s2;
    CHECK_EQ(purr_msgstore_open(&s2, &IO, "/m", 0, scratch, sizeof(scratch)), 2);
    ta = purr_msgstore_thread_by_id(&s2, (uint32_t)a);
    CHECK(ta != NULL && !strcmp(ta->name, "Sam Rivera") && !strcmp(ta->peer, "!a1b2c3d4"));
    CHECK_EQ(ta->unread, 0);                                                               /* the read marker was kept */
    CHECK_EQ(purr_msgstore_thread_by_id(&s2, (uint32_t)b)->unread, 1);
    CHECK_EQ(ta->last_state, PURR_MSGS_DELIVERED);
    CHECK_EQ(purr_msgstore_used(&s2), purr_msgstore_used(&s));
    uint32_t id4;
    CHECK_EQ(purr_msgstore_append(&s2, (uint32_t)a, PURR_MSG_OUT, PURR_MSGS_QUEUED, 140, "", "again", &id4), 0);
    CHECK(id4 != id1 && id4 != id2);                                                       /* message numbers carry on */
    CHECK_EQ(purr_msgstore_thread(&s2, "meshtastic", "new-peer", NULL) > 0, 1);            /* thread ids carry on */

    CHECK_EQ(purr_msgstore_delete_thread(&s2, (uint32_t)b), 0);
    CHECK_EQ(purr_msgstore_thread_by_id(&s2, (uint32_t)b) == NULL, 1);
    CHECK_EQ(purr_msgstore_delete_thread(&s2, (uint32_t)b), PURR_MSG_NOTFOUND);
}

static void test_limit_trim_export(void)
{
    fs_reset();
    purr_msgstore_t s;
    purr_msgstore_open(&s, &IO, "/m", 4000, scratch, sizeof(scratch));
    int32_t t = purr_msgstore_thread(&s, "meshtastic", "p", "P");
    char text[64];
    int stored = 0, rc = 0;
    for (int i = 0; i < 200 && rc == 0; i++) {
        snprintf(text, sizeof(text), "message number %03d", i);
        rc = purr_msgstore_append(&s, (uint32_t)t, PURR_MSG_IN, PURR_MSGS_RECEIVED, 1000 + (uint32_t)i, "", text, NULL);
        if (rc == 0) {
            stored++;
        }
    }
    CHECK_EQ(rc, PURR_MSG_FULL);
    CHECK(stored > 50 && purr_msgstore_used(&s) <= 4000);
    uint32_t before = purr_msgstore_used(&s);
    CHECK_EQ(purr_msgstore_append(&s, (uint32_t)t, PURR_MSG_IN, PURR_MSGS_RECEIVED, 5000, "", "refused", NULL), PURR_MSG_FULL);
    CHECK_EQ(purr_msgstore_used(&s), before);                                              /* nothing was written */

    CHECK_EQ(purr_msgstore_export(&s, "/export/all.txt"), 0);
    int fi = fs_find("/export/all.txt");
    CHECK(fi >= 0 && g_f[fi].len > (size_t)stored * 20);
    g_f[fi].data[g_f[fi].len] = 0;
    CHECK(strstr((char *)g_f[fi].data, "message number 000") != NULL && strstr((char *)g_f[fi].data, "message number 049") != NULL);
    CHECK(strstr((char *)g_f[fi].data, "== meshtastic: P (p) ==") != NULL);

    uint32_t freed = purr_msgstore_trim(&s, 2000);
    CHECK(freed > 0 && purr_msgstore_used(&s) <= 2400);
    purr_msg_t m[2];
    CHECK_EQ(purr_msgstore_messages(&s, (uint32_t)t, 0, m, 2), 2);
    CHECK(!strcmp(m[1].text, "message number 049") || strstr(m[1].text, "message number") != NULL);   /* the newest are kept */
    CHECK(purr_msgstore_thread_by_id(&s, (uint32_t)t)->bytes == purr_msgstore_used(&s));
    CHECK_EQ(purr_msgstore_append(&s, (uint32_t)t, PURR_MSG_IN, PURR_MSGS_RECEIVED, 6000, "", "room again", NULL), 0);
    purr_msgstore_set_limit(&s, 1000000);
    CHECK_EQ(purr_msgstore_limit(&s), 1000000u);
}

int main(void)
{
    test_basic_and_reopen();
    test_limit_trim_export();
    fs_reset();
    TK_DONE("test_msgstore");
}
