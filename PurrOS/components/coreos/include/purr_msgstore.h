/*
 * purr_msgstore.h - the message store behind the `messaging` catcall (Messaging/SPEC.md): threads (one per service and peer) and the messages in them, as plain text files in one folder.
 *
 * One file per thread, "<service>~<hash of the peer>.thr", append-only. The first line names the thread, then one line per event:
 *     T|<thread id>|<service>|<peer>|<name>
 *     M|<message id>|<0 in, 1 out>|<state>|<time>|<attachment path>|<text, with \n \p \\ escaped>
 *     S|<message id>|<state>            a later change of state
 *     R|<time>                          everything up to here has been read
 * Nothing is kept in memory except a small summary of each thread; the messages are read from the end of the file on demand. The total size is counted against a limit (1 MB by
 * default): an append that would pass it is refused with PURR_MSG_FULL, and the caller decides (raise the limit, or export and trim). Plain C, no ESP-IDF, host-tested.
 */
#ifndef PURR_MSGSTORE_H
#define PURR_MSGSTORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_MSG_TEXT_MAX    240
#define PURR_MSG_KEY_LEN     16     /* a service's key: lower case letters and digits */
#define PURR_MSG_PEER_LEN    40
#define PURR_MSG_NAME_LEN    24
#define PURR_MSG_ATTACH_LEN  56
#define PURR_MSG_THREADS_MAX 32
#define PURR_MSG_LIMIT_DEFAULT (1024u * 1024u)

#define PURR_MSG_IN  0
#define PURR_MSG_OUT 1

#define PURR_MSGS_QUEUED    0
#define PURR_MSGS_SENDING   1
#define PURR_MSGS_SENT      2
#define PURR_MSGS_DELIVERED 3
#define PURR_MSGS_FAILED    4
#define PURR_MSGS_RECEIVED  5

#define PURR_MSG_OK     0
#define PURR_MSG_ERR   (-1)
#define PURR_MSG_FULL  (-2)         /* the limit is used up */
#define PURR_MSG_NOTFOUND (-3)

/* The file system, as the store needs it. Paths are what the store builds ("<dir>/<name>"). All return 0 on success. */
typedef struct {
    void *ctx;
    /* Up to cap bytes from offset; *len = bytes read, *total = the file's size. */
    int (*read)(void *ctx, const char *path, uint32_t offset, void *buf, size_t cap, size_t *len, uint32_t *total);
    int (*append)(void *ctx, const char *path, const void *data, size_t len);        /* creates the file and the folder if needed */
    int (*write)(void *ctx, const char *path, const void *data, size_t len);         /* replaces the file */
    int (*remove)(void *ctx, const char *path);
    int (*list)(void *ctx, const char *dir, void (*cb)(void *c2, const char *name, uint32_t size), void *c2);
} purr_msgio_t;

typedef struct {
    uint32_t id;                    /* stable; 0 = unused */
    char service[PURR_MSG_KEY_LEN];
    char peer[PURR_MSG_PEER_LEN];
    char name[PURR_MSG_NAME_LEN];   /* what to show; may be empty (then the peer is shown) */
    uint32_t last_time;
    uint32_t unread;
    uint32_t bytes;                 /* the file's size */
    uint32_t seq;                   /* the next message number in this thread */
    uint32_t last_id;               /* the newest message */
    uint8_t last_dir, last_state;
    char last[48];                  /* the start of the newest message */
} purr_msgthread_t;

typedef struct {
    uint32_t id;
    uint8_t dir, state;
    uint32_t time;
    char attach[PURR_MSG_ATTACH_LEN];
    char text[PURR_MSG_TEXT_MAX + 1];
} purr_msg_t;

typedef struct {
    purr_msgio_t io;
    char dir[48];
    purr_msgthread_t th[PURR_MSG_THREADS_MAX];
    int nth;
    uint32_t next_thread;
    uint32_t limit, used;
    uint8_t *scratch;               /* supplied by the caller; one thread's tail is read into it */
    size_t scratch_cap;
} purr_msgstore_t;

/* Scans the folder and builds the thread table. scratch should be about 12 KB. Returns the number of threads, or < 0. */
int purr_msgstore_open(purr_msgstore_t *s, const purr_msgio_t *io, const char *dir, uint32_t limit, void *scratch, size_t scratch_cap);

/* Finds the thread for (service, peer) or makes it. name may be NULL; a non-empty name replaces the stored one. Returns the thread id, or < 0 (table full, bad input). */
int32_t purr_msgstore_thread(purr_msgstore_t *s, const char *service, const char *peer, const char *name);
const purr_msgthread_t *purr_msgstore_thread_by_id(const purr_msgstore_t *s, uint32_t id);
/* Thread ids, newest first. Returns how many were written (at most max). */
int purr_msgstore_order(const purr_msgstore_t *s, uint32_t *ids, int max);

/* Adds a message and returns its id in *msg_id. PURR_MSG_FULL if it would pass the limit (nothing is written). */
int purr_msgstore_append(purr_msgstore_t *s, uint32_t thread, int dir, int state, uint32_t time, const char *attach, const char *text, uint32_t *msg_id);
int purr_msgstore_set_state(purr_msgstore_t *s, uint32_t msg_id, int state);
int purr_msgstore_mark_read(purr_msgstore_t *s, uint32_t thread, uint32_t time);
int purr_msgstore_delete_thread(purr_msgstore_t *s, uint32_t thread);

/* Messages of a thread, oldest first within the window: the `max` messages that end `skip` messages before the newest. Returns how many were written. */
int purr_msgstore_messages(purr_msgstore_t *s, uint32_t thread, int skip, purr_msg_t *out, int max);
/* One message by id (searched in its thread's tail). */
int purr_msgstore_message(purr_msgstore_t *s, uint32_t msg_id, purr_msg_t *out);

uint32_t purr_msgstore_used(const purr_msgstore_t *s);
uint32_t purr_msgstore_limit(const purr_msgstore_t *s);
void purr_msgstore_set_limit(purr_msgstore_t *s, uint32_t limit);

/* Drops the oldest messages of every thread, a half at a time, until the store uses at most target bytes. Returns the bytes freed. Meant to run after an export. */
uint32_t purr_msgstore_trim(purr_msgstore_t *s, uint32_t target);
/* Writes every thread, oldest message first, as readable text into one file (created fresh at path). Returns 0, or < 0. */
int purr_msgstore_export(purr_msgstore_t *s, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* PURR_MSGSTORE_H */
