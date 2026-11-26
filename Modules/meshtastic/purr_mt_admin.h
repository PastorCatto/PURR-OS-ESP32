/* purr_mt_admin.h - administering a node over the mesh (plan9.md, "The Heltec V3 as a PURR OS node"): the messages carried on Meshtastic's private application port (PRIVATE_APP, 256) on a
 * private channel. A request is a line of the PURR command line, signed with the administrator's key (P-256, the proximity link's curve and code) over the line, a counter that only
 * goes up, and the number of the node it is for; the node refuses anything unsigned, signed by a key it was not given, replayed, or meant for another node. The answer comes back in
 * pieces. Plain C over purr_px_ecc_t and SHA-256 (Modules/proximity); no radio, no clock. */
#ifndef PURR_MT_ADMIN_H
#define PURR_MT_ADMIN_H

#include <stddef.h>
#include <stdint.h>

#include "../proximity/purr_px_pair.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_MTA_PORT 256               /* PRIVATE_APP */
#define PURR_MTA_REQ 1
#define PURR_MTA_RESP 2
#define PURR_MTA_CMD_MAX 120            /* bytes of command line in one request */
#define PURR_MTA_REQ_MAX (9 + PURR_MTA_CMD_MAX + 64)
#define PURR_MTA_RESP_DATA 180          /* bytes of answer in one piece */
#define PURR_MTA_RESP_PARTS 16          /* so an answer is at most about 2.8 KB */
#define PURR_MTA_MAX_ADMINS 4

/* ---- the administrator's side */

/* A signed request for `target` (its mesh node number) carrying `cmd` (a line, up to PURR_MTA_CMD_MAX bytes). counter must be larger than the last one this key used there. seq names the
 * exchange (the answer repeats it). Returns the length, or -1. */
int purr_mta_make_request(const purr_px_ecc_t *ecc, const uint8_t priv[32], uint32_t target, uint32_t counter, uint16_t seq, const char *cmd, uint8_t *out, size_t cap);

/* Collects the pieces of an answer. */
typedef struct {
    uint16_t seq;
    uint8_t count, got;
    uint32_t mask;
    uint16_t len;
    char text[PURR_MTA_RESP_PARTS * PURR_MTA_RESP_DATA + 1];
} purr_mta_answer_t;

void purr_mta_answer_init(purr_mta_answer_t *a, uint16_t seq);
/* A piece arrived. 1 when the answer is complete (a->text, a->len), 0 while more is needed or for a piece of another exchange or a repeat, -1 for a message that is not an answer. */
int purr_mta_answer_push(purr_mta_answer_t *a, const uint8_t *msg, size_t n);

/* ---- the node's side */

typedef struct {
    uint8_t used;
    uint8_t pub[64];
    uint32_t last_counter;          /* the highest counter seen from this key: keep it with the keys, it is what stops a replay */
} purr_mta_admin_t;

typedef struct {
    purr_mta_admin_t admin[PURR_MTA_MAX_ADMINS];
} purr_mta_node_t;

#define PURR_MTA_OK 0
#define PURR_MTA_E_FORMAT -1
#define PURR_MTA_E_NOT_FOR_US -2        /* not a request of this protocol, or too short */
#define PURR_MTA_E_SIGNATURE -3         /* signed by no key this node holds, or for another node, or altered */
#define PURR_MTA_E_REPLAY -4            /* a counter that is not higher than the last one */
#define PURR_MTA_E_FULL -5

void purr_mta_node_init(purr_mta_node_t *n);
int purr_mta_add_admin(purr_mta_node_t *n, const uint8_t pub[64]);            /* 0, or PURR_MTA_E_FULL; the same key twice is kept once */
int purr_mta_remove_admin(purr_mta_node_t *n, const uint8_t pub[64]);         /* 1 if it was there */

/* Checks a request that arrived for `my_num`. On success returns the length of the command (copied, NUL-terminated, into cmd), sets *seq, and moves that key's counter up. Otherwise a
 * PURR_MTA_E_* code and nothing is changed. */
int purr_mta_check_request(purr_mta_node_t *n, const purr_px_ecc_t *ecc, uint32_t my_num, const uint8_t *msg, size_t len, char *cmd, size_t cmd_cap, uint16_t *seq);

/* The answer to an exchange, piece by piece: how many pieces `len` bytes of text make (0 for an empty answer still makes one), and piece `part` of it. Returns the bytes written, or -1. */
int purr_mta_answer_parts(size_t len);
int purr_mta_make_answer(uint16_t seq, const char *text, size_t len, int part, uint8_t *out, size_t cap);

/* ---- the commands a node will run: a fixed safe set, not the whole command line (owner, 2026-11-26) */

typedef struct purr_mta_cmd {
    const char *name;                   /* the first word of the line, lower case: "status" */
    const char *help;
    int min_args, max_args;             /* words after the name */
    /* Writes its answer to out (NUL-terminated, at most cap - 1 bytes) and returns 0, or a negative number for a failure the answer explains. args are the words after the name. */
    int (*run)(void *ctx, int argc, char **argv, char *out, size_t cap);
} purr_mta_cmd_t;

#define PURR_MTA_E_UNKNOWN_CMD -10
#define PURR_MTA_E_BAD_ARGS -11

/* Runs a request line against the table: an unknown name is refused (and `help` and `?` list what there is), a wrong number of words is refused, a word over 40 bytes or more than 8 words
 * is refused, anything not printable ASCII is refused. Returns the command's own result, or PURR_MTA_E_*; out always holds a short answer. */
int purr_mta_dispatch(const purr_mta_cmd_t *table, int ncmds, void *ctx, const char *line, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
