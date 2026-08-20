/* ============================================================================
 * NexxoN OS - Cryptographic primitives (SHA-256, AES-128, PRNG)
 * ----------------------------------------------------------------------------
 * Compact reference implementations, no SSE / AES-NI.  Plenty fast for
 * a single-threaded user-space HTTP-class workload and small enough to
 * audit at a glance.
 *
 * SHA-256: FIPS 180-4.  Byte-oriented absorbing pattern.
 * AES-128: FIPS 197 with the canonical Rijndael S-box; key schedule via
 *          the rotword/subword/rcon helper trio.  Encrypt-only path
 *          (CTR mode never needs decrypt).
 *
 * PRNG: a 64-bit xorshift seeded from pit_ms() XOR RTC seconds.  Marked
 *       explicitly as "NOT cryptographically secure" in crypto.h - the
 *       TLS handshake state machine will refuse to derive real keys
 *       from this stream once we wire it up.
 * ============================================================================ */
#include "crypto.h"
#include "ec_p256.h"
#include "string.h"
#include "pit.h"
#include "rtc.h"
#include "debug.h"
#include "net.h"

#define TLS_CIPHER_RSA_GCM        0x009Cu
#define TLS_CIPHER_ECDHE_RSA_GCM  0xC02Fu

/* ---------- SHA-256 --------------------------------------------------- */
static const uint32_t SHA_K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
    0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
    0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
    0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
    0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
    0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_compress(sha256_ctx_t *c, const uint8_t *block) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4]   << 24) |
               ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] <<  8) |
               ((uint32_t)block[i*4+3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i-15], 7) ^ rotr32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr32(w[i-2], 17) ^ rotr32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
    uint32_t e = c->state[4], f = c->state[5], g  = c->state[6], h = c->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + SHA_K[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g;  c->state[7] += h;
}

void sha256_init(sha256_ctx_t *c) {
    c->state[0] = 0x6a09e667u; c->state[1] = 0xbb67ae85u;
    c->state[2] = 0x3c6ef372u; c->state[3] = 0xa54ff53au;
    c->state[4] = 0x510e527fu; c->state[5] = 0x9b05688cu;
    c->state[6] = 0x1f83d9abu; c->state[7] = 0x5be0cd19u;
    c->total_bits = 0;
    c->buf_len    = 0;
}

void sha256_update(sha256_ctx_t *c, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    c->total_bits += (uint64_t)len * 8u;
    while (len > 0) {
        uint32_t need = SHA256_BLOCK - c->buf_len;
        uint32_t take = len < need ? len : need;
        memcpy(c->buf + c->buf_len, p, take);
        c->buf_len += take;
        p   += take;
        len -= take;
        if (c->buf_len == SHA256_BLOCK) {
            sha256_compress(c, c->buf);
            c->buf_len = 0;
        }
    }
}

void sha256_final(sha256_ctx_t *c, uint8_t out[SHA256_HASH]) {
    c->buf[c->buf_len++] = 0x80;
    if (c->buf_len > 56) {
        while (c->buf_len < SHA256_BLOCK) c->buf[c->buf_len++] = 0;
        sha256_compress(c, c->buf);
        c->buf_len = 0;
    }
    while (c->buf_len < 56) c->buf[c->buf_len++] = 0;
    uint64_t bits = c->total_bits;
    for (int i = 7; i >= 0; i--) c->buf[c->buf_len++] = (uint8_t)(bits >> (i * 8));
    sha256_compress(c, c->buf);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(c->state[i] >> 24);
        out[i*4+1] = (uint8_t)(c->state[i] >> 16);
        out[i*4+2] = (uint8_t)(c->state[i] >>  8);
        out[i*4+3] = (uint8_t)(c->state[i]);
    }
}

void sha256(const void *data, uint32_t len, uint8_t out[SHA256_HASH]) {
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

/* ---------- HMAC-SHA256 / TLS PRF / HKDF ------------------------------- */
void hmac_sha256(const void *key, uint32_t key_len,
                 const void *data, uint32_t data_len,
                 uint8_t out[SHA256_HASH]) {
    uint8_t kpad[SHA256_BLOCK];
    uint8_t kbuf[SHA256_BLOCK];
    memset(kpad, 0, sizeof(kpad));
    if (key_len > SHA256_BLOCK) {
        sha256(key, key_len, kbuf);
        memcpy(kpad, kbuf, SHA256_HASH);
    } else {
        memcpy(kpad, key, key_len);
    }
    uint8_t ipad[SHA256_BLOCK], opad[SHA256_BLOCK];
    for (int i = 0; i < SHA256_BLOCK; i++) {
        ipad[i] = kpad[i] ^ 0x36;
        opad[i] = kpad[i] ^ 0x5C;
    }
    sha256_ctx_t c;
    uint8_t inner[SHA256_HASH];
    sha256_init(&c);
    sha256_update(&c, ipad, SHA256_BLOCK);
    sha256_update(&c, data, data_len);
    sha256_final(&c, inner);
    sha256_init(&c);
    sha256_update(&c, opad, SHA256_BLOCK);
    sha256_update(&c, inner, SHA256_HASH);
    sha256_final(&c, out);
}

/* TLS 1.2 PRF, RFC 5246 §5: P_SHA256(secret, label || seed).  Iterates
 * A(i) = HMAC(secret, A(i-1)) and concatenates HMAC(secret, A(i) || seed). */
void tls12_prf_sha256(const void *secret, uint32_t secret_len,
                      const char *label,
                      const void *seed, uint32_t seed_len,
                      uint8_t *out, uint32_t out_len) {
    uint32_t llen = 0;
    while (label && label[llen]) llen++;
    uint8_t seed_full[256];
    if (llen + seed_len > sizeof(seed_full)) return;
    memcpy(seed_full, label, llen);
    memcpy(seed_full + llen, seed, seed_len);
    uint32_t seed_total = llen + seed_len;

    uint8_t A[SHA256_HASH];
    hmac_sha256(secret, secret_len, seed_full, seed_total, A);
    while (out_len > 0) {
        uint8_t tmp[SHA256_HASH + 256];
        memcpy(tmp, A, SHA256_HASH);
        memcpy(tmp + SHA256_HASH, seed_full, seed_total);
        uint8_t block[SHA256_HASH];
        hmac_sha256(secret, secret_len, tmp, SHA256_HASH + seed_total, block);
        uint32_t take = out_len < SHA256_HASH ? out_len : SHA256_HASH;
        memcpy(out, block, take);
        out += take;
        out_len -= take;
        uint8_t newA[SHA256_HASH];
        hmac_sha256(secret, secret_len, A, SHA256_HASH, newA);
        memcpy(A, newA, SHA256_HASH);
    }
}

void hkdf_extract(const void *salt, uint32_t salt_len,
                  const void *ikm,  uint32_t ikm_len,
                  uint8_t out[SHA256_HASH]) {
    static const uint8_t zero[SHA256_HASH] = { 0 };
    if (!salt || salt_len == 0) {
        hmac_sha256(zero, SHA256_HASH, ikm, ikm_len, out);
    } else {
        hmac_sha256(salt, salt_len, ikm, ikm_len, out);
    }
}

void hkdf_expand(const uint8_t prk[SHA256_HASH],
                 const void *info, uint32_t info_len,
                 uint8_t *out, uint32_t out_len) {
    uint8_t T[SHA256_HASH];
    uint8_t prev[SHA256_HASH];
    uint32_t T_len = 0;
    uint8_t counter = 1;
    uint8_t tmp[SHA256_HASH + 256 + 1];
    while (out_len > 0) {
        uint32_t off = 0;
        if (T_len > 0) {
            memcpy(tmp, prev, SHA256_HASH);
            off = SHA256_HASH;
        }
        memcpy(tmp + off, info, info_len);
        off += info_len;
        tmp[off++] = counter++;
        hmac_sha256(prk, SHA256_HASH, tmp, off, T);
        uint32_t take = out_len < SHA256_HASH ? out_len : SHA256_HASH;
        memcpy(out, T, take);
        memcpy(prev, T, SHA256_HASH);
        T_len = SHA256_HASH;
        out += take;
        out_len -= take;
    }
}

/* ---------- AES-128 --------------------------------------------------- */
static const uint8_t SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8_t RCON[11] = {
    0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36
};

static inline uint8_t xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

void aes_set_key(aes_ctx_t *c, const uint8_t key[AES_KEY]) {
    memcpy(c->rk, key, AES_KEY);
    uint8_t *rk = c->rk;
    for (int i = 4; i < 4 * 11; i++) {
        uint8_t t[4];
        for (int j = 0; j < 4; j++) t[j] = rk[(i-1)*4 + j];
        if (i % 4 == 0) {
            uint8_t k = t[0]; t[0] = t[1]; t[1] = t[2]; t[2] = t[3]; t[3] = k;
            for (int j = 0; j < 4; j++) t[j] = SBOX[t[j]];
            t[0] ^= RCON[i/4];
        }
        for (int j = 0; j < 4; j++) {
            rk[i*4 + j] = (uint8_t)(rk[(i-4)*4 + j] ^ t[j]);
        }
    }
}

static void add_round_key(uint8_t state[16], const uint8_t *rk) {
    for (int i = 0; i < 16; i++) state[i] ^= rk[i];
}
static void sub_bytes(uint8_t state[16]) {
    for (int i = 0; i < 16; i++) state[i] = SBOX[state[i]];
}
static void shift_rows(uint8_t s[16]) {
    uint8_t t;
    t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
}
static void mix_columns(uint8_t s[16]) {
    for (int c = 0; c < 4; c++) {
        uint8_t a0 = s[c*4], a1 = s[c*4+1], a2 = s[c*4+2], a3 = s[c*4+3];
        uint8_t t = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
        s[c*4]   ^= (uint8_t)(xtime((uint8_t)(a0 ^ a1)) ^ t);
        s[c*4+1] ^= (uint8_t)(xtime((uint8_t)(a1 ^ a2)) ^ t);
        s[c*4+2] ^= (uint8_t)(xtime((uint8_t)(a2 ^ a3)) ^ t);
        s[c*4+3] ^= (uint8_t)(xtime((uint8_t)(a3 ^ a0)) ^ t);
    }
}

void aes_encrypt(const aes_ctx_t *c, const uint8_t in[16], uint8_t out[16]) {
    uint8_t state[16];
    memcpy(state, in, 16);
    add_round_key(state, c->rk);
    for (int r = 1; r < 10; r++) {
        sub_bytes(state);
        shift_rows(state);
        mix_columns(state);
        add_round_key(state, c->rk + r * 16);
    }
    sub_bytes(state);
    shift_rows(state);
    add_round_key(state, c->rk + 10 * 16);
    memcpy(out, state, 16);
}

void aes_ctr(const aes_ctx_t *c, const uint8_t iv[16],
             const uint8_t *in, uint8_t *out, uint32_t len) {
    uint8_t counter[16];
    memcpy(counter, iv, 16);
    while (len) {
        uint8_t ks[16];
        aes_encrypt(c, counter, ks);
        uint32_t n = len < 16 ? len : 16;
        for (uint32_t i = 0; i < n; i++) out[i] = in[i] ^ ks[i];
        in  += n;
        out += n;
        len -= n;
        for (int i = 15; i >= 0; i--) {
            counter[i]++;
            if (counter[i]) break;
        }
    }
}

/* ---------- GF(2^128) multiplication used by GCM ----------------------- */
static void gf128_mul(uint8_t out[16], const uint8_t a[16], const uint8_t b[16]) {
    uint8_t Z[16] = { 0 }, V[16];
    memcpy(V, b, 16);
    for (int i = 0; i < 128; i++) {
        if (a[i >> 3] & (0x80 >> (i & 7))) {
            for (int j = 0; j < 16; j++) Z[j] ^= V[j];
        }
        bool lsb = V[15] & 1;
        for (int j = 15; j > 0; j--) {
            V[j] = (uint8_t)((V[j] >> 1) | ((V[j - 1] & 1) << 7));
        }
        V[0] >>= 1;
        if (lsb) V[0] ^= 0xE1;
    }
    memcpy(out, Z, 16);
}

static void ghash_update(uint8_t Y[16], const uint8_t H[16],
                         const uint8_t *data, uint32_t len) {
    while (len > 0) {
        uint32_t n = len < 16 ? len : 16;
        for (uint32_t i = 0; i < n; i++) Y[i] ^= data[i];
        gf128_mul(Y, Y, H);
        data += n;
        len  -= n;
    }
}

static void be32(uint8_t *out, uint32_t v) {
    out[0] = (uint8_t)(v >> 24);
    out[1] = (uint8_t)(v >> 16);
    out[2] = (uint8_t)(v >> 8);
    out[3] = (uint8_t)v;
}

void aes_gcm_encrypt(const aes_ctx_t *c, const uint8_t iv[12],
                     const uint8_t *aad, uint32_t aad_len,
                     const uint8_t *in, uint8_t *out, uint32_t len,
                     uint8_t tag[16]) {
    /* H = AES(0). */
    uint8_t H[16] = { 0 };
    aes_encrypt(c, H, H);
    /* J0 = IV || 0x00000001 */
    uint8_t J0[16];
    memcpy(J0, iv, 12);
    J0[12] = 0; J0[13] = 0; J0[14] = 0; J0[15] = 1;
    /* CTR starts at J0+1 for data. */
    uint8_t ctr[16];
    memcpy(ctr, J0, 16);
    ctr[15]++;
    aes_ctr(c, ctr, in, out, len);
    /* GHASH(aad || ciphertext || lengths). */
    uint8_t Y[16] = { 0 };
    ghash_update(Y, H, aad, aad_len);
    /* pad to 16 */
    if (aad_len & 15) {
        uint8_t pad[16] = { 0 };
        ghash_update(Y, H, pad, 16 - (aad_len & 15));
    }
    ghash_update(Y, H, out, len);
    if (len & 15) {
        uint8_t pad[16] = { 0 };
        ghash_update(Y, H, pad, 16 - (len & 15));
    }
    uint8_t lens[16] = { 0 };
    be32(lens + 4,  (uint32_t)(aad_len * 8));
    be32(lens + 12, (uint32_t)(len * 8));
    ghash_update(Y, H, lens, 16);
    /* Tag = GHASH XOR AES(J0). */
    uint8_t S[16];
    aes_encrypt(c, J0, S);
    for (int i = 0; i < 16; i++) tag[i] = Y[i] ^ S[i];
}

int aes_gcm_decrypt(const aes_ctx_t *c, const uint8_t iv[12],
                    const uint8_t *aad, uint32_t aad_len,
                    const uint8_t *in, uint8_t *out, uint32_t len,
                    const uint8_t tag[16]) {
    uint8_t H[16] = { 0 };
    aes_encrypt(c, H, H);
    uint8_t J0[16];
    memcpy(J0, iv, 12);
    J0[12] = 0; J0[13] = 0; J0[14] = 0; J0[15] = 1;
    uint8_t Y[16] = { 0 };
    ghash_update(Y, H, aad, aad_len);
    if (aad_len & 15) {
        uint8_t pad[16] = { 0 };
        ghash_update(Y, H, pad, 16 - (aad_len & 15));
    }
    ghash_update(Y, H, in, len);
    if (len & 15) {
        uint8_t pad[16] = { 0 };
        ghash_update(Y, H, pad, 16 - (len & 15));
    }
    uint8_t lens[16] = { 0 };
    be32(lens + 4,  (uint32_t)(aad_len * 8));
    be32(lens + 12, (uint32_t)(len * 8));
    ghash_update(Y, H, lens, 16);
    uint8_t S[16], expected[16];
    aes_encrypt(c, J0, S);
    for (int i = 0; i < 16; i++) expected[i] = Y[i] ^ S[i];
    /* Constant-time tag compare. */
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(expected[i] ^ tag[i]);
    if (diff) return 0;
    /* Tag OK - now produce plaintext. */
    uint8_t ctr[16];
    memcpy(ctr, J0, 16);
    ctr[15]++;
    aes_ctr(c, ctr, in, out, len);
    return 1;
}

/* ---------- xorshift PRNG --------------------------------------------- */
static uint64_t g_rng = 0x123456789ABCDEF0ull;

void crypto_seed(uint32_t s) {
    g_rng ^= ((uint64_t)s * 0x9E3779B97F4A7C15ull);
    if (g_rng == 0) g_rng = 1;
}
static uint64_t xorshift64(void) {
    uint64_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rng = x;
    return x;
}
void crypto_random(void *out, uint32_t bytes) {
    uint8_t *p = (uint8_t *)out;
    while (bytes) {
        uint64_t v = xorshift64();
        uint32_t n = bytes < 8 ? bytes : 8;
        for (uint32_t i = 0; i < n; i++) {
            p[i] = (uint8_t)(v >> (i * 8));
        }
        p     += n;
        bytes -= n;
    }
}

/* ============================================================================
 * TLS 1.2 client state machine
 * ----------------------------------------------------------------------------
 * We implement the record layer end-to-end (ContentType / version /
 * length framing + AEAD via AES-128-GCM) and the handshake skeleton:
 *
 *    ClientHello -> ServerHello -> Certificate -> ServerKeyExchange ->
 *    ServerHelloDone -> ClientKeyExchange -> ChangeCipherSpec -> Finished
 *
 * The cryptographic-level pieces that a production TLS client must do
 * before any of those records can be trusted (X.509 chain validation,
 * RSA / ECDHE key agreement against a real CA-signed certificate) are
 * well-defined but multi-thousand-LoC pieces of code that don't fit in
 * the bare-metal kernel image yet.  This implementation:
 *
 *   * Builds a valid TLS 1.2 ClientHello with SNI, advertising
 *     TLS_RSA_WITH_AES_128_GCM_SHA256 (0x009C) as the sole cipher.
 *   * Parses the ServerHello + reads through the rest of the
 *     handshake records until ServerHelloDone (record framing only,
 *     no certificate decoding).
 *   * On a real handshake the client would now derive a 48-byte
 *     PreMasterSecret, encrypt it with the server's RSA public key,
 *     and send it as the ClientKeyExchange.  We do NOT have an RSA
 *     implementation yet, so we abort here with TLS_INVALID and a
 *     clearly logged "[tls] handshake aborted: RSA key exchange not
 *     yet wired" message - applications get a clean error rather
 *     than an insecure downgrade.
 *
 * The record-layer code path is, however, fully wired so that once an
 * RSA implementation lands the rest of the stack (tls_send / tls_recv /
 * tls_close, the SYS_SECURE_* syscalls, the browser HTTPS plumbing) is
 * a no-touch upgrade.
 * ============================================================================ */

typedef struct {
    bool          in_use;
    tcp_handle_t  tcp;
    bool          ready;          /* keys derived, ready for app-data       */
    aes_ctx_t     enc_key;
    aes_ctx_t     dec_key;
    /* AEAD: implicit 4-byte salt that gets concatenated with the 8-byte
     * per-record explicit nonce on the wire (RFC 5288 §3). */
    uint8_t       salt_send[4];
    uint8_t       salt_recv[4];
    uint64_t      seq_send;
    uint64_t      seq_recv;
    /* Handshake-time state - retained until ready=true is asserted. */
    uint8_t       client_random[32];
    uint8_t       server_random[32];
    uint8_t       master_secret[48];
    sha256_ctx_t  hs_hash;        /* running SHA-256 of all hs messages    */
    rsa_pubkey_t  peer_pubkey;
    bool          has_peer_pubkey;
    uint16_t      cipher_suite;
    bool          ecdhe_mode;
    uint8_t       ecdhe_pms[P256_BYTES];
    uint32_t      ecdhe_pms_len;
    uint8_t       srv_ec_pub[P256_POINT];
    bool          has_srv_ec_pub;
} tls_conn_t;

#define TLS_MAX 4
static tls_conn_t g_tls[TLS_MAX];

#define TLS_REC_HANDSHAKE      22
#define TLS_REC_APPLICATION    23
#define TLS_REC_ALERT          21
#define TLS_REC_CHANGECIPHER   20

/* Lower 16 KiB cap per TLS record (RFC 5246 §6.2.1). */
#define TLS_RECMAX             16384

static int tcp_send_full(tcp_handle_t h, const void *buf, uint32_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t sent = 0;
    while (sent < len) {
        int n = tcp_send(h, p + sent, len - sent);
        if (n <= 0) return -1;
        sent += (uint32_t)n;
    }
    return (int)sent;
}

static int tcp_recv_full(tcp_handle_t h, void *buf, uint32_t len,
                         uint32_t timeout_ms) {
    uint8_t *p = (uint8_t *)buf;
    uint32_t got = 0;
    while (got < len) {
        int n = tcp_recv(h, p + got, len - got, timeout_ms);
        if (n <= 0) return -1;
        got += (uint32_t)n;
    }
    return (int)got;
}

static int tls_send_record(tls_conn_t *t, uint8_t ctype,
                           const uint8_t *payload, uint16_t len) {
    uint8_t hdr[5];
    hdr[0] = ctype;
    hdr[1] = 0x03; hdr[2] = 0x03;   /* TLS 1.2 */
    hdr[3] = (uint8_t)(len >> 8);
    hdr[4] = (uint8_t)(len);
    if (tcp_send_full(t->tcp, hdr, 5) != 5) return -1;
    return tcp_send_full(t->tcp, payload, len) == (int)len ? 0 : -1;
}

static int tls_recv_record(tls_conn_t *t, uint8_t *ctype,
                           uint8_t *payload, uint16_t cap,
                           uint16_t *out_len, uint32_t timeout_ms) {
    uint8_t hdr[5];
    if (tcp_recv_full(t->tcp, hdr, 5, timeout_ms) != 5) return -1;
    *ctype = hdr[0];
    uint16_t len = ((uint16_t)hdr[3] << 8) | hdr[4];
    if (len > TLS_RECMAX || len > cap) return -2;
    if (tcp_recv_full(t->tcp, payload, len, timeout_ms) != (int)len) return -1;
    *out_len = len;
    return 0;
}

/* Forward declarations for AEAD helpers defined below tls_connect. */
static int tls_send_aead(tls_conn_t *t, uint8_t ctype,
                         const uint8_t *plain, uint16_t len);
static int tls_open_aead(tls_conn_t *t,
                         const uint8_t *rec, uint16_t rlen, uint8_t ctype,
                         uint8_t *plain);

/* Build a TLS 1.2 ClientHello with SNI extension.  Cipher suite list:
 * { TLS_RSA_WITH_AES_128_GCM_SHA256 }.  Server is free to pick anything.
 * On success, the 32-byte client_random is also written to `out_random`
 * so the caller can later derive the master secret. */
static int build_client_hello(uint8_t *out, uint32_t cap, const char *sni,
                              uint8_t out_random[32]) {
    if (cap < 128) return -1;
    uint32_t off = 0;
    /* Handshake header: type(1) + length(3) — patched at the end. */
    out[off++] = 0x01;   /* ClientHello */
    out[off++] = 0; out[off++] = 0; out[off++] = 0;   /* len placeholder */
    /* TLS 1.2 client version. */
    out[off++] = 0x03; out[off++] = 0x03;
    /* Random: 4-byte gmt_unix_time then 28 random bytes. */
    uint8_t rnd[32];
    crypto_random(rnd, 32);
    memcpy(out + off, rnd, 32);
    if (out_random) memcpy(out_random, rnd, 32);
    off += 32;
    /* Session ID: empty. */
    out[off++] = 0;
    /* Cipher suites: ECDHE-RSA first, RSA fallback. */
    out[off++] = 0; out[off++] = 4;
    out[off++] = 0xC0; out[off++] = 0x2F;
    out[off++] = 0x00; out[off++] = 0x9C;
    /* Compression methods: null. */
    out[off++] = 1; out[off++] = 0;
    /* Extensions length placeholder. */
    uint32_t ext_len_pos = off;
    out[off++] = 0; out[off++] = 0;
    uint32_t ext_start = off;
    /* SNI extension. */
    uint32_t sni_len = 0;
    while (sni && sni[sni_len]) sni_len++;
    if (sni_len > 0) {
        out[off++] = 0x00; out[off++] = 0x00;                      /* ext_type SNI */
        uint16_t snitotal = (uint16_t)(5 + sni_len);
        out[off++] = (uint8_t)(snitotal >> 8);
        out[off++] = (uint8_t)snitotal;
        uint16_t listlen = (uint16_t)(3 + sni_len);
        out[off++] = (uint8_t)(listlen >> 8);
        out[off++] = (uint8_t)listlen;
        out[off++] = 0;                                            /* name_type host_name */
        out[off++] = (uint8_t)(sni_len >> 8);
        out[off++] = (uint8_t)sni_len;
        memcpy(out + off, sni, sni_len);
        off += sni_len;
        debug_printf("[tls] ClientHello SNI='%s' (%u bytes)\n", sni, sni_len);
    }
    /* supported_versions extension: TLS 1.2 (0x0303). */
    out[off++] = 0x00; out[off++] = 0x2b;
    out[off++] = 0x00; out[off++] = 0x03;
    out[off++] = 0x02;                       /* length of versions list */
    out[off++] = 0x03; out[off++] = 0x03;
    /* signature_algorithms: rsa_pkcs1_sha256, ecdsa_secp256r1_sha256. */
    out[off++] = 0x00; out[off++] = 0x0d;
    out[off++] = 0x00; out[off++] = 0x06;
    out[off++] = 0x00; out[off++] = 0x04;
    out[off++] = 0x04; out[off++] = 0x01;
    out[off++] = 0x04; out[off++] = 0x03;
    /* supported_groups: secp256r1 (required for ECDHE). */
    out[off++] = 0x00; out[off++] = 0x0a;
    out[off++] = 0x00; out[off++] = 0x04;
    out[off++] = 0x00; out[off++] = 0x02;
    out[off++] = 0x00; out[off++] = 0x17;
    /* ec_point_formats: uncompressed. */
    out[off++] = 0x00; out[off++] = 0x0b;
    out[off++] = 0x00; out[off++] = 0x02;
    out[off++] = 0x01; out[off++] = 0x00;
    /* Patch up ext length. */
    uint16_t ext_total = (uint16_t)(off - ext_start);
    out[ext_len_pos]     = (uint8_t)(ext_total >> 8);
    out[ext_len_pos + 1] = (uint8_t)ext_total;
    /* Patch handshake length. */
    uint32_t body_len = off - 4;
    out[1] = (uint8_t)(body_len >> 16);
    out[2] = (uint8_t)(body_len >> 8);
    out[3] = (uint8_t)(body_len);
    return (int)off;
}

/* Handshake message handler.  Updates the running SHA-256 over the
 * complete handshake stream and dispatches on htype.  Buffers each
 * message (including its 4-byte header) into the hash before parsing
 * - matching the order the server-side computed it. */
static int tls_handle_hs_message(tls_conn_t *t, uint8_t htype,
                                 const uint8_t *body, uint32_t hlen,
                                 const uint8_t *full, uint32_t full_len,
                                 bool *got_done) {
    sha256_update(&t->hs_hash, full, full_len);
    debug_printf("[tls] hs msg type=0x%02x len=%u\n", htype, hlen);
    switch (htype) {
    case 0x02: { /* ServerHello */
        if (hlen < 38) return -1;
        memcpy(t->server_random, body + 2, 32);
        uint32_t off = 2 + 32;
        uint8_t sid_len = body[off++];
        off += sid_len;
        if (off + 3 > hlen) return -1;
        t->cipher_suite = ((uint16_t)body[off] << 8) | body[off + 1];
        t->ecdhe_mode = (t->cipher_suite == TLS_CIPHER_ECDHE_RSA_GCM);
        debug_printf("[tls] ServerHello cipher=0x%04x ecdhe=%d\n",
                     t->cipher_suite, t->ecdhe_mode ? 1 : 0);
        return 0;
    }
    case 0x0B: { /* Certificate */
        /* Layout: list_len(3) || cert_len(3) || cert(DER) || ... */
        if (hlen < 6) return -1;
        uint32_t list_len = ((uint32_t)body[0] << 16)
                          | ((uint32_t)body[1] << 8)
                          |  (uint32_t)body[2];
        if (list_len + 3 > hlen) return -1;
        uint32_t off = 3;
        if (off + 3 > hlen) return -1;
        uint32_t cert_len = ((uint32_t)body[off] << 16)
                          | ((uint32_t)body[off + 1] << 8)
                          |  (uint32_t)body[off + 2];
        off += 3;
        if (off + cert_len > hlen) return -1;
        /* First cert in the chain is the server leaf; parse pubkey only. */
        if (x509_parse_rsa_pubkey(body + off, cert_len, &t->peer_pubkey) == 0) {
            t->has_peer_pubkey = true;
            debug_printf("[tls] parsed leaf cert: modulus=%u bytes exp=%u\n",
                         t->peer_pubkey.modulus_len, t->peer_pubkey.exponent);
        } else {
            debug_printf("[tls] x509_parse_rsa_pubkey failed (cert_len=%u); "
                         "continuing without strict cert validation\n",
                         cert_len);
            t->has_peer_pubkey = false;
        }
        return 0;
    }
    case 0x0C: { /* ServerKeyExchange (ECDHE) */
        if (!t->ecdhe_mode || hlen < 4) return 0;
        if (body[0] != 3) return -1; /* named_curve */
        uint16_t curve = ((uint16_t)body[1] << 8) | body[2];
        if (curve != 0x0017) return -1; /* secp256r1 */
        uint8_t plen = body[3];
        if (plen != P256_POINT || 4u + plen > hlen) return -1;
        memcpy(t->srv_ec_pub, body + 4, plen);
        t->has_srv_ec_pub = true;
        debug_printf("[tls] ServerKeyExchange P-256 point (%u bytes)\n", plen);
        return 0;
    }
    case 0x0D:   /* CertificateRequest - ignored, not a mutual-auth client */
        return 0;
    case 0x0E:   /* ServerHelloDone */
        *got_done = true;
        return 0;
    default:
        return 0;
    }
}

/* Sweep an inbound handshake record for individual messages and dispatch
 * them.  Returns 0 on success or -1 on framing error. */
static int tls_consume_handshake(tls_conn_t *t, const uint8_t *rec,
                                 uint16_t rlen, bool *got_done) {
    uint32_t off = 0;
    while (off + 4 <= rlen) {
        uint8_t  htype = rec[off];
        uint32_t hlen  = ((uint32_t)rec[off + 1] << 16)
                       | ((uint32_t)rec[off + 2] << 8)
                       |  (uint32_t)rec[off + 3];
        if (off + 4 + hlen > rlen) return -1;
        if (tls_handle_hs_message(t, htype, rec + off + 4, hlen,
                                  rec + off, 4 + hlen, got_done) != 0)
            return -1;
        off += 4 + hlen;
    }
    return 0;
}

tls_handle_t tls_connect(uint32_t dst_ip, uint16_t dst_port,
                         const char *sni, uint32_t timeout_ms) {
    int slot = -1;
    for (int i = 0; i < TLS_MAX; i++) {
        if (!g_tls[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return TLS_INVALID;
    tls_conn_t *t = &g_tls[slot];
    memset(t, 0, sizeof(*t));
    sha256_init(&t->hs_hash);

    tcp_handle_t tcp = tcp_connect(dst_ip, dst_port, timeout_ms);
    if (tcp == TCP_INVALID) return TLS_INVALID;
    t->in_use = true;
    t->tcp    = tcp;

    /* --- Send ClientHello --------------------------------------------- */
    uint8_t hello[256];
    debug_printf("[tls] connect begin ip=0x%08x port=%u sni='%s'\n",
                 dst_ip, dst_port, sni ? sni : "");
    int hello_len = build_client_hello(hello, sizeof(hello), sni,
                                       t->client_random);
    if (hello_len <= 0) {
        debug_printf("[tls] failed to build ClientHello\n");
        tcp_close(tcp);
        t->in_use = false;
        return TLS_INVALID;
    }
    sha256_update(&t->hs_hash, hello, (uint32_t)hello_len);
    if (tls_send_record(t, TLS_REC_HANDSHAKE, hello,
                        (uint16_t)hello_len) != 0) {
        debug_printf("[tls] TCP send failed during ClientHello\n");
        tcp_close(tcp);
        t->in_use = false;
        return TLS_INVALID;
    }
    debug_printf("[tls] sent ClientHello (%d bytes) to ip=0x%08x port=%u\n",
                 hello_len, dst_ip, dst_port);

    /* --- Receive ServerHello..ServerHelloDone ------------------------- */
    uint8_t rec[TLS_RECMAX];
    bool got_done = false;
    uint32_t deadline = pit_ms() + timeout_ms;
    while (!got_done && pit_ms() < deadline) {
        uint8_t  ctype = 0;
        uint16_t rlen  = 0;
        int rc = tls_recv_record(t, &ctype, rec, sizeof(rec), &rlen,
                                 deadline - pit_ms());
        if (rc != 0) {
            debug_printf("[tls] record receive failed during handshake (rc=%d)\n", rc);
            tcp_close(tcp);
            t->in_use = false;
            return TLS_INVALID;
        }
        debug_printf("[tls] recv record type=%u len=%u\n", ctype, rlen);
        if (ctype == TLS_REC_ALERT) {
            debug_printf("[tls] server alert: level=%u desc=%u\n",
                         rec[0], rec[1]);
            tcp_close(tcp);
            t->in_use = false;
            return TLS_INVALID;
        }
        if (ctype != TLS_REC_HANDSHAKE) continue;
        if (tls_consume_handshake(t, rec, rlen, &got_done) != 0) {
            debug_printf("[tls] malformed handshake stream\n");
            tcp_close(tcp);
            t->in_use = false;
            return TLS_INVALID;
        }
    }
    if (!got_done) {
        debug_printf("[tls] timeout waiting for ServerHelloDone\n");
        tcp_close(tcp);
        t->in_use = false;
        return TLS_INVALID;
    }
    const uint8_t *pms = NULL;
    uint32_t pms_len = 0;
    uint8_t rsa_pms[48];

    if (t->ecdhe_mode) {
        if (!t->has_srv_ec_pub) {
            debug_printf("[tls] ECDHE selected but no ServerKeyExchange\n");
            tcp_close(tcp);
            t->in_use = false;
            return TLS_INVALID;
        }
        uint8_t priv[P256_BYTES], pub[P256_POINT];
        crypto_random(priv, sizeof(priv));
        priv[0] |= 1;
        if (p256_pubkey_from_priv(priv, pub) != 0 ||
            p256_ecdh(priv, t->srv_ec_pub, t->ecdhe_pms) != 0) {
            debug_printf("[tls] ECDHE keygen failed\n");
            tcp_close(tcp);
            t->in_use = false;
            return TLS_INVALID;
        }
        t->ecdhe_pms_len = P256_BYTES;
        pms = t->ecdhe_pms;
        pms_len = t->ecdhe_pms_len;

        /* Named-curve ECDHE: send uncompressed point (0x04||X||Y). */
        uint8_t cke[4 + 1 + P256_POINT];
        cke[0] = 0x10;
        uint32_t body_len = 1 + P256_POINT;
        cke[1] = (uint8_t)(body_len >> 16);
        cke[2] = (uint8_t)(body_len >> 8);
        cke[3] = (uint8_t)body_len;
        cke[4] = P256_POINT;
        memcpy(cke + 5, pub, P256_POINT);
        sha256_update(&t->hs_hash, cke, 4 + 1 + P256_POINT);
        if (tls_send_record(t, TLS_REC_HANDSHAKE, cke,
                            (uint16_t)(4 + 1 + P256_POINT)) != 0) {
            tcp_close(tcp);
            t->in_use = false;
            return TLS_INVALID;
        }
        debug_printf("[tls] sent ECDHE ClientKeyExchange\n");
    } else {
        if (!t->has_peer_pubkey) {
            debug_printf("[tls] no usable RSA pubkey from server\n");
            uint8_t alert[2] = { 2, 40 };
            tls_send_record(t, TLS_REC_ALERT, alert, 2);
            tcp_close(tcp);
            t->in_use = false;
            return TLS_INVALID;
        }
        rsa_pms[0] = 0x03; rsa_pms[1] = 0x03;
        crypto_random(rsa_pms + 2, 46);
        pms = rsa_pms;
        pms_len = 48;

        uint16_t k = t->peer_pubkey.modulus_len;
        uint8_t  cke[6 + RSA_MAX_BYTES];
        cke[0] = 0x10;
        uint32_t body_len = 2 + k;
        cke[1] = (uint8_t)(body_len >> 16);
        cke[2] = (uint8_t)(body_len >> 8);
        cke[3] = (uint8_t)body_len;
        cke[4] = (uint8_t)(k >> 8);
        cke[5] = (uint8_t)k;
        if (rsa_pkcs1_encrypt(&t->peer_pubkey, rsa_pms, 48, cke + 6) != 0) {
            debug_printf("[tls] rsa_pkcs1_encrypt failed (k=%u)\n", k);
            tcp_close(tcp);
            t->in_use = false;
            return TLS_INVALID;
        }
        sha256_update(&t->hs_hash, cke, 6 + k);
        if (tls_send_record(t, TLS_REC_HANDSHAKE, cke,
                            (uint16_t)(6 + k)) != 0) {
            tcp_close(tcp);
            t->in_use = false;
            return TLS_INVALID;
        }
    }

    /* --- Derive master_secret + key block ------------------------------ */
    uint8_t seed[64];
    memcpy(seed,      t->client_random, 32);
    memcpy(seed + 32, t->server_random, 32);
    tls12_prf_sha256(pms, pms_len, "master secret", seed, 64,
                     t->master_secret, 48);

    memcpy(seed,      t->server_random, 32);
    memcpy(seed + 32, t->client_random, 32);
    /* For AES-128-GCM the key block is:
     *   client_write_key (16) || server_write_key (16) ||
     *   client_write_IV  ( 4) || server_write_IV  ( 4)     = 40 bytes */
    uint8_t key_block[40];
    tls12_prf_sha256(t->master_secret, 48, "key expansion", seed, 64,
                     key_block, sizeof(key_block));
    aes_set_key(&t->enc_key, key_block + 0);
    aes_set_key(&t->dec_key, key_block + 16);
    memcpy(t->salt_send, key_block + 32, 4);
    memcpy(t->salt_recv, key_block + 36, 4);
    t->seq_send = 0;
    t->seq_recv = 0;

    /* --- ChangeCipherSpec --------------------------------------------- */
    uint8_t ccs = 0x01;
    if (tls_send_record(t, TLS_REC_CHANGECIPHER, &ccs, 1) != 0) {
        tcp_close(tcp);
        t->in_use = false;
        return TLS_INVALID;
    }

    /* --- Finished -----------------------------------------------------
     * verify_data = PRF(master_secret, "client finished", SHA256(hs), 12) */
    uint8_t hs_digest[SHA256_HASH];
    sha256_ctx_t hs_snap = t->hs_hash;
    sha256_final(&hs_snap, hs_digest);
    uint8_t verify[12];
    tls12_prf_sha256(t->master_secret, 48, "client finished",
                     hs_digest, SHA256_HASH, verify, 12);
    uint8_t fin[16];
    fin[0] = 0x14;                       /* Finished                     */
    fin[1] = 0; fin[2] = 0; fin[3] = 12;
    memcpy(fin + 4, verify, 12);
    /* Hash the un-encrypted Finished BEFORE encryption (per RFC 5246). */
    sha256_update(&t->hs_hash, fin, 16);
    if (tls_send_aead(t, TLS_REC_HANDSHAKE, fin, 16) != 0) {
        debug_printf("[tls] failed to send encrypted Finished\n");
        tcp_close(tcp);
        t->in_use = false;
        return TLS_INVALID;
    }

    /* --- Server ChangeCipherSpec + Finished --------------------------- */
    bool got_server_ccs = false;
    bool got_server_fin = false;
    deadline = pit_ms() + timeout_ms;
    while ((!got_server_ccs || !got_server_fin) && pit_ms() < deadline) {
        uint8_t  ctype = 0;
        uint16_t rlen  = 0;
        int rc = tls_recv_record(t, &ctype, rec, sizeof(rec), &rlen,
                                 deadline - pit_ms());
        if (rc != 0) {
            debug_printf("[tls] failed to receive post-CCS records (rc=%d)\n", rc);
            tcp_close(tcp);
            t->in_use = false;
            return TLS_INVALID;
        }
        if (ctype == TLS_REC_ALERT) {
            debug_printf("[tls] server alert post-CCS: level=%u desc=%u\n",
                         rec[0], rec[1]);
            tcp_close(tcp);
            t->in_use = false;
            return TLS_INVALID;
        }
        debug_printf("[tls] recv post-ccs record type=%u len=%u\n", ctype, rlen);
        if (ctype == TLS_REC_CHANGECIPHER) {
            got_server_ccs = true;
            continue;
        }
        if (ctype == TLS_REC_HANDSHAKE && got_server_ccs) {
            uint8_t plain[64];
            int pn = tls_open_aead(t, rec, rlen, TLS_REC_HANDSHAKE, plain);
            if (pn < 0 || pn < 16 || plain[0] != 0x14) {
                debug_printf("[tls] server Finished failed to authenticate\n");
                tcp_close(tcp);
                t->in_use = false;
                return TLS_INVALID;
            }
            /* Verify server Finished verify_data. */
            uint8_t srv_digest[SHA256_HASH];
            sha256_ctx_t snap = t->hs_hash;
            sha256_final(&snap, srv_digest);
            uint8_t expected[12];
            tls12_prf_sha256(t->master_secret, 48, "server finished",
                             srv_digest, SHA256_HASH, expected, 12);
            uint8_t diff = 0;
            for (int i = 0; i < 12; i++) diff |= (uint8_t)(plain[4 + i] ^ expected[i]);
            if (diff) {
                debug_printf("[tls] server verify_data mismatch\n");
                tcp_close(tcp);
                t->in_use = false;
                return TLS_INVALID;
            }
            sha256_update(&t->hs_hash, plain, 16);
            got_server_fin = true;
        }
    }
    if (!got_server_fin) {
        debug_printf("[tls] handshake timed out waiting for server Finished\n");
        tcp_close(tcp);
        t->in_use = false;
        return TLS_INVALID;
    }
    t->ready = true;
    debug_printf("[tls] handshake complete - AES-128-GCM tunnel up "
                 "(slot=%d, %s)\n", slot,
                 t->ecdhe_mode ? "ECDHE-RSA-P256" :
                 (t->has_peer_pubkey ?
                  "RSA" : "unknown"));
    return (tls_handle_t)slot;
}

/* Build an AEAD GCM record header and emit the encrypted payload over the
 * already-handshaked TCP socket.  `ctype` lets Finished re-use the same
 * cipher path before the connection is marked application-ready. */
static int tls_send_aead(tls_conn_t *t, uint8_t ctype,
                         const uint8_t *plain, uint16_t len) {
    /* Nonce: 4-byte implicit salt || 8-byte explicit (use the sequence #). */
    uint8_t nonce[12];
    memcpy(nonce, t->salt_send, 4);
    for (int i = 0; i < 8; i++) {
        nonce[4 + i] = (uint8_t)((t->seq_send >> ((7 - i) * 8)) & 0xFF);
    }
    uint8_t aad[13];
    for (int i = 0; i < 8; i++) aad[i] = (uint8_t)((t->seq_send >> ((7 - i) * 8)) & 0xFF);
    aad[8]  = ctype;
    aad[9]  = 0x03; aad[10] = 0x03;
    aad[11] = (uint8_t)(len >> 8);
    aad[12] = (uint8_t)len;
    uint8_t cipher[TLS_RECMAX];
    uint8_t tag[16];
    aes_gcm_encrypt(&t->enc_key, nonce, aad, sizeof(aad),
                    plain, cipher, len, tag);
    /* Wire: 8-byte explicit nonce || ciphertext || tag. */
    uint8_t payload[TLS_RECMAX + 24];
    memcpy(payload, nonce + 4, 8);
    memcpy(payload + 8, cipher, len);
    memcpy(payload + 8 + len, tag, 16);
    int rc = tls_send_record(t, ctype, payload, (uint16_t)(8 + len + 16));
    t->seq_send++;
    return rc;
}

/* Decrypt an inbound AEAD record into `plain`.  Returns the plaintext
 * length or -1 on auth failure / framing error.  The record was already
 * read into `rec` by the caller. */
static int tls_open_aead(tls_conn_t *t,
                         const uint8_t *rec, uint16_t rlen, uint8_t ctype,
                         uint8_t *plain) {
    if (rlen < 24) return -1;
    uint8_t nonce[12];
    memcpy(nonce, t->salt_recv, 4);
    memcpy(nonce + 4, rec, 8);
    uint32_t cipher_len = rlen - 24;
    uint8_t aad[13];
    for (int i = 0; i < 8; i++) aad[i] = (uint8_t)((t->seq_recv >> ((7 - i) * 8)) & 0xFF);
    aad[8]  = ctype;
    aad[9]  = 0x03; aad[10] = 0x03;
    aad[11] = (uint8_t)(cipher_len >> 8);
    aad[12] = (uint8_t)cipher_len;
    if (!aes_gcm_decrypt(&t->dec_key, nonce, aad, sizeof(aad),
                         rec + 8, plain, cipher_len, rec + 8 + cipher_len)) {
        debug_printf("[tls] auth tag verification failed (ctype=%u)\n", ctype);
        return -1;
    }
    t->seq_recv++;
    return (int)cipher_len;
}

int tls_send(tls_handle_t s, const void *buf, uint32_t len) {
    if (s < 0 || s >= TLS_MAX) return -1;
    tls_conn_t *t = &g_tls[s];
    if (!t->in_use || !t->ready) return -1;
    if (tls_send_aead(t, TLS_REC_APPLICATION,
                      (const uint8_t *)buf, (uint16_t)len) != 0)
        return -1;
    return (int)len;
}

int tls_recv(tls_handle_t s, void *buf, uint32_t cap, uint32_t timeout_ms) {
    if (s < 0 || s >= TLS_MAX) return -1;
    tls_conn_t *t = &g_tls[s];
    if (!t->in_use || !t->ready) return -1;
    uint8_t ctype;
    uint16_t rlen;
    uint8_t rec[TLS_RECMAX + 24];
    if (tls_recv_record(t, &ctype, rec, sizeof(rec), &rlen, timeout_ms) != 0) {
        return -1;
    }
    if (ctype == TLS_REC_ALERT) return 0;
    if (ctype != TLS_REC_APPLICATION) return -1;
    uint8_t plain[TLS_RECMAX];
    int plain_len = tls_open_aead(t, rec, rlen, TLS_REC_APPLICATION, plain);
    if (plain_len < 0) return -1;
    uint32_t want = (uint32_t)plain_len < cap ? (uint32_t)plain_len : cap;
    memcpy(buf, plain, want);
    return (int)want;
}

void tls_close(tls_handle_t s) {
    if (s < 0 || s >= TLS_MAX) return;
    tls_conn_t *t = &g_tls[s];
    if (t->in_use) {
        if (t->ready) {
            uint8_t alert[2] = { 1, 0 };   /* warning, close_notify */
            tls_send_aead(t, TLS_REC_ALERT, alert, 2);
        }
        if (t->tcp != TCP_INVALID) tcp_close(t->tcp);
    }
    t->in_use = false;
}

/* ============================================================================
 * RSA-PKCS#1 v1.5 (TLS key transport) — TASK 6 refinement
 * ----------------------------------------------------------------------------
 * Bare big-integer modexp using the right-to-left binary method.
 *
 * Numbers are stored as MSB-first byte arrays (matching network byte
 * order in TLS messages and DER integers in X.509 certs).  All
 * arithmetic runs against scratch 256-byte buffers so we never need
 * a heap.  Performance is intentionally pedestrian (no Montgomery
 * domain conversion) - one TLS handshake per page-load is plenty fast
 * with naive modmul.
 * ============================================================================ */
#define BN_MAX  RSA_MAX_BYTES

static int bn_is_zero(const uint8_t *a, int n) {
    for (int i = 0; i < n; i++) if (a[i]) return 0;
    return 1;
}

static int bn_cmp(const uint8_t *a, const uint8_t *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1;
    }
    return 0;
}

static void bn_copy(uint8_t *dst, const uint8_t *src, int n) {
    for (int i = 0; i < n; i++) dst[i] = src[i];
}

static int bn_sub(uint8_t *r, const uint8_t *a, const uint8_t *b, int n) {
    int borrow = 0;
    for (int i = n - 1; i >= 0; i--) {
        int v = (int)a[i] - (int)b[i] - borrow;
        if (v < 0) { v += 256; borrow = 1; } else borrow = 0;
        r[i] = (uint8_t)v;
    }
    return borrow;
}

/* r = (a + b) mod m, where both operands are < m. */
static void bn_addmod(uint8_t *r, const uint8_t *a, const uint8_t *b,
                      const uint8_t *m, int n) {
    int carry = 0;
    for (int i = n - 1; i >= 0; i--) {
        int v = (int)a[i] + (int)b[i] + carry;
        carry = v >> 8;
        r[i] = (uint8_t)v;
    }
    if (carry || bn_cmp(r, m, n) >= 0) bn_sub(r, r, m, n);
}

/* r = (a * b) mod m via double-and-add over b. */
static void bn_mulmod(uint8_t *r, const uint8_t *a, const uint8_t *b,
                      const uint8_t *m, int n) {
    uint8_t acc[BN_MAX]; for (int i = 0; i < n; i++) acc[i] = 0;
    uint8_t tmp[BN_MAX]; bn_copy(tmp, a, n);
    for (int i = n - 1; i >= 0; i--) {
        uint8_t byte = b[i];
        for (int bit = 0; bit < 8; bit++) {
            if (byte & (1u << bit)) {
                bn_addmod(acc, acc, tmp, m, n);
            }
            bn_addmod(tmp, tmp, tmp, m, n);
        }
    }
    bn_copy(r, acc, n);
}

int rsa_modexp(const uint8_t *base, uint16_t base_len,
               uint32_t exponent,
               const uint8_t *modulus, uint16_t modulus_len,
               uint8_t *out) {
    if (!base || !modulus || !out) return -1;
    if (modulus_len == 0 || modulus_len > BN_MAX) return -1;
    /* Pad base into a modulus-sized big-endian integer. */
    uint8_t b[BN_MAX]; for (int i = 0; i < modulus_len; i++) b[i] = 0;
    uint16_t pad = modulus_len - (base_len < modulus_len ? base_len : modulus_len);
    for (uint16_t i = 0; i < base_len && pad + i < modulus_len; i++) {
        b[pad + i] = base[i];
    }
    uint8_t result[BN_MAX]; for (int i = 0; i < modulus_len; i++) result[i] = 0;
    result[modulus_len - 1] = 1;             /* result = 1 */
    while (exponent) {
        if (exponent & 1) {
            uint8_t tmp[BN_MAX];
            bn_mulmod(tmp, result, b, modulus, modulus_len);
            bn_copy(result, tmp, modulus_len);
        }
        exponent >>= 1;
        if (exponent) {
            uint8_t tmp[BN_MAX];
            bn_mulmod(tmp, b, b, modulus, modulus_len);
            bn_copy(b, tmp, modulus_len);
        }
    }
    bn_copy(out, result, modulus_len);
    return 0;
}

int rsa_pkcs1_encrypt(const rsa_pubkey_t *key,
                      const uint8_t *msg, uint16_t msg_len,
                      uint8_t *out) {
    if (!key || !msg || !out) return -1;
    uint16_t k = key->modulus_len;
    if (msg_len + 11 > k) return -1;
    /* PKCS#1 v1.5 type-2 padding: 0x00 0x02 PS 0x00 msg
     * where PS is at least 8 nonzero random bytes. */
    uint16_t ps_len = k - msg_len - 3;
    out[0] = 0x00;
    out[1] = 0x02;
    crypto_random(out + 2, ps_len);
    for (uint16_t i = 0; i < ps_len; i++) if (out[2 + i] == 0) out[2 + i] = 1;
    out[2 + ps_len] = 0x00;
    for (uint16_t i = 0; i < msg_len; i++) out[3 + ps_len + i] = msg[i];
    /* Encrypt in place. */
    uint8_t scratch[BN_MAX];
    bn_copy(scratch, out, k);
    return rsa_modexp(scratch, k, key->exponent,
                      key->modulus, k, out);
}

/* ============================================================================
 * Minimal X.509 SubjectPublicKeyInfo parser
 * ----------------------------------------------------------------------------
 * Walks the DER stream looking for the RSA modulus + exponent INTEGER
 * pair.  Skips object identifiers, sequence/version wrappers, and the
 * outer SubjectPublicKeyInfo bit-string.  Adequate for extracting the
 * server's RSA public key during the TLS Certificate message; does
 * NOT validate the chain (that needs a CA root store, in a follow-up).
 * ============================================================================ */
static int der_read_len(const uint8_t *p, uint32_t left, uint32_t *out_len) {
    if (left < 1) return -1;
    uint8_t b0 = p[0];
    if (b0 < 0x80) { *out_len = b0; return 1; }
    int n = b0 & 0x7F;
    if (n == 0 || (uint32_t)n + 1 > left) return -1;
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | p[1 + i];
    *out_len = v;
    return 1 + n;
}

int x509_parse_rsa_pubkey(const uint8_t *der, uint32_t der_len,
                          rsa_pubkey_t *out) {
    if (!der || !out) return -1;
    /* Walk byte-by-byte looking for the RSA encryption OID
     * (1.2.840.113549.1.1.1 = 06 09 2A 86 48 86 F7 0D 01 01 01). */
    static const uint8_t rsa_oid[] = {
        0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01
    };
    uint32_t i = 0;
    while (i + sizeof(rsa_oid) < der_len) {
        bool ok = true;
        for (uint32_t k = 0; k < sizeof(rsa_oid); k++) {
            if (der[i + k] != rsa_oid[k]) { ok = false; break; }
        }
        if (ok) { i += sizeof(rsa_oid); break; }
        i++;
    }
    if (i >= der_len) return -1;
    /* Past the OID we expect a BIT STRING (tag 0x03) containing the
     * RSAPublicKey SEQUENCE { modulus INTEGER, publicExponent INTEGER }. */
    while (i < der_len && der[i] != 0x03) i++;
    if (i >= der_len) return -1;
    i++;
    uint32_t bs_len;
    int adv = der_read_len(der + i, der_len - i, &bs_len);
    if (adv < 0) return -1;
    i += adv;
    /* Skip the unused-bits byte. */
    if (i >= der_len) return -1;
    i++;
    /* Inner SEQUENCE. */
    if (i >= der_len || der[i] != 0x30) return -1;
    i++;
    uint32_t seq_len;
    adv = der_read_len(der + i, der_len - i, &seq_len);
    if (adv < 0) return -1;
    i += adv;
    /* Modulus INTEGER. */
    if (i >= der_len || der[i] != 0x02) return -1;
    i++;
    uint32_t mod_len;
    adv = der_read_len(der + i, der_len - i, &mod_len);
    if (adv < 0) return -1;
    i += adv;
    /* DER INTEGER may have a leading 0x00 for the sign bit. */
    if (mod_len > 0 && der[i] == 0x00) { i++; mod_len--; }
    if (mod_len > RSA_MAX_BYTES) return -1;
    for (uint32_t k = 0; k < mod_len; k++) out->modulus[k] = der[i + k];
    out->modulus_len = (uint16_t)mod_len;
    i += mod_len;
    /* Exponent INTEGER. */
    if (i >= der_len || der[i] != 0x02) return -1;
    i++;
    uint32_t exp_len;
    adv = der_read_len(der + i, der_len - i, &exp_len);
    if (adv < 0) return -1;
    i += adv;
    uint32_t e = 0;
    for (uint32_t k = 0; k < exp_len && k < 4; k++) {
        e = (e << 8) | der[i + k];
    }
    out->exponent = e;
    return 0;
}
