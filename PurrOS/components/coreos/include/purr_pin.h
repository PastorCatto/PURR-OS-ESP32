/*
 * purr_pin.h - the optional PIN of the lock screen (plan7.md, "The Type shell").
 *
 * A PIN is a quick unlock for the lock screen, nothing more: the account password still signs in after a restart, and the PIN is not a secret that survives a
 * copy of the storage (four digits are too few for that). What it does give is a lock that a pocket or a curious friend cannot open: a salted PBKDF2 record, a
 * comparison that does not stop at the first different byte, and a pause after wrong tries (and after nine in a row, only the password will do).
 *
 * Plain C, no dependencies beyond purr_pbkdf2.h; the shell stores the record (/home/<user>/.config/lock.pin) and supplies the clock.
 */
#ifndef PURR_PIN_H
#define PURR_PIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_PIN_LEN         4
#define PURR_PIN_SALT_LEN    8
#define PURR_PIN_RECORD_MAX  96                 /* "v1:" + 16 hex of salt + ":" + 64 hex of hash + NUL */

/* 1 if `pin` is exactly PURR_PIN_LEN digits. */
int purr_pin_valid(const char *pin);

/* Writes the record for `pin` with `salt` (the shell takes it from the hardware random number generator). 0, or -1 for an invalid PIN or a short buffer. */
int purr_pin_make(const char *pin, const uint8_t salt[PURR_PIN_SALT_LEN], char *out, size_t cap);

/* 1 if `pin` is the PIN the record was made from; 0 for a different PIN, an invalid one, or a record that is damaged. */
int purr_pin_check(const char *record, const char *pin);

/* The pause after wrong tries. Every third wrong try in a row makes the next wait longer: 30 s, 60 s, 120 s, up to 5 minutes. After PURR_PIN_MAX_FAILS in a
 * row the PIN is refused for good until the password has been used (purr_pin_guard_ok after that). `now_ms` is any millisecond clock that wraps at 2^32. */
#define PURR_PIN_MAX_FAILS 9

typedef struct {
    uint8_t fails;                              /* wrong tries in a row */
    uint32_t wait_until_ms;
} purr_pin_guard_t;

uint32_t purr_pin_guard_wait(const purr_pin_guard_t *g, uint32_t now_ms);     /* seconds still to wait, 0 if a try is allowed */
void purr_pin_guard_fail(purr_pin_guard_t *g, uint32_t now_ms);
void purr_pin_guard_ok(purr_pin_guard_t *g);
int purr_pin_guard_locked_out(const purr_pin_guard_t *g);                     /* 1 once only the password will do */

#ifdef __cplusplus
}
#endif

#endif /* PURR_PIN_H */
