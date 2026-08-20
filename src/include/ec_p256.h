/* NexxoN OS - secp256r1 (P-256) ECDH for TLS 1.2 ECDHE */
#ifndef NEXXON_EC_P256_H
#define NEXXON_EC_P256_H

#include "types.h"

#define P256_BYTES 32
#define P256_POINT 65   /* 0x04 || X || Y */

/* priv32 = 32 random bytes (non-zero mod n); peer65 = uncompressed point.
 * shared32 = big-endian X coordinate of priv * peer.  Returns 0 on success. */
int p256_ecdh(const uint8_t priv32[P256_BYTES],
              const uint8_t peer65[P256_POINT],
              uint8_t shared32[P256_BYTES]);

/* Generate uncompressed public key from private scalar. */
int p256_pubkey_from_priv(const uint8_t priv32[P256_BYTES],
                          uint8_t pub65[P256_POINT]);

#endif /* NEXXON_EC_P256_H */
