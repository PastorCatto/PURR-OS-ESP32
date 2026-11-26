/* purr_mt_pkc.h - direct messages with public keys, as firmware 2.5 and later send them: X25519 for the shared secret, SHA-256 of it for the key, AES-CCM (an 8 byte tag) for the
 * packet. A packet like that has channel hash 0 in its header and carries ciphertext, the tag, and a four byte extra nonce.
 *
 * The primitives are checked against the standards' vectors (RFC 7748 for X25519, RFC 3610 for CCM, FIPS 180 for SHA-256). How they are put together for a mesh packet (the nonce,
 * what is hashed, where the tag sits) is from memory of the firmware and is marked VERIFY: the first thing to check against a real node. */
#ifndef PURR_MT_PKC_H
#define PURR_MT_PKC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_MT_PKC_OVERHEAD 12                 /* the 8 byte tag and the 4 byte extra nonce after the ciphertext */

/* X25519 (RFC 7748): q = n * p. With p NULL, the base point (the public key of a private key). */
void purr_mt_x25519(uint8_t q[32], const uint8_t n[32], const uint8_t *p);
/* A private key from 32 random bytes (clamped), and its public key. */
void purr_mt_pkc_keypair(const uint8_t random[32], uint8_t priv[32], uint8_t pub[32]);

/* AES-CCM (RFC 3610) with an AES-128 or AES-256 key, a 13 byte nonce, a tag of 4 to 16 bytes. */
int purr_mt_ccm_seal(const uint8_t *key, int keylen, const uint8_t nonce[13], int tag_len, const uint8_t *aad, size_t aad_len, const uint8_t *pt, size_t n, uint8_t *ct, uint8_t *tag);
int purr_mt_ccm_open(const uint8_t *key, int keylen, const uint8_t nonce[13], int tag_len, const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t n, const uint8_t *tag, uint8_t *pt);   /* 0, or -1 if the tag is wrong */

/* A direct message, sealed for the node whose public key is `their_pub`: out = ciphertext, tag, extra nonce (n + PURR_MT_PKC_OVERHEAD bytes). from and id are the header's. */
int purr_mt_pkc_seal(const uint8_t my_priv[32], const uint8_t their_pub[32], uint32_t from, uint32_t id, uint32_t extra_nonce, const uint8_t *pt, size_t n, uint8_t *out);
/* The other direction: in = ciphertext, tag, extra nonce (n bytes in all); the plaintext is n - PURR_MT_PKC_OVERHEAD bytes. Returns that length, or -1 if it does not authenticate. */
int purr_mt_pkc_open(const uint8_t my_priv[32], const uint8_t their_pub[32], uint32_t from, uint32_t id, const uint8_t *in, size_t n, uint8_t *pt);

#ifdef __cplusplus
}
#endif

#endif
