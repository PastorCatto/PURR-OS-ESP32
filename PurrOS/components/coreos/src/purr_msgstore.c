/* purr_msgstore.c - see purr_msgstore.h. */
#include "purr_msgstore.h"

#include <stdio.h>
#include <string.h>

#define LINE_MAX_LEN (PURR_MSG_TEXT_MAX * 2 + PURR_MSG_ATTACH_LEN + 64)

/* ---------------------------------------------------------------- small helpers */

static uint32_t fnv(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h = (h ^ (uint8_t)*s++) * 16777619u;
    }
    return h;
}

static int key_ok(const char *k)
{
    size_t n = k ? strlen(k) : 0;
    if (n == 0 || n >= PURR_MSG_KEY_LEN) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (!((k[i] >= 'a' && k[i] <= 'z') || (k[i] >= '0' && k[i] <= '9'))) {
            return 0;
        }
    }
    return 1;
}

static void copy(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* Text on a line: newline, carriage return, the separator and the backslash are escaped. Returns the length written (without the NUL). */
static size_t esc(char *out, size_t cap, const char *in)
{
    size_t n = 0;
    for (; in && *in && n + 3 < cap; in++) {
        if (*in == '\n') {
            out[n++] = '\\'; out[n++] = 'n';
        } else if (*in == '\r') {
            continue;
        } else if (*in == '|') {
            out[n++] = '\\'; out[n++] = 'p';
        } else if (*in == '\\') {
            out[n++] = '\\'; out[n++] = '\\';
        } else {
            out[n++] = *in;
        }
    }
    out[n] = '\0';
    return n;
}

static void unesc(char *out, size_t cap, const char *in, size_t len)
{
    size_t n = 0;
    for (size_t i = 0; i < len && n + 1 < cap; i++) {
        if (in[i] == '\\' && i + 1 < len) {
            i++;
            out[n++] = in[i] == 'n' ? '\n' : in[i] == 'p' ? '|' : in[i];
        } else {
            out[n++] = in[i];
        }
    }
    out[n] = '\0';
}

static void thread_path(const purr_msgstore_t *s, const purr_msgthread_t *t, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s~%08x.thr", s->dir, t->service, (unsigned)fnv(t->peer));
}

/* Splits a line (not NUL-terminated: ptr, len) on '|' into at most max fields; the last field takes the rest. Returns the field count. */
static int split(const char *line, size_t len, const char **f, size_t *fl, int max)
{
    int n = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len && n < max; i++) {
        if (i == len || (line[i] == '|' && n < max - 1)) {
            f[n] = line + start;
            fl[n] = i - start;
            n++;
            start = i + 1;
        }
    }
    return n;
}

static uint32_t num(const char *p, size_t n)
{
    uint32_t v = 0;
    for (size_t i = 0; i < n && p[i] >= '0' && p[i] <= '9'; i++) {
        v = v * 10 + (uint32_t)(p[i] - '0');
    }
    return v;
}

/* ---------------------------------------------------------------- reading a thread's summary */

/* Walks the lines of buf (len bytes). `partial_first`: the first line may be cut and is skipped. cb is called with each line. */
typedef void (*line_fn)(void *ctx, const char *line, size_t len);
static void each_line(const char *buf, size_t len, int partial_first, line_fn cb, void *ctx)
{
    size_t i = 0;
    if (partial_first) {
        while (i < len && buf[i] != '\n') {
            i++;
        }
        i++;
    }
    while (i < len) {
        size_t j = i;
        while (j < len && buf[j] != '\n') {
            j++;
        }
        if (j < len) {                                  /* a complete line only */
            cb(ctx, buf + i, j - i);
        }
        i = j + 1;
    }
}

typedef struct {
    purr_msgthread_t *t;
    uint32_t max_seq;
} sum_ctx_t;

static void sum_line(void *vctx, const char *line, size_t len)
{
    sum_ctx_t *c = vctx;
    const char *f[7];
    size_t fl[7];
    if (len < 2) {
        return;
    }
    if (line[0] == 'M') {
        int n = split(line, len, f, fl, 7);
        if (n < 7) {
            return;
        }
        uint32_t id = num(f[1], fl[1]);
        c->t->last_id = id;
        c->t->last_dir = (uint8_t)num(f[2], fl[2]);
        c->t->last_state = (uint8_t)num(f[3], fl[3]);
        c->t->last_time = num(f[4], fl[4]);
        char tmp[PURR_MSG_TEXT_MAX + 1];
        unesc(tmp, sizeof(tmp), f[6], fl[6]);
        copy(c->t->last, sizeof(c->t->last), tmp);
        if ((id & 0xFFFFF) >= c->max_seq) {
            c->max_seq = (id & 0xFFFFF) + 1;
        }
        if (c->t->last_dir == PURR_MSG_IN) {
            c->t->unread++;
        }
    } else if (line[0] == 'S') {
        int n = split(line, len, f, fl, 3);
        if (n >= 3 && num(f[1], fl[1]) == c->t->last_id) {
            c->t->last_state = (uint8_t)num(f[2], fl[2]);
        }
    } else if (line[0] == 'R') {
        c->t->unread = 0;
    }
}

/* Reads the head (the T line) and the tail of a thread's file and fills in t. Returns 0, or < 0 if the file is not a thread. */
static int load_thread(purr_msgstore_t *s, const char *path, uint32_t size, purr_msgthread_t *t)
{
    char head[192];
    size_t got = 0;
    uint32_t total = 0;
    memset(t, 0, sizeof(*t));
    if (s->io.read(s->io.ctx, path, 0, head, sizeof(head) - 1, &got, &total) != 0 || got < 4) {
        return PURR_MSG_ERR;
    }
    head[got] = '\0';
    char *nl = memchr(head, '\n', got);
    if (head[0] != 'T' || nl == NULL) {
        return PURR_MSG_ERR;
    }
    const char *f[5];
    size_t fl[5];
    if (split(head, (size_t)(nl - head), f, fl, 5) < 5) {
        return PURR_MSG_ERR;
    }
    t->id = num(f[1], fl[1]);
    char tmp[PURR_MSG_PEER_LEN + 8];
    unesc(tmp, sizeof(tmp), f[2], fl[2]);
    copy(t->service, sizeof(t->service), tmp);
    unesc(tmp, sizeof(tmp), f[3], fl[3]);
    copy(t->peer, sizeof(t->peer), tmp);
    unesc(tmp, sizeof(tmp), f[4], fl[4]);
    copy(t->name, sizeof(t->name), tmp);
    t->bytes = size;
    t->seq = 1;
    if (t->id == 0 || !key_ok(t->service)) {
        return PURR_MSG_ERR;
    }
    /* the tail */
    uint32_t off = total > s->scratch_cap ? total - (uint32_t)s->scratch_cap : 0;
    size_t tl = 0;
    if (s->io.read(s->io.ctx, path, off, s->scratch, s->scratch_cap, &tl, &total) == 0) {
        sum_ctx_t c = {t, 1};
        each_line((const char *)s->scratch, tl, off > 0, sum_line, &c);
        t->seq = c.max_seq;
    }
    return 0;
}

/* ---------------------------------------------------------------- open */

typedef struct {
    char name[56];
    uint32_t size;
} entry_t;

typedef struct {
    entry_t e[PURR_MSG_THREADS_MAX];
    int n;
    uint32_t total;
} scan_t;

static void scan_cb(void *vctx, const char *name, uint32_t size)
{
    scan_t *sc = vctx;
    size_t n = strlen(name);
    sc->total += size;
    if (n > 4 && strcmp(name + n - 4, ".thr") == 0 && n < sizeof(sc->e[0].name) && sc->n < PURR_MSG_THREADS_MAX) {
        copy(sc->e[sc->n].name, sizeof(sc->e[0].name), name);
        sc->e[sc->n].size = size;
        sc->n++;
    }
}

int purr_msgstore_open(purr_msgstore_t *s, const purr_msgio_t *io, const char *dir, uint32_t limit, void *scratch, size_t scratch_cap)
{
    if (s == NULL || io == NULL || dir == NULL || scratch == NULL || scratch_cap < 1024 || strlen(dir) >= sizeof(s->dir)) {
        return PURR_MSG_ERR;
    }
    memset(s, 0, sizeof(*s));
    s->io = *io;
    copy(s->dir, sizeof(s->dir), dir);
    s->limit = limit != 0 ? limit : PURR_MSG_LIMIT_DEFAULT;
    s->scratch = scratch;
    s->scratch_cap = scratch_cap;
    s->next_thread = 1;
    scan_t sc;
    memset(&sc, 0, sizeof(sc));
    if (io->list(io->ctx, dir, scan_cb, &sc) != 0) {
        return 0;                                       /* no folder yet: nothing stored */
    }
    s->used = sc.total;
    for (int i = 0; i < sc.n; i++) {
        char path[96];
        snprintf(path, sizeof(path), "%s/%s", dir, sc.e[i].name);
        purr_msgthread_t t;
        if (load_thread(s, path, sc.e[i].size, &t) == 0 && s->nth < PURR_MSG_THREADS_MAX) {
            s->th[s->nth++] = t;
            if (t.id >= s->next_thread) {
                s->next_thread = t.id + 1;
            }
        }
    }
    return s->nth;
}

/* ---------------------------------------------------------------- threads */

static purr_msgthread_t *find_id(purr_msgstore_t *s, uint32_t id)
{
    for (int i = 0; i < s->nth; i++) {
        if (s->th[i].id == id) {
            return &s->th[i];
        }
    }
    return NULL;
}

const purr_msgthread_t *purr_msgstore_thread_by_id(const purr_msgstore_t *s, uint32_t id)
{
    return find_id((purr_msgstore_t *)s, id);
}

int32_t purr_msgstore_thread(purr_msgstore_t *s, const char *service, const char *peer, const char *name)
{
    if (s == NULL || !key_ok(service) || peer == NULL || peer[0] == '\0' || strlen(peer) >= PURR_MSG_PEER_LEN) {
        return PURR_MSG_ERR;
    }
    for (int i = 0; i < s->nth; i++) {
        if (strcmp(s->th[i].service, service) == 0 && strcmp(s->th[i].peer, peer) == 0) {
            if (name != NULL && name[0] != '\0' && strncmp(s->th[i].name, name, PURR_MSG_NAME_LEN - 1) != 0) {
                copy(s->th[i].name, sizeof(s->th[i].name), name);      /* the shown name is kept in memory; the file keeps the first one */
            }
            return (int32_t)s->th[i].id;
        }
    }
    if (s->nth >= PURR_MSG_THREADS_MAX) {
        return PURR_MSG_ERR;
    }
    purr_msgthread_t *t = &s->th[s->nth];
    memset(t, 0, sizeof(*t));
    t->id = s->next_thread;
    copy(t->service, sizeof(t->service), service);
    copy(t->peer, sizeof(t->peer), peer);
    copy(t->name, sizeof(t->name), name);
    t->seq = 1;
    char pe[PURR_MSG_PEER_LEN * 2 + 4], ne[PURR_MSG_NAME_LEN * 2 + 4], line[160], path[96];
    esc(pe, sizeof(pe), t->peer);
    esc(ne, sizeof(ne), t->name);
    int n = snprintf(line, sizeof(line), "T|%u|%s|%s|%s\n", (unsigned)t->id, t->service, pe, ne);
    if (s->used + (uint32_t)n > s->limit) {
        return PURR_MSG_FULL;
    }
    thread_path(s, t, path, sizeof(path));
    if (s->io.append(s->io.ctx, path, line, (size_t)n) != 0) {
        return PURR_MSG_ERR;
    }
    t->bytes = (uint32_t)n;
    s->used += (uint32_t)n;
    s->next_thread++;
    s->nth++;
    return (int32_t)t->id;
}

int purr_msgstore_order(const purr_msgstore_t *s, uint32_t *ids, int max)
{
    int n = 0;
    int used[PURR_MSG_THREADS_MAX] = {0};
    while (n < max && n < s->nth) {
        int best = -1;
        for (int i = 0; i < s->nth; i++) {
            if (!used[i] && (best < 0 || s->th[i].last_time > s->th[best].last_time ||
                             (s->th[i].last_time == s->th[best].last_time && s->th[i].id > s->th[best].id))) {
                best = i;
            }
        }
        used[best] = 1;
        ids[n++] = s->th[best].id;
    }
    return n;
}

/* ---------------------------------------------------------------- appending */

int purr_msgstore_append(purr_msgstore_t *s, uint32_t thread, int dir, int state, uint32_t time, const char *attach, const char *text, uint32_t *msg_id)
{
    purr_msgthread_t *t = s ? find_id(s, thread) : NULL;
    if (t == NULL) {
        return PURR_MSG_NOTFOUND;
    }
    char te[PURR_MSG_TEXT_MAX * 2 + 4], ae[PURR_MSG_ATTACH_LEN * 2 + 4], line[LINE_MAX_LEN], path[96];
    char clipped[PURR_MSG_TEXT_MAX + 1];
    copy(clipped, sizeof(clipped), text);
    esc(te, sizeof(te), clipped);
    esc(ae, sizeof(ae), attach);
    uint32_t id = (t->id << 20) | (t->seq & 0xFFFFF);
    int n = snprintf(line, sizeof(line), "M|%u|%d|%d|%u|%s|%s\n", (unsigned)id, dir ? 1 : 0, state, (unsigned)time, ae, te);
    if (n <= 0 || (size_t)n >= sizeof(line)) {
        return PURR_MSG_ERR;
    }
    if (s->used + (uint32_t)n > s->limit) {
        return PURR_MSG_FULL;
    }
    thread_path(s, t, path, sizeof(path));
    if (s->io.append(s->io.ctx, path, line, (size_t)n) != 0) {
        return PURR_MSG_ERR;
    }
    t->seq++;
    t->bytes += (uint32_t)n;
    s->used += (uint32_t)n;
    t->last_id = id;
    t->last_dir = dir ? 1 : 0;
    t->last_state = (uint8_t)state;
    t->last_time = time;
    copy(t->last, sizeof(t->last), clipped);
    if (!dir) {
        t->unread++;
    }
    if (msg_id) {
        *msg_id = id;
    }
    return PURR_MSG_OK;
}

int purr_msgstore_set_state(purr_msgstore_t *s, uint32_t msg_id, int state)
{
    purr_msgthread_t *t = s ? find_id(s, msg_id >> 20) : NULL;
    if (t == NULL) {
        return PURR_MSG_NOTFOUND;
    }
    char line[40], path[96];
    int n = snprintf(line, sizeof(line), "S|%u|%d\n", (unsigned)msg_id, state);
    if (s->used + (uint32_t)n > s->limit) {
        return PURR_MSG_FULL;
    }
    thread_path(s, t, path, sizeof(path));
    if (s->io.append(s->io.ctx, path, line, (size_t)n) != 0) {
        return PURR_MSG_ERR;
    }
    t->bytes += (uint32_t)n;
    s->used += (uint32_t)n;
    if (t->last_id == msg_id) {
        t->last_state = (uint8_t)state;
    }
    return PURR_MSG_OK;
}

int purr_msgstore_mark_read(purr_msgstore_t *s, uint32_t thread, uint32_t time)
{
    purr_msgthread_t *t = s ? find_id(s, thread) : NULL;
    if (t == NULL) {
        return PURR_MSG_NOTFOUND;
    }
    if (t->unread == 0) {
        return PURR_MSG_OK;
    }
    char line[24], path[96];
    int n = snprintf(line, sizeof(line), "R|%u\n", (unsigned)time);
    thread_path(s, t, path, sizeof(path));
    if (s->used + (uint32_t)n <= s->limit && s->io.append(s->io.ctx, path, line, (size_t)n) == 0) {
        t->bytes += (uint32_t)n;
        s->used += (uint32_t)n;
    }
    t->unread = 0;                                      /* in memory regardless: at worst the marker is lost and the count comes back after a restart */
    return PURR_MSG_OK;
}

int purr_msgstore_delete_thread(purr_msgstore_t *s, uint32_t thread)
{
    for (int i = 0; s && i < s->nth; i++) {
        if (s->th[i].id == thread) {
            char path[96];
            thread_path(s, &s->th[i], path, sizeof(path));
            s->io.remove(s->io.ctx, path);
            s->used = s->used >= s->th[i].bytes ? s->used - s->th[i].bytes : 0;
            s->th[i] = s->th[s->nth - 1];
            s->nth--;
            return PURR_MSG_OK;
        }
    }
    return PURR_MSG_NOTFOUND;
}

/* ---------------------------------------------------------------- reading messages */

typedef struct {
    int total;                  /* pass 1: M lines */
    int from, to;               /* pass 2: the index range wanted */
    int idx;
    purr_msg_t *out;
    int k, max;
    uint32_t want_id;           /* purr_msgstore_message */
    int found;
} read_ctx_t;

static void fill_msg(purr_msg_t *m, const char **f, size_t *fl)
{
    memset(m, 0, sizeof(*m));
    m->id = num(f[1], fl[1]);
    m->dir = (uint8_t)num(f[2], fl[2]);
    m->state = (uint8_t)num(f[3], fl[3]);
    m->time = num(f[4], fl[4]);
    unesc(m->attach, sizeof(m->attach), f[5], fl[5]);
    unesc(m->text, sizeof(m->text), f[6], fl[6]);
}

static void count_line(void *vctx, const char *line, size_t len)
{
    read_ctx_t *c = vctx;
    if (len > 1 && line[0] == 'M') {
        c->total++;
    }
}

static void pick_line(void *vctx, const char *line, size_t len)
{
    read_ctx_t *c = vctx;
    const char *f[7];
    size_t fl[7];
    if (len < 2) {
        return;
    }
    if (line[0] == 'M' && split(line, len, f, fl, 7) >= 7) {
        if (c->idx >= c->from && c->idx < c->to && c->k < c->max) {
            fill_msg(&c->out[c->k++], f, fl);
        }
        c->idx++;
    } else if (line[0] == 'S' && split(line, len, f, fl, 3) >= 3) {
        uint32_t id = num(f[1], fl[1]);
        for (int i = 0; i < c->k; i++) {
            if (c->out[i].id == id) {
                c->out[i].state = (uint8_t)num(f[2], fl[2]);
            }
        }
    }
}

static int read_tail(purr_msgstore_t *s, const purr_msgthread_t *t, size_t *len, int *partial)
{
    char path[96];
    uint32_t total = 0;
    thread_path(s, t, path, sizeof(path));
    uint32_t off = t->bytes > s->scratch_cap ? t->bytes - (uint32_t)s->scratch_cap : 0;
    *partial = off > 0;
    return s->io.read(s->io.ctx, path, off, s->scratch, s->scratch_cap, len, &total);
}

int purr_msgstore_messages(purr_msgstore_t *s, uint32_t thread, int skip, purr_msg_t *out, int max)
{
    purr_msgthread_t *t = s ? find_id(s, thread) : NULL;
    size_t len = 0;
    int partial = 0;
    if (t == NULL || out == NULL || max <= 0 || read_tail(s, t, &len, &partial) != 0) {
        return 0;
    }
    read_ctx_t c;
    memset(&c, 0, sizeof(c));
    each_line((const char *)s->scratch, len, partial, count_line, &c);
    c.to = c.total - (skip > 0 ? skip : 0);
    c.from = c.to - max;
    if (c.to <= 0) {
        return 0;
    }
    if (c.from < 0) {
        c.from = 0;
    }
    c.out = out;
    c.max = max;
    each_line((const char *)s->scratch, len, partial, pick_line, &c);
    return c.k;
}

static void find_line(void *vctx, const char *line, size_t len)
{
    read_ctx_t *c = vctx;
    const char *f[7];
    size_t fl[7];
    if (len < 2) {
        return;
    }
    if (line[0] == 'M' && split(line, len, f, fl, 7) >= 7 && num(f[1], fl[1]) == c->want_id) {
        fill_msg(c->out, f, fl);
        c->found = 1;
    } else if (line[0] == 'S' && c->found && split(line, len, f, fl, 3) >= 3 && num(f[1], fl[1]) == c->want_id) {
        c->out->state = (uint8_t)num(f[2], fl[2]);
    }
}

int purr_msgstore_message(purr_msgstore_t *s, uint32_t msg_id, purr_msg_t *out)
{
    purr_msgthread_t *t = s ? find_id(s, msg_id >> 20) : NULL;
    size_t len = 0;
    int partial = 0;
    if (t == NULL || out == NULL || read_tail(s, t, &len, &partial) != 0) {
        return PURR_MSG_NOTFOUND;
    }
    read_ctx_t c;
    memset(&c, 0, sizeof(c));
    c.want_id = msg_id;
    c.out = out;
    each_line((const char *)s->scratch, len, partial, find_line, &c);
    return c.found ? PURR_MSG_OK : PURR_MSG_NOTFOUND;
}

/* ---------------------------------------------------------------- the limit, trimming, export */

uint32_t purr_msgstore_used(const purr_msgstore_t *s) { return s ? s->used : 0; }
uint32_t purr_msgstore_limit(const purr_msgstore_t *s) { return s ? s->limit : 0; }
void purr_msgstore_set_limit(purr_msgstore_t *s, uint32_t limit) { if (s && limit > 0) { s->limit = limit; } }

/* Keeps the T line and the newest half of the lines that fit the window. */
static uint32_t trim_thread(purr_msgstore_t *s, purr_msgthread_t *t)
{
    char path[96];
    size_t len = 0;
    int partial = 0;
    if (read_tail(s, t, &len, &partial) != 0 || len < 64) {
        return 0;
    }
    const char *buf = (const char *)s->scratch;
    size_t i = partial ? 0 : 0;
    if (partial) {
        while (i < len && buf[i] != '\n') {
            i++;
        }
        i++;
    } else {
        while (i < len && buf[i] != '\n') {                 /* skip the T line: it is written again below */
            i++;
        }
        i++;
    }
    size_t body = len > i ? len - i : 0;
    size_t keep_from = i + body / 2;
    while (keep_from < len && buf[keep_from - 1] != '\n') {
        keep_from++;
    }
    if (keep_from >= len) {
        return 0;
    }
    char pe[PURR_MSG_PEER_LEN * 2 + 4], ne[PURR_MSG_NAME_LEN * 2 + 4], head[160];
    esc(pe, sizeof(pe), t->peer);
    esc(ne, sizeof(ne), t->name);
    int hn = snprintf(head, sizeof(head), "T|%u|%s|%s|%s\n", (unsigned)t->id, t->service, pe, ne);
    size_t kl = len - keep_from;
    if ((size_t)hn + kl > s->scratch_cap) {
        return 0;
    }
    memmove(s->scratch + hn, s->scratch + keep_from, kl);
    memcpy(s->scratch, head, (size_t)hn);
    thread_path(s, t, path, sizeof(path));
    if (s->io.write(s->io.ctx, path, s->scratch, (size_t)hn + kl) != 0) {
        return 0;
    }
    uint32_t before = t->bytes;
    t->bytes = (uint32_t)(hn + kl);
    s->used = s->used - before + t->bytes;
    return before - t->bytes;
}

uint32_t purr_msgstore_trim(purr_msgstore_t *s, uint32_t target)
{
    uint32_t freed = 0;
    for (int round = 0; s && s->used > target && round < 64; round++) {
        purr_msgthread_t *big = NULL;
        for (int i = 0; i < s->nth; i++) {
            if (big == NULL || s->th[i].bytes > big->bytes) {
                big = &s->th[i];
            }
        }
        if (big == NULL) {
            break;
        }
        uint32_t f = trim_thread(s, big);
        if (f == 0) {
            break;
        }
        freed += f;
    }
    return freed;
}

typedef struct {
    purr_msgstore_t *s;
    const char *path;
    int err;
} export_ctx_t;

static void export_line(void *vctx, const char *line, size_t len)
{
    export_ctx_t *c = vctx;
    const char *f[7];
    size_t fl[7];
    if (c->err || len < 2 || line[0] != 'M' || split(line, len, f, fl, 7) < 7) {
        return;
    }
    char attach[PURR_MSG_ATTACH_LEN], text[PURR_MSG_TEXT_MAX + 1], out[PURR_MSG_TEXT_MAX + PURR_MSG_ATTACH_LEN + 64];
    unesc(attach, sizeof(attach), f[5], fl[5]);
    unesc(text, sizeof(text), f[6], fl[6]);
    int n = snprintf(out, sizeof(out), "[%u] %s: %s", (unsigned)num(f[4], fl[4]), num(f[2], fl[2]) == PURR_MSG_OUT ? "you" : "them", text);
    if (attach[0] && n > 0 && (size_t)n < sizeof(out)) {
        n += snprintf(out + n, sizeof(out) - (size_t)n, " (picture: %s)", attach);
    }
    if (n > 0 && (size_t)n + 1 < sizeof(out)) {
        out[n++] = '\n';
        if (c->s->io.append(c->s->io.ctx, c->path, out, (size_t)n) != 0) {
            c->err = 1;
        }
    }
}

int purr_msgstore_export(purr_msgstore_t *s, const char *path)
{
    if (s == NULL || path == NULL) {
        return PURR_MSG_ERR;
    }
    static const char title[] = "PURR OS messages export\n\n";
    if (s->io.write(s->io.ctx, path, title, sizeof(title) - 1) != 0) {
        return PURR_MSG_ERR;
    }
    export_ctx_t c = {s, path, 0};
    for (int i = 0; i < s->nth && !c.err; i++) {
        const purr_msgthread_t *t = &s->th[i];
        char hd[160], tp[96];
        int hn = snprintf(hd, sizeof(hd), "== %s: %s (%s) ==\n", t->service, t->name[0] ? t->name : t->peer, t->peer);
        if (s->io.append(s->io.ctx, path, hd, (size_t)hn) != 0) {
            return PURR_MSG_ERR;
        }
        thread_path(s, t, tp, sizeof(tp));
        uint32_t off = 0;
        while (off < t->bytes && !c.err) {                          /* the file from its start, a chunk at a time, cut at a line end */
            size_t got = 0;
            uint32_t total = 0;
            if (s->io.read(s->io.ctx, tp, off, s->scratch, s->scratch_cap, &got, &total) != 0 || got == 0) {
                break;
            }
            size_t end = got;
            if (off + got < total) {
                while (end > 0 && s->scratch[end - 1] != '\n') {
                    end--;
                }
                if (end == 0) {
                    break;                                          /* a line longer than the window: cannot happen with the sizes in use */
                }
            }
            each_line((const char *)s->scratch, end, 0, export_line, &c);
            off += (uint32_t)end;
        }
        s->io.append(s->io.ctx, path, "\n", 1);
    }
    return c.err ? PURR_MSG_ERR : PURR_MSG_OK;
}
