/* purr_mt_admin.c - see purr_mt_admin.h. */
#include "purr_mt_admin.h"

#include <string.h>

#include "../proximity/purr_px_crypto.h"

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* what is signed: a label, the node it is for, then everything in the request before the signature */
static void request_hash(uint32_t target, const uint8_t *head, size_t head_len, uint8_t out[32])
{
    uint8_t buf[9 + PURR_MTA_CMD_MAX + 16];
    static const char label[] = "PURR-MESH-ADMIN1";
    memcpy(buf, label, 16);
    put32(buf + 16, target);
    memcpy(buf + 20, head, head_len);
    purr_px_sha256(buf, 20 + head_len, out);
}

int purr_mta_make_request(const purr_px_ecc_t *ecc, const uint8_t priv[32], uint32_t target, uint32_t counter, uint16_t seq, const char *cmd, uint8_t *out, size_t cap)
{
    size_t cl = strlen(cmd);
    if (cl == 0 || cl > PURR_MTA_CMD_MAX || cap < 9 + cl + 64) {
        return -1;
    }
    out[0] = PURR_MTA_REQ;
    out[1] = 1;
    put32(out + 2, counter);
    out[6] = (uint8_t)seq;
    out[7] = (uint8_t)(seq >> 8);
    out[8] = (uint8_t)cl;
    memcpy(out + 9, cmd, cl);
    uint8_t h[32];
    request_hash(target, out, 9 + cl, h);
    if (!ecc->sign(priv, h, out + 9 + cl)) {
        return -1;
    }
    return (int)(9 + cl + 64);
}

void purr_mta_answer_init(purr_mta_answer_t *a, uint16_t seq)
{
    memset(a, 0, sizeof(*a));
    a->seq = seq;
}

int purr_mta_answer_push(purr_mta_answer_t *a, const uint8_t *msg, size_t n)
{
    if (n < 5 || msg[0] != PURR_MTA_RESP) {
        return -1;
    }
    uint16_t seq = (uint16_t)(msg[1] | (msg[2] << 8));
    int part = msg[3], count = msg[4];
    if (seq != a->seq) {
        return 0;
    }
    if (count < 1 || count > PURR_MTA_RESP_PARTS || part >= count || n - 5 > PURR_MTA_RESP_DATA) {
        return -1;
    }
    if (a->count == 0) {
        a->count = (uint8_t)count;
    } else if (a->count != count) {
        return -1;
    }
    if (a->mask & (1u << part)) {
        return 0;
    }
    memcpy(a->text + (size_t)part * PURR_MTA_RESP_DATA, msg + 5, n - 5);
    a->mask |= 1u << part;
    a->got++;
    if (part == count - 1) {
        a->len = (uint16_t)((size_t)part * PURR_MTA_RESP_DATA + (n - 5));
    }
    if (a->got == a->count) {
        a->text[a->len] = '\0';
        return 1;
    }
    return 0;
}

void purr_mta_node_init(purr_mta_node_t *n) { memset(n, 0, sizeof(*n)); }

int purr_mta_add_admin(purr_mta_node_t *n, const uint8_t pub[64])
{
    int free_slot = -1;
    for (int i = 0; i < PURR_MTA_MAX_ADMINS; i++) {
        if (n->admin[i].used && memcmp(n->admin[i].pub, pub, 64) == 0) {
            return 0;
        }
        if (!n->admin[i].used && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        return PURR_MTA_E_FULL;
    }
    n->admin[free_slot].used = 1;
    memcpy(n->admin[free_slot].pub, pub, 64);
    n->admin[free_slot].last_counter = 0;
    return 0;
}

int purr_mta_remove_admin(purr_mta_node_t *n, const uint8_t pub[64])
{
    for (int i = 0; i < PURR_MTA_MAX_ADMINS; i++) {
        if (n->admin[i].used && memcmp(n->admin[i].pub, pub, 64) == 0) {
            memset(&n->admin[i], 0, sizeof(n->admin[i]));
            return 1;
        }
    }
    return 0;
}

int purr_mta_check_request(purr_mta_node_t *n, const purr_px_ecc_t *ecc, uint32_t my_num, const uint8_t *msg, size_t len, char *cmd, size_t cmd_cap, uint16_t *seq)
{
    if (len < 9 + 1 + 64 || msg[0] != PURR_MTA_REQ || msg[1] != 1) {
        return PURR_MTA_E_NOT_FOR_US;
    }
    size_t cl = msg[8];
    if (cl == 0 || cl > PURR_MTA_CMD_MAX || len != 9 + cl + 64 || cmd_cap < cl + 1) {
        return PURR_MTA_E_FORMAT;
    }
    uint32_t counter = get32(msg + 2);
    uint8_t h[32];
    request_hash(my_num, msg, 9 + cl, h);
    int key = -1, signed_ok = 0;
    for (int i = 0; i < PURR_MTA_MAX_ADMINS; i++) {
        if (n->admin[i].used && ecc->verify(n->admin[i].pub, h, msg + 9 + cl)) {
            key = i;
            signed_ok = 1;
            break;
        }
    }
    if (!signed_ok) {
        return PURR_MTA_E_SIGNATURE;
    }
    if (counter <= n->admin[key].last_counter) {
        return PURR_MTA_E_REPLAY;
    }
    n->admin[key].last_counter = counter;                               /* only after the signature has been checked: a forger cannot use up a counter */
    memcpy(cmd, msg + 9, cl);
    cmd[cl] = '\0';
    *seq = (uint16_t)(msg[6] | (msg[7] << 8));
    return (int)cl;
}

int purr_mta_answer_parts(size_t len)
{
    int p = (int)((len + PURR_MTA_RESP_DATA - 1) / PURR_MTA_RESP_DATA);
    return p < 1 ? 1 : p;
}

int purr_mta_make_answer(uint16_t seq, const char *text, size_t len, int part, uint8_t *out, size_t cap)
{
    int parts = purr_mta_answer_parts(len);
    if (parts > PURR_MTA_RESP_PARTS || part < 0 || part >= parts) {
        return -1;
    }
    size_t off = (size_t)part * PURR_MTA_RESP_DATA;
    size_t n = len > off ? len - off : 0;
    if (n > PURR_MTA_RESP_DATA) {
        n = PURR_MTA_RESP_DATA;
    }
    if (cap < 5 + n) {
        return -1;
    }
    out[0] = PURR_MTA_RESP;
    out[1] = (uint8_t)seq;
    out[2] = (uint8_t)(seq >> 8);
    out[3] = (uint8_t)part;
    out[4] = (uint8_t)parts;
    memcpy(out + 5, text + off, n);
    return (int)(5 + n);
}
