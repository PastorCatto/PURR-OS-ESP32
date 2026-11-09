#include "purr_pbkdf2.h"

#include <string.h>

#include "purr_util.h"

#define BLOCK 64
#define DIGEST 32

void purr_hmac_sha256(const uint8_t *key, size_t key_len,
                      const uint8_t *data, size_t data_len, uint8_t out[32])
{
    uint8_t key_block[BLOCK];
    memset(key_block, 0, sizeof(key_block));
    if (key_len > BLOCK) {
        purr_sha256(key, key_len, key_block);   /* only fills the first 32 bytes */
    } else {
        memcpy(key_block, key, key_len);
    }

    uint8_t ipad[BLOCK], opad[BLOCK];
    for (int i = 0; i < BLOCK; i++) {
        ipad[i] = (uint8_t)(key_block[i] ^ 0x36);
        opad[i] = (uint8_t)(key_block[i] ^ 0x5c);
    }

    purr_sha256_t ctx;
    uint8_t inner[DIGEST];
    purr_sha256_init(&ctx);
    purr_sha256_update(&ctx, ipad, sizeof(ipad));
    purr_sha256_update(&ctx, data, data_len);
    purr_sha256_final(&ctx, inner);

    purr_sha256_init(&ctx);
    purr_sha256_update(&ctx, opad, sizeof(opad));
    purr_sha256_update(&ctx, inner, sizeof(inner));
    purr_sha256_final(&ctx, out);
}

void purr_pbkdf2_hmac_sha256(const uint8_t *password, size_t password_len,
                             const uint8_t *salt, size_t salt_len,
                             uint32_t iterations, uint8_t out[32])
{
    /* One block (PBKDF2's block index 1), which is exactly 32 bytes: salt || 0001. */
    uint8_t buf[256 + 4];                         /* callers use short salts; generous still */
    size_t n = salt_len;
    if (n > sizeof(buf) - 4) {
        n = sizeof(buf) - 4;                      /* refuse to overrun; a real salt is 16 bytes */
    }
    memcpy(buf, salt, n);
    buf[n] = 0;
    buf[n + 1] = 0;
    buf[n + 2] = 0;
    buf[n + 3] = 1;

    uint8_t u[DIGEST], t[DIGEST];
    purr_hmac_sha256(password, password_len, buf, n + 4, u);
    memcpy(t, u, DIGEST);

    for (uint32_t i = 1; i < iterations; i++) {
        purr_hmac_sha256(password, password_len, u, DIGEST, u);
        for (int b = 0; b < DIGEST; b++) {
            t[b] = (uint8_t)(t[b] ^ u[b]);
        }
    }
    memcpy(out, t, DIGEST);
}
