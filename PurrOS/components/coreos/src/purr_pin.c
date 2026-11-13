#include "purr_pin.h"

#include <string.h>

#include "purr_pbkdf2.h"

#define ITERATIONS 1000u

static const char HEX[] = "0123456789abcdef";

int purr_pin_valid(const char *pin)
{
    if (pin == NULL) {
        return 0;
    }
    for (int i = 0; i < PURR_PIN_LEN; i++) {
        if (pin[i] < '0' || pin[i] > '9') {
            return 0;
        }
    }
    return pin[PURR_PIN_LEN] == '\0';
}

static void hex(char *out, const uint8_t *in, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = HEX[in[i] >> 4];
        out[2 * i + 1] = HEX[in[i] & 15];
    }
}

static int unhex_byte(const char *s)
{
    int v = 0;
    for (int i = 0; i < 2; i++) {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1);
        if (d < 0) {
            return -1;
        }
        v = v * 16 + d;
    }
    return v;
}

int purr_pin_make(const char *pin, const uint8_t salt[PURR_PIN_SALT_LEN], char *out, size_t cap)
{
    if (!purr_pin_valid(pin) || salt == NULL || out == NULL || cap < PURR_PIN_RECORD_MAX) {
        return -1;
    }
    uint8_t h[32];
    purr_pbkdf2_hmac_sha256((const uint8_t *)pin, PURR_PIN_LEN, salt, PURR_PIN_SALT_LEN, ITERATIONS, h);
    memcpy(out, "v1:", 3);
    hex(out + 3, salt, PURR_PIN_SALT_LEN);
    out[3 + 2 * PURR_PIN_SALT_LEN] = ':';
    hex(out + 4 + 2 * PURR_PIN_SALT_LEN, h, sizeof(h));
    out[4 + 2 * PURR_PIN_SALT_LEN + 64] = '\0';
    return 0;
}

int purr_pin_check(const char *record, const char *pin)
{
    if (record == NULL || !purr_pin_valid(pin) || strncmp(record, "v1:", 3) != 0) {
        return 0;
    }
    size_t want = 3 + 2 * PURR_PIN_SALT_LEN + 1 + 64;
    size_t len = strlen(record);
    while (len > want && (record[len - 1] == '\n' || record[len - 1] == '\r')) {
        len--;                                          /* a file's trailing newline */
    }
    if (len != want || record[3 + 2 * PURR_PIN_SALT_LEN] != ':') {
        return 0;
    }
    uint8_t salt[PURR_PIN_SALT_LEN], stored[32], h[32];
    for (int i = 0; i < PURR_PIN_SALT_LEN; i++) {
        int b = unhex_byte(record + 3 + 2 * i);
        if (b < 0) {
            return 0;
        }
        salt[i] = (uint8_t)b;
    }
    for (int i = 0; i < 32; i++) {
        int b = unhex_byte(record + 4 + 2 * PURR_PIN_SALT_LEN + 2 * i);
        if (b < 0) {
            return 0;
        }
        stored[i] = (uint8_t)b;
    }
    purr_pbkdf2_hmac_sha256((const uint8_t *)pin, PURR_PIN_LEN, salt, PURR_PIN_SALT_LEN, ITERATIONS, h);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) {
        diff |= (uint8_t)(h[i] ^ stored[i]);            /* every byte, whatever the first ones say */
    }
    return diff == 0;
}

uint32_t purr_pin_guard_wait(const purr_pin_guard_t *g, uint32_t now_ms)
{
    int32_t left = (int32_t)(g->wait_until_ms - now_ms);
    if (g->fails == 0 || left <= 0) {
        return 0;
    }
    return ((uint32_t)left + 999u) / 1000u;
}

void purr_pin_guard_fail(purr_pin_guard_t *g, uint32_t now_ms)
{
    if (g->fails < 255) {
        g->fails++;
    }
    if (g->fails % 3 == 0) {
        uint32_t step = g->fails / 3;                   /* 1, 2, 3, ... */
        uint32_t secs = 30u << (step > 4 ? 4 : step - 1);
        if (secs > 300u) {
            secs = 300u;
        }
        g->wait_until_ms = now_ms + secs * 1000u;
    }
}

void purr_pin_guard_ok(purr_pin_guard_t *g)
{
    g->fails = 0;
    g->wait_until_ms = 0;
}

int purr_pin_guard_locked_out(const purr_pin_guard_t *g)
{
    return g->fails >= PURR_PIN_MAX_FAILS;
}
