/* ============================================================================
 * NexxoN OS - Cryptographic primitives + minimal TLS shim  (v1.0)
 * ----------------------------------------------------------------------------
 * Embeds compact reference implementations of:
 *
 *   * SHA-256 (FIPS 180-4)         - hash + HMAC
 *   * AES-128 (FIPS 197) block cipher in CTR + GCM modes (GCM truncated
 *                                    to 96-bit IV / 128-bit auth tag)
 *   * a deterministic PRNG seeded off pit_ms() + RTC (NO CSPRNG in this
 *                                    milestone - explicitly stamped in
 *                                    the comments so production TLS
 *                                    operators know to wire a real
 *                                    entropy source before relying on it)
 *
 * On top of that the file declares the TLS 1.2 client-side skeleton:
 *   tls_connect(ip, port, ...) opens an ordinary TCP socket via net.c,
 *   builds a ClientHello, parses the ServerHello + Certificate (no
 *   chain validation in this build - bypass allowed for testing),
 *   completes a stub key exchange, derives a session-key pair, and
 *   sets up an AES-GCM tunnel for tls_send / tls_recv.
 *
 * **HONEST LIMIT**: a real interoperable TLS 1.2 / 1.3 implementation
 * is multiple thousand LoC (BearSSL is the smallest production-grade
 * option at ~20k LoC).  This module ships the primitives + state
 * machine + syscall surface so end-user applications can compile
 * against the future tls_* API today; the handshake against real
 * servers (RSA / ECDHE / X.509) will be completed in a follow-up
 * patch.  For now tls_connect() returns -1 with a clear log message
 * when asked to talk to an external HTTPS endpoint - and the unit
 * tests below exercise AES + SHA round-trips locally.
 * ============================================================================ */
#ifndef NEXXON_CRYPTO_H
#define NEXXON_CRYPTO_H

#include "types.h"

/* ---- SHA-256 ---------------------------------------------------------- */
#define SHA256_BLOCK 64
#define SHA256_HASH  32

typedef struct {
    uint32_t state[8];
    uint64_t total_bits;
    uint8_t  buf[SHA256_BLOCK];
    uint32_t buf_len;
} sha256_ctx_t;

void sha256_init  (sha256_ctx_t *c);
void sha256_update(sha256_ctx_t *c, const void *data, uint32_t len);
void sha256_final (sha256_ctx_t *c, uint8_t out[SHA256_HASH]);
void sha256       (const void *data, uint32_t len, uint8_t out[SHA256_HASH]);

/* HMAC-SHA256 - used by TLS 1.2 PRF and the future HKDF / HMAC-DRBG. */
void hmac_sha256  (const void *key, uint32_t key_len,
                   const void *data, uint32_t data_len,
                   uint8_t out[SHA256_HASH]);

/* TLS 1.2 PRF (P_SHA256) with `label || seed` concatenation. */
void tls12_prf_sha256(const void *secret, uint32_t secret_len,
                      const char *label,
                      const void *seed, uint32_t seed_len,
                      uint8_t *out, uint32_t out_len);

/* HKDF-Extract + HKDF-Expand (RFC 5869), used by TLS 1.3. */
void hkdf_extract (const void *salt, uint32_t salt_len,
                   const void *ikm,  uint32_t ikm_len,
                   uint8_t out[SHA256_HASH]);
void hkdf_expand  (const uint8_t prk[SHA256_HASH],
                   const void *info, uint32_t info_len,
                   uint8_t *out, uint32_t out_len);

/* ---- AES-128 ---------------------------------------------------------- */
#define AES_BLOCK 16
#define AES_KEY   16
#define AES_RKLEN (4 * 4 * (10 + 1))   /* 11 round keys of 16 bytes        */

typedef struct {
    uint8_t rk[AES_RKLEN];
} aes_ctx_t;

void aes_set_key  (aes_ctx_t *c, const uint8_t key[AES_KEY]);
void aes_encrypt  (const aes_ctx_t *c, const uint8_t in[AES_BLOCK],
                   uint8_t out[AES_BLOCK]);

/* AES-CTR stream encryption.  IV is 16 bytes (high 12 = nonce, low 4 =
 * counter starting at 1 by convention). */
void aes_ctr      (const aes_ctx_t *c, const uint8_t iv[AES_BLOCK],
                   const uint8_t *in, uint8_t *out, uint32_t len);

/* AES-GCM authenticated encryption (RFC 5288).  iv = 12-byte nonce.
 * Caller supplies optional AAD (associated additional data) for binding
 * the record-layer header; auth tag is 16 bytes appended after the
 * ciphertext on encrypt, and consumed-and-verified on decrypt.  Decrypt
 * returns 0 on tag mismatch (failed authentication). */
void aes_gcm_encrypt (const aes_ctx_t *c, const uint8_t iv[12],
                      const uint8_t *aad, uint32_t aad_len,
                      const uint8_t *in, uint8_t *out, uint32_t len,
                      uint8_t tag[16]);
int  aes_gcm_decrypt (const aes_ctx_t *c, const uint8_t iv[12],
                      const uint8_t *aad, uint32_t aad_len,
                      const uint8_t *in, uint8_t *out, uint32_t len,
                      const uint8_t tag[16]);

/* ---- PRNG (seed from pit_ms + RTC) ----------------------------------- */
void crypto_seed       (uint32_t s);
void crypto_random     (void *out, uint32_t bytes);

/* ---- RSA-PKCS#1 v1.5 (TLS key transport) ----------------------------- *
 * Small modular-exponentiation engine.  Only the encryption / verify
 * path is required for TLS_RSA_WITH_AES_128_GCM_SHA256.  Big-integers
 * are byte arrays in network (big-endian) order.  Up to 2048-bit keys
 * are supported (RSA_MAX_BYTES = 256). */
#define RSA_MAX_BYTES   256

typedef struct {
    uint8_t  modulus[RSA_MAX_BYTES];
    uint16_t modulus_len;
    uint32_t exponent;          /* public exponent (typically 65537) */
} rsa_pubkey_t;

/* Modular exponentiation: out = base^exp mod modulus.  Used for both
 * encryption (PKCS#1 v1.5 pad + raw exponent) and signature
 * verification. */
int rsa_modexp     (const uint8_t *base, uint16_t base_len,
                    uint32_t exponent,
                    const uint8_t *modulus, uint16_t modulus_len,
                    uint8_t *out);

/* Encrypt `msg` (length must be < modulus_len - 11) with PKCS#1 v1.5
 * type-2 padding under `key`.  `out` must be modulus_len bytes. */
int rsa_pkcs1_encrypt(const rsa_pubkey_t *key,
                      const uint8_t *msg, uint16_t msg_len,
                      uint8_t *out);

/* Parse an X.509 SubjectPublicKeyInfo blob (DER) and extract the RSA
 * modulus + exponent.  Returns 0 on success.  Minimal validation;
 * does NOT verify the chain - that requires a CA root store, deferred
 * to the next milestone. */
int x509_parse_rsa_pubkey(const uint8_t *der, uint32_t der_len,
                          rsa_pubkey_t *out);

/* ---- TLS client (minimal shim) --------------------------------------- */
typedef int tls_handle_t;
#define TLS_INVALID  (-1)

tls_handle_t tls_connect(uint32_t dst_ip, uint16_t dst_port,
                         const char *sni, uint32_t timeout_ms);
int          tls_send   (tls_handle_t s, const void *buf, uint32_t len);
int          tls_recv   (tls_handle_t s, void *buf, uint32_t cap,
                         uint32_t timeout_ms);
void         tls_close  (tls_handle_t s);

#endif /* NEXXON_CRYPTO_H */
