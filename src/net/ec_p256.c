/* NexxoN OS - secp256r1 ECDH (TLS 1.2 ECDHE-RSA) */
#include "ec_p256.h"
#include "string.h"

typedef uint32_t fe[8];

static const fe P256_P = {
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000001u, 0xFFFFFFFFu
};
static const fe P256_N = {
    0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xBCE6FAADu, 0xA7179E84u, 0xF3B9CAC2u, 0xFC632551u
};
static const fe P256_DELTA = {
    0x00000000u, 0xFFFFFFFFu, 0x00000001u, 0x00000001u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u
};
static const fe P256_GX = {
    0x6B17D1F2u, 0xE12B4248u, 0xF8CE6E56u, 0x26331A7Du,
    0x86D39D08u, 0x9615FF96u, 0x84447E40u, 0x9AB1C5FCu
};
static const fe P256_GY = {
    0x4FE342E2u, 0xE1F7FED8u, 0xE0B5F755u, 0x9C98F9DBu,
    0x4C4AB0E8u, 0x5A0C0E2Fu, 0x340A2F8Fu, 0xB05D5B58u
};

static void fe_copy(fe r, const fe a) { memcpy(r, a, sizeof(fe)); }

static int fe_is_zero(const fe a) {
    uint32_t z = 0;
    for (int i = 0; i < 8; i++) z |= a[i];
    return z == 0;
}

static int fe_cmp(const fe a, const fe b) {
    for (int i = 0; i < 8; i++)
        if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1;
    return 0;
}

static void fe_from_bytes(fe r, const uint8_t *in) {
    for (int i = 0; i < 8; i++)
        r[i] = ((uint32_t)in[i*4] << 24) | ((uint32_t)in[i*4+1] << 16) |
               ((uint32_t)in[i*4+2] << 8) | (uint32_t)in[i*4+3];
}

static void fe_to_bytes(uint8_t *out, const fe a) {
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(a[i] >> 24);
        out[i*4+1] = (uint8_t)(a[i] >> 16);
        out[i*4+2] = (uint8_t)(a[i] >> 8);
        out[i*4+3] = (uint8_t)a[i];
    }
}

static void fe_add_u32_lsw(uint32_t w[16], int idx, uint64_t v) {
    if (idx < 0 || idx >= 16) return;
    uint64_t sum = (uint64_t)w[idx] + v;
    w[idx] = (uint32_t)sum;
    if (sum >> 32)
        fe_add_u32_lsw(w, idx + 1, sum >> 32);
}

static void fe_from_words(fe r, const uint32_t w[16], int start) {
    for (int i = 0; i < 8; i++)
        r[7 - i] = w[start + i];
}

static void fe_mul_raw(uint32_t w[16], const fe a, const fe b) {
    memset(w, 0, sizeof(uint32_t) * 16);
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++)
            fe_add_u32_lsw(w, 14 - i - j, (uint64_t)a[i] * b[j]);
    }
}

static void fe_mod_p(fe r) {
    while (fe_cmp(r, P256_P) >= 0) {
        fe t;
        int borrow = 0;
        for (int i = 7; i >= 0; i--) {
            uint64_t v = (uint64_t)r[i] - P256_P[i] - borrow;
            borrow = v >> 63;
            t[i] = (uint32_t)v;
        }
        fe_copy(r, t);
    }
}

static void fe_add_p(fe r, const fe a, const fe b) {
    uint64_t carry = 0;
    for (int i = 7; i >= 0; i--) {
        uint64_t v = (uint64_t)a[i] + b[i] + carry;
        carry = v >> 32;
        r[i] = (uint32_t)v;
    }
    fe_mod_p(r);
}

static void fe_sub_p(fe r, const fe a, const fe b) {
    int borrow = 0;
    for (int i = 7; i >= 0; i--) {
        uint64_t v = (uint64_t)a[i] - b[i] - borrow;
        borrow = v >> 63;
        r[i] = (uint32_t)v;
    }
    if (borrow) {
        uint64_t carry = 0;
        for (int i = 7; i >= 0; i--) {
            uint64_t v = (uint64_t)r[i] + P256_P[i] + carry;
            carry = v >> 32;
            r[i] = (uint32_t)v;
        }
    }
}

/* Fold the high 256 bits of a 512-bit product using P256_DELTA. */
static void fe_mul_delta(fe r, const fe high) {
    uint32_t w[16];
    fe_mul_raw(w, high, P256_DELTA);
    fe low, carry, extra;
    fe_from_words(low, w, 0);
    fe_from_words(carry, w, 8);
    fe_mul_raw(w, carry, P256_DELTA);
    fe_from_words(extra, w, 0);
    fe_add_p(r, low, extra);
}

static void fe_reduce_p(fe r, const uint32_t w[16]) {
    fe low, high, prod, sum;
    fe_from_words(low, w, 0);
    fe_from_words(high, w, 8);
    fe_mul_delta(prod, high);
    fe_add_p(sum, low, prod);
    fe_copy(r, sum);
}

static void fe_mul_p(fe r, const fe a, const fe b) {
    uint32_t w[16];
    fe_mul_raw(w, a, b);
    fe_reduce_p(r, w);
}

static void fe_sqr_p(fe r, const fe a) { fe_mul_p(r, a, a); }

static void fe_inv_p(fe r, const fe a) {
    fe e, base;
    fe_copy(base, a);
    fe_copy(e, P256_P);
    e[7] -= 2;
    fe_copy(r, base);
    for (int i = 0; i < 8; i++)
        for (int bit = 31; bit >= 0; bit--) {
            fe_sqr_p(r, r);
            if ((e[i] >> bit) & 1) fe_mul_p(r, r, base);
        }
}

typedef struct { fe x, y, z; } jac;

static void jac_affine(fe x, fe y, const jac *p) {
    if (fe_is_zero(p->z)) { memset(x, 0, sizeof(fe)); memset(y, 0, sizeof(fe)); return; }
    fe zi, z2, z3;
    fe_inv_p(zi, p->z);
    fe_sqr_p(z2, zi);
    fe_mul_p(z3, z2, zi);
    fe_mul_p(x, p->x, z2);
    fe_mul_p(y, p->y, z3);
}

static void jac_double(jac *r, const jac *p) {
    if (fe_is_zero(p->z)) { memset(r, 0, sizeof(*r)); return; }
    fe t1, t2, t3, t4, t5;
    fe_sqr_p(t1, p->y);
    fe_mul_p(t2, p->x, t1);
    fe_sqr_p(t3, t1);
    fe_sqr_p(t4, p->x);
    fe_add_p(t5, t4, t4);
    fe_add_p(t4, t5, t4);
    fe_sqr_p(t5, t4);
    fe_sub_p(t1, t5, t2);
    fe_sub_p(t1, t1, t2);
    fe_add_p(t2, t2, t2);
    fe_add_p(t2, t2, t2);
    fe_mul_p(t4, t4, t4);
    fe_sub_p(t2, t2, t4);
    fe_sub_p(t2, t2, t4);
    fe_mul_p(t3, t3, p->x);
    fe_add_p(t3, t3, t3);
    fe_add_p(t3, t3, t3);
    fe_sqr_p(r->x, t1);
    fe_sub_p(r->x, r->x, t2);
    fe_sub_p(r->x, r->x, t2);
    fe_sub_p(t2, t2, r->x);
    fe_mul_p(t1, t1, t2);
    fe_sub_p(r->y, t1, t3);
    fe_mul_p(r->z, p->y, p->z);
    fe_add_p(r->z, r->z, r->z);
}

static void jac_add_mixed(jac *r, const jac *p, const fe qx, const fe qy) {
    if (fe_is_zero(p->z)) {
        fe_copy(r->x, qx); fe_copy(r->y, qy);
        memset(r->z, 0, sizeof(r->z)); r->z[7] = 1;
        return;
    }
    fe t1, t2, t3, t4, t5, t6;
    fe_sqr_p(t1, p->z);
    fe_mul_p(t2, t1, p->z);
    fe_mul_p(t1, t1, qx);
    fe_mul_p(t2, t2, qy);
    fe_sub_p(t1, t1, p->x);
    fe_sub_p(t2, t2, p->y);
    if (fe_is_zero(t1)) {
        if (fe_is_zero(t2)) jac_double(r, p);
        else memset(r, 0, sizeof(*r));
        return;
    }
    fe_mul_p(r->z, p->z, t1);
    fe_sqr_p(t3, t1);
    fe_mul_p(t4, t3, t1);
    fe_mul_p(t3, t3, p->x);
    fe_add_p(t1, t1, t1);
    fe_add_p(t6, t3, t3);
    fe_sqr_p(t5, t2);
    fe_sub_p(t5, t5, t1);
    fe_sub_p(t5, t5, t4);
    fe_sub_p(t3, t3, t5);
    fe_mul_p(t3, t3, t2);
    fe_mul_p(t4, t4, p->y);
    fe_sub_p(r->y, t3, t4);
    fe_sub_p(t3, t5, t6);
    fe_mul_p(r->x, t2, t3);
    fe_mul_p(t4, t4, t1);
    fe_sub_p(r->x, r->x, t4);
}

static void scalar_mult(jac *r, const uint8_t k[32], const fe gx, const fe gy) {
    jac q;
    fe_copy(q.x, gx); fe_copy(q.y, gy);
    memset(q.z, 0, sizeof(q.z)); q.z[7] = 1;
    int started = 0;
    for (int i = 0; i < 32; i++) {
        uint8_t byte = k[i];
        for (int bit = 7; bit >= 0; bit--) {
            if (started) jac_double(r, r);
            if (byte & (1u << bit)) {
                if (!started) { *r = q; started = 1; }
                else { jac t; jac_add_mixed(&t, r, q.x, q.y); *r = t; }
            }
        }
    }
}

static int scalar_ok(const uint8_t k[32]) {
    fe s; fe_from_bytes(s, k);
    return !fe_is_zero(s) && fe_cmp(s, P256_N) < 0;
}

int p256_pubkey_from_priv(const uint8_t priv32[P256_BYTES], uint8_t pub65[P256_POINT]) {
    if (!priv32 || !pub65 || !scalar_ok(priv32)) return -1;
    jac r; scalar_mult(&r, priv32, P256_GX, P256_GY);
    fe x, y; jac_affine(x, y, &r);
    pub65[0] = 0x04;
    fe_to_bytes(pub65 + 1, x);
    fe_to_bytes(pub65 + 33, y);
    return 0;
}

int p256_ecdh(const uint8_t priv32[P256_BYTES], const uint8_t peer65[P256_POINT],
              uint8_t shared32[P256_BYTES]) {
    if (!priv32 || !peer65 || !shared32) return -1;
    if (peer65[0] != 0x04 || !scalar_ok(priv32)) return -1;
    fe qx, qy;
    fe_from_bytes(qx, peer65 + 1);
    fe_from_bytes(qy, peer65 + 33);
    jac r; scalar_mult(&r, priv32, qx, qy);
    fe x, y; (void)y;
    jac_affine(x, y, &r);
    fe_to_bytes(shared32, x);
    return 0;
}
