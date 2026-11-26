/* purr_px_setup.h - remote setup (plan1.md, "Remote setup"): a device with no account yet asks a paired device to set it up, and the paired device sends it what it needs (a name, an
 * account, a Wi-Fi network, apps to install). The messages travel on the setup channel (PURR_PX_CH_SETUP) of a secure session (purr_px_pair.h), so the password never leaves it in the clear.
 * Plain C: the node side is a small state machine that calls back into the device's own setup code; the admin side builds and reads messages. */
#ifndef PURR_PX_SETUP_H
#define PURR_PX_SETUP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_PXS_NEEDS     0x01         /* node -> admin: what is still missing */
#define PURR_PXS_SET_NAME  0x02         /* admin -> node: the device's name */
#define PURR_PXS_SET_ACCT  0x03         /* the first account: user name and password */
#define PURR_PXS_SET_WIFI  0x04         /* a Wi-Fi network: name and password */
#define PURR_PXS_INSTALL   0x05         /* an app (or driver) to install, by name, from the repositories (needs the network) */
#define PURR_PXS_DONE      0x06         /* finished: the node applies everything and restarts */
#define PURR_PXS_ACK       0x10         /* node -> admin: the type it answers and a status */

#define PURR_PXS_NEED_ACCOUNT (1u << 0)
#define PURR_PXS_NEED_WIFI    (1u << 1)
#define PURR_PXS_NEED_NAME    (1u << 2)

#define PURR_PXS_OK        0
#define PURR_PXS_REFUSED   1            /* an account exists already, or setup is closed */
#define PURR_PXS_BAD       2            /* a message that does not make sense (lengths, characters) */
#define PURR_PXS_FAILED    3            /* the device could not do it */
#define PURR_PXS_MAX_NAME  24
#define PURR_PXS_MAX_USER  23
#define PURR_PXS_MAX_PASS  64
#define PURR_PXS_MAX_SSID  32
#define PURR_PXS_MAX_MSG   (3 + PURR_PXS_MAX_USER + PURR_PXS_MAX_PASS)

/* ---- admin side: builders (each returns the length, or -1) */
int purr_pxs_make_name(const char *name, uint8_t *out, size_t cap);
int purr_pxs_make_account(const char *user, const char *pass, uint8_t *out, size_t cap);
int purr_pxs_make_wifi(const char *ssid, const char *pass, uint8_t *out, size_t cap);
int purr_pxs_make_install(const char *app, uint8_t *out, size_t cap);
int purr_pxs_make_done(uint8_t *out, size_t cap);
/* The node's NEEDS: flags (PURR_PXS_NEED_*) and its name; 0, or -1 for something else. */
int purr_pxs_parse_needs(const uint8_t *msg, size_t n, unsigned *flags, char name[PURR_PXS_MAX_NAME + 1]);
/* An ACK: the type it answers and the status; 0, or -1. */
int purr_pxs_parse_ack(const uint8_t *msg, size_t n, int *type, int *status);

/* ---- node side */
typedef struct {
    int (*has_account)(void *ctx);                                                  /* 1 once the device has a user */
    int (*set_name)(void *ctx, const char *name);                                   /* 0 or < 0 */
    int (*set_account)(void *ctx, const char *user, const char *pass);
    int (*set_wifi)(void *ctx, const char *ssid, const char *pass);
    int (*install)(void *ctx, const char *app);
    void (*done)(void *ctx);                                                        /* apply and restart */
} purr_pxs_ops_t;

typedef struct {
    const purr_pxs_ops_t *ops;
    void *ctx;
    int open;                       /* 1 while the device accepts setup (it asked for it, and a paired device is the sender) */
    int got_account, got_wifi, got_name;
} purr_pxs_node_t;

void purr_pxs_node_init(purr_pxs_node_t *n, const purr_pxs_ops_t *ops, void *ctx);
/* What the device is still missing, and the NEEDS message that says so (written to out); returns its length. */
int purr_pxs_node_needs(const purr_pxs_node_t *n, const char *device_name, uint8_t *out, size_t cap);
/* A message from the paired device. Writes the ACK to out and returns its length (always an answer, never silence). A message while closed, or SET_ACCT once an account exists, is refused. */
int purr_pxs_node_rx(purr_pxs_node_t *n, const uint8_t *msg, size_t len, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
