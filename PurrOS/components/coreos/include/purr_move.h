/*
 * purr_move.h - moving the OTA address (plan8.md "Moving the OTA address", owner decision 2026-11-06).
 *
 * The recovery manifest address used to be a constant in every image. A device can now be told, by a small signed file next to the manifest it asks
 * (moved.txt and moved.txt.sig), that updates come from somewhere else. Plain C with no ESP-IDF dependency, tested on the PC. The device keeps the
 * result; the file is the only thing this unit reads. A redirect cannot install code: images are still verified against the boot key wherever they come from.
 *
 * moved.txt is key=value lines (unknown keys are ignored):
 *   moved=1                      the format
 *   seq=N                        rises with every move; a device ignores a file whose seq is not above the one it has
 *   key_id=N                     the key in the device's bag that signed it (a boot-role key)
 *   from=<address>               the recovery manifest address the file is placed next to: the device follows it only if it asked exactly that
 *   recovery=<address>           the new recovery.manifest
 *   modules=<address>            the new modules.manifest
 *   message=<text>               shown to the user (printable ASCII, up to 79 characters)
 * moved.txt.sig is the signature over the exact bytes of moved.txt: ECDSA P-256 over SHA-256, 64 bytes as 128 hex digits (as the repository files).
 */
#ifndef PURR_MOVE_H
#define PURR_MOVE_H

#include <stddef.h>
#include <stdint.h>

#include "purr_verify.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_MOVE_URL_LEN 120
#define PURR_MOVE_MSG_LEN 80
#define PURR_MOVE_MAX_TEXT 1024

typedef struct {
    uint32_t seq;
    uint8_t key_id;
    char from[PURR_MOVE_URL_LEN];
    char recovery[PURR_MOVE_URL_LEN];
    char modules[PURR_MOVE_URL_LEN];
    char message[PURR_MOVE_MSG_LEN];
} purr_move_t;

typedef enum {
    PURR_MOVE_OK = 0,
    PURR_MOVE_BAD_FORMAT,     /* not a moved.txt, or a field missing, too long or with odd characters */
    PURR_MOVE_NO_KEY,         /* the key id is not in the bag */
    PURR_MOVE_WRONG_ROLE,     /* the key is not a boot-role key */
    PURR_MOVE_REVOKED,
    PURR_MOVE_BAD_SIGNATURE,
    PURR_MOVE_WRONG_FROM,     /* meant for another address */
    PURR_MOVE_OLD_SEQ,        /* not newer than the move already followed */
    PURR_MOVE_BAD_URL         /* not https, or not named recovery.manifest / modules.manifest */
} purr_move_result_t;

/* Parses the text (no signature check). 0 on success, -1 if it is not a well-formed moved.txt. */
int purr_move_parse(const char *text, size_t len, purr_move_t *out);

/*
 * Parses and checks. `asked` is the recovery manifest address the device just asked (the file must say `from=` exactly that), `stored_seq` the seq of the
 * last move it followed (0 if none). On PURR_MOVE_OK, *out holds the move.
 */
purr_move_result_t purr_move_check(const purr_crypto_t *crypto, const purr_keybag_t *bag, const char *text, size_t len, const uint8_t sig[PURR_SIG_LEN],
                                   const char *asked, uint32_t stored_seq, purr_move_t *out);

/* Does the move name exactly the two default addresses (so following it means "back to the default": the stored source is cleared)? */
int purr_move_is_default(const purr_move_t *m, const char *default_recovery, const char *default_modules);

const char *purr_move_result_name(purr_move_result_t r);

#ifdef __cplusplus
}
#endif

#endif /* PURR_MOVE_H */
