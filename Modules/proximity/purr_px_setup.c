/* purr_px_setup.c - see purr_px_setup.h. */
#include "purr_px_setup.h"

#include <string.h>

static int printable(const char *s, size_t max, int allow_empty)
{
    size_t n = strlen(s);
    if (n > max || (n == 0 && !allow_empty)) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7E) {
            return 0;
        }
    }
    return 1;
}

static int put_str(uint8_t *out, size_t cap, size_t at, const char *s)
{
    size_t n = strlen(s);
    if (at + 1 + n > cap) {
        return -1;
    }
    out[at] = (uint8_t)n;
    memcpy(out + at + 1, s, n);
    return (int)(at + 1 + n);
}

int purr_pxs_make_name(const char *name, uint8_t *out, size_t cap)
{
    if (!printable(name, PURR_PXS_MAX_NAME, 0) || cap < 2) {
        return -1;
    }
    out[0] = PURR_PXS_SET_NAME;
    return put_str(out, cap, 1, name);
}

int purr_pxs_make_account(const char *user, const char *pass, uint8_t *out, size_t cap)
{
    if (!printable(user, PURR_PXS_MAX_USER, 0) || !printable(pass, PURR_PXS_MAX_PASS, 0) || cap < 3) {
        return -1;
    }
    out[0] = PURR_PXS_SET_ACCT;
    int at = put_str(out, cap, 1, user);
    return at < 0 ? -1 : put_str(out, cap, (size_t)at, pass);
}

int purr_pxs_make_wifi(const char *ssid, const char *pass, uint8_t *out, size_t cap)
{
    if (!printable(ssid, PURR_PXS_MAX_SSID, 0) || !printable(pass, PURR_PXS_MAX_PASS, 1) || cap < 3) {
        return -1;
    }
    out[0] = PURR_PXS_SET_WIFI;
    int at = put_str(out, cap, 1, ssid);
    return at < 0 ? -1 : put_str(out, cap, (size_t)at, pass);
}

int purr_pxs_make_install(const char *app, uint8_t *out, size_t cap)
{
    if (!printable(app, PURR_PXS_MAX_NAME, 0) || cap < 2) {
        return -1;
    }
    out[0] = PURR_PXS_INSTALL;
    return put_str(out, cap, 1, app);
}

int purr_pxs_make_done(uint8_t *out, size_t cap)
{
    if (cap < 1) {
        return -1;
    }
    out[0] = PURR_PXS_DONE;
    return 1;
}

int purr_pxs_parse_needs(const uint8_t *msg, size_t n, unsigned *flags, char name[PURR_PXS_MAX_NAME + 1])
{
    if (n < 3 || msg[0] != PURR_PXS_NEEDS || (size_t)msg[2] + 3 != n || msg[2] > PURR_PXS_MAX_NAME) {
        return -1;
    }
    *flags = msg[1];
    memcpy(name, msg + 3, msg[2]);
    name[msg[2]] = '\0';
    return 0;
}

int purr_pxs_parse_ack(const uint8_t *msg, size_t n, int *type, int *status)
{
    if (n != 3 || msg[0] != PURR_PXS_ACK) {
        return -1;
    }
    *type = msg[1];
    *status = msg[2];
    return 0;
}

void purr_pxs_node_init(purr_pxs_node_t *n, const purr_pxs_ops_t *ops, void *ctx)
{
    memset(n, 0, sizeof(*n));
    n->ops = ops;
    n->ctx = ctx;
}

int purr_pxs_node_needs(const purr_pxs_node_t *n, const char *device_name, uint8_t *out, size_t cap)
{
    size_t nl = strlen(device_name);
    if (nl > PURR_PXS_MAX_NAME || cap < 3 + nl) {
        return -1;
    }
    unsigned f = 0;
    if (!n->ops->has_account(n->ctx) && !n->got_account) f |= PURR_PXS_NEED_ACCOUNT;
    if (!n->got_wifi) f |= PURR_PXS_NEED_WIFI;
    if (!n->got_name) f |= PURR_PXS_NEED_NAME;
    out[0] = PURR_PXS_NEEDS;
    out[1] = (uint8_t)f;
    out[2] = (uint8_t)nl;
    memcpy(out + 3, device_name, nl);
    return (int)(3 + nl);
}

static int ack(uint8_t *out, size_t cap, int type, int status)
{
    if (cap < 3) {
        return -1;
    }
    out[0] = PURR_PXS_ACK;
    out[1] = (uint8_t)type;
    out[2] = (uint8_t)status;
    return 3;
}

/* reads a length-prefixed string at *at into dst (cap bytes with the end mark); 0, or -1 if it does not fit in the message or the buffer, or is not printable */
static int get_str(const uint8_t *msg, size_t len, size_t *at, char *dst, size_t cap, int allow_empty)
{
    if (*at >= len) {
        return -1;
    }
    size_t l = msg[*at];
    if (*at + 1 + l > len || l + 1 > cap || (l == 0 && !allow_empty)) {
        return -1;
    }
    memcpy(dst, msg + *at + 1, l);
    dst[l] = '\0';
    *at += 1 + l;
    return printable(dst, l, 1) ? 0 : -1;
}

int purr_pxs_node_rx(purr_pxs_node_t *n, const uint8_t *msg, size_t len, uint8_t *out, size_t cap)
{
    int type = len > 0 ? msg[0] : 0;
    if (!n->open) {
        return ack(out, cap, type, PURR_PXS_REFUSED);
    }
    size_t at = 1;
    char a[PURR_PXS_MAX_PASS + 1], b[PURR_PXS_MAX_PASS + 1];
    int rc = PURR_PXS_BAD;
    switch (type) {
    case PURR_PXS_SET_NAME:
        if (get_str(msg, len, &at, a, PURR_PXS_MAX_NAME + 1, 0) == 0 && at == len) {
            rc = n->ops->set_name(n->ctx, a) == 0 ? PURR_PXS_OK : PURR_PXS_FAILED;
            if (rc == PURR_PXS_OK) n->got_name = 1;
        }
        break;
    case PURR_PXS_SET_ACCT:
        if (n->ops->has_account(n->ctx) || n->got_account) {
            rc = PURR_PXS_REFUSED;                                          /* never over an account that exists: setup cannot be used to take over a device */
        } else if (get_str(msg, len, &at, a, PURR_PXS_MAX_USER + 1, 0) == 0 && get_str(msg, len, &at, b, PURR_PXS_MAX_PASS + 1, 0) == 0 && at == len) {
            rc = n->ops->set_account(n->ctx, a, b) == 0 ? PURR_PXS_OK : PURR_PXS_FAILED;
            if (rc == PURR_PXS_OK) n->got_account = 1;
        }
        break;
    case PURR_PXS_SET_WIFI:
        if (get_str(msg, len, &at, a, PURR_PXS_MAX_SSID + 1, 0) == 0 && get_str(msg, len, &at, b, PURR_PXS_MAX_PASS + 1, 1) == 0 && at == len) {
            rc = n->ops->set_wifi(n->ctx, a, b) == 0 ? PURR_PXS_OK : PURR_PXS_FAILED;
            if (rc == PURR_PXS_OK) n->got_wifi = 1;
        }
        break;
    case PURR_PXS_INSTALL:
        if (get_str(msg, len, &at, a, PURR_PXS_MAX_NAME + 1, 0) == 0 && at == len) {
            rc = n->ops->install(n->ctx, a) == 0 ? PURR_PXS_OK : PURR_PXS_FAILED;
        }
        break;
    case PURR_PXS_DONE:
        if (len == 1) {
            rc = PURR_PXS_OK;
            n->open = 0;
            n->ops->done(n->ctx);
        }
        break;
    default:
        break;
    }
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    return ack(out, cap, type, rc);
}
