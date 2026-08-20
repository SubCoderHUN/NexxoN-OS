/* Host-side sanity check for secp256r1 ECDH. */
#include <stdio.h>
#include "string.h"
#include "ec_p256.h"

static const uint8_t k_g_pub[65] = {
    0x04,
    0x6B, 0x17, 0xD1, 0xF2, 0xE1, 0x2B, 0x42, 0x48, 0xF8, 0xCE, 0x6E, 0x56,
    0x26, 0x33, 0x1A, 0x7D, 0x86, 0xD3, 0x9D, 0x08, 0x96, 0x15, 0xFF, 0x96,
    0x84, 0x44, 0x7E, 0x40, 0x9A, 0xB1, 0xC5, 0xFC,
    0x4F, 0xE3, 0x42, 0xE2, 0xE1, 0xF7, 0xFE, 0xD8, 0xE0, 0xB5, 0xF7, 0x55,
    0x9C, 0x98, 0xF9, 0xDB, 0x4C, 0x4A, 0xB0, 0xE8, 0x5A, 0x0C, 0x0E, 0x2F,
    0x34, 0x0A, 0x2F, 0x8F, 0xB0, 0x5D, 0x5B, 0x58
};

static void hexdump(const char *tag, const uint8_t *b, int n) {
    printf("%s:", tag);
    for (int i = 0; i < n; i++) printf(" %02x", b[i]);
    printf("\n");
}

int main(void) {
    uint8_t one[32] = { 0 };
    one[31] = 1;
    uint8_t pub[65];

    if (p256_pubkey_from_priv(one, pub) != 0) {
        fprintf(stderr, "pubkey(1) failed\n");
        return 1;
    }
    if (memcmp(pub, k_g_pub, 65) != 0) {
        fprintf(stderr, "G mismatch\n");
        hexdump("want", k_g_pub, 65);
        hexdump(" got", pub, 65);
        return 1;
    }

    uint8_t priv[32] = { 0 };
    priv[0] = 0x42;
    priv[31] = 0x01;
    uint8_t pub2[65], shared_a[32], shared_b[32];
    if (p256_pubkey_from_priv(priv, pub2) != 0) {
        fprintf(stderr, "pubkey2 failed\n");
        return 1;
    }
    if (p256_ecdh(priv, k_g_pub, shared_a) != 0 ||
        p256_ecdh(one, pub2, shared_b) != 0) {
        fprintf(stderr, "ecdh failed\n");
        return 1;
    }
    if (memcmp(shared_a, shared_b, 32) != 0) {
        fprintf(stderr, "symmetry mismatch\n");
        hexdump("a", shared_a, 32);
        hexdump("b", shared_b, 32);
        return 1;
    }

    uint8_t pub_x[32];
    memcpy(pub_x, pub2 + 1, 32);
    if (memcmp(shared_b, pub_x, 32) != 0) {
        fprintf(stderr, "shared x != pubkey x\n");
        hexdump("shared", shared_b, 32);
        hexdump("pub_x ", pub_x, 32);
        return 1;
    }

    printf("host ec_p256 OK\n");
    return 0;
}
