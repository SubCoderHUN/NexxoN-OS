/* ============================================================================
 * NexxoN OS - Encrypted credential vault
 * ----------------------------------------------------------------------------
 * Per-user AES-128-GCM credential store.  Key derivation:
 *
 *     prk     = HMAC-SHA256(login_password, "nexxon-vault-salt")
 *     vault_k = HKDF-Expand(prk, "vault-key", 16)
 *
 * Each entry's plaintext (name||user||password packed) is encrypted
 * with a unique 12-byte nonce drawn from the PRNG and tagged with the
 * GMAC produced by aes_gcm_encrypt.  Records are appended to an in-RAM
 * array that the future installer will persist to /sys/vault.db.
 * ============================================================================ */
#include "vault.h"
#include "crypto.h"
#include "string.h"
#include "debug.h"

typedef struct {
    bool      in_use;
    char      name[VAULT_NAME_MAX];
    uint8_t   nonce[12];
    uint16_t  cipher_len;
    uint8_t   cipher[VAULT_USER_MAX + VAULT_PASS_MAX + 4];
    uint8_t   tag[16];
} entry_t;

static entry_t   g_entries[VAULT_MAX_ENTRIES];
static aes_ctx_t g_aes;
static bool      g_unlocked = false;

void vault_init(void) {
    memset(g_entries, 0, sizeof(g_entries));
    g_unlocked = false;
}

int vault_unlock(const char *login_password) {
    if (!login_password || !login_password[0]) return -1;
    uint8_t prk[SHA256_HASH];
    hkdf_extract("nexxon-vault-salt", 17,
                 login_password, (uint32_t)strlen(login_password), prk);
    uint8_t key[16];
    hkdf_expand(prk, "vault-key", 9, key, 16);
    aes_set_key(&g_aes, key);
    g_unlocked = true;
    debug_printf("[vault] unlocked (key derived via HKDF)\n");
    return 0;
}

void vault_lock(void) {
    memset(&g_aes, 0, sizeof(g_aes));
    g_unlocked = false;
}

bool vault_unlocked(void) { return g_unlocked; }

static void pack_record(const vault_record_t *r, uint8_t *out, uint16_t *out_len) {
    uint16_t off = 0;
    uint16_t n = (uint16_t)strlen(r->username);
    out[off++] = (uint8_t)(n & 0xFF);
    out[off++] = (uint8_t)(n >> 8);
    memcpy(out + off, r->username, n); off += n;
    n = (uint16_t)strlen(r->password);
    out[off++] = (uint8_t)(n & 0xFF);
    out[off++] = (uint8_t)(n >> 8);
    memcpy(out + off, r->password, n); off += n;
    *out_len = off;
}

static void unpack_record(const uint8_t *in, uint16_t len,
                          vault_record_t *r) {
    uint16_t off = 0;
    if (off + 2 > len) return;
    uint16_t un = in[off] | (in[off + 1] << 8); off += 2;
    if (un >= VAULT_USER_MAX) un = VAULT_USER_MAX - 1;
    if (off + un > len) return;
    memcpy(r->username, in + off, un); r->username[un] = 0; off += un;
    if (off + 2 > len) return;
    uint16_t pn = in[off] | (in[off + 1] << 8); off += 2;
    if (pn >= VAULT_PASS_MAX) pn = VAULT_PASS_MAX - 1;
    if (off + pn > len) return;
    memcpy(r->password, in + off, pn); r->password[pn] = 0;
}

int vault_store(const vault_record_t *r) {
    if (!g_unlocked || !r) return -1;
    /* Find an existing entry by name, else allocate. */
    entry_t *e = NULL;
    for (int i = 0; i < VAULT_MAX_ENTRIES; i++) {
        if (g_entries[i].in_use &&
            strcmp(g_entries[i].name, r->name) == 0) { e = &g_entries[i]; break; }
    }
    if (!e) {
        for (int i = 0; i < VAULT_MAX_ENTRIES; i++) {
            if (!g_entries[i].in_use) { e = &g_entries[i]; break; }
        }
    }
    if (!e) return -1;
    memset(e, 0, sizeof(*e));
    e->in_use = true;
    int n = 0;
    while (r->name[n] && n < VAULT_NAME_MAX - 1) { e->name[n] = r->name[n]; n++; }
    e->name[n] = 0;
    /* Fresh nonce. */
    crypto_random(e->nonce, 12);
    uint8_t plain[VAULT_USER_MAX + VAULT_PASS_MAX + 4];
    uint16_t plen = 0;
    pack_record(r, plain, &plen);
    aes_gcm_encrypt(&g_aes, e->nonce,
                    (const uint8_t *)e->name, (uint32_t)strlen(e->name),
                    plain, e->cipher, plen, e->tag);
    e->cipher_len = plen;
    return 0;
}

int vault_lookup(const char *name, vault_record_t *out) {
    if (!g_unlocked || !name || !out) return -1;
    for (int i = 0; i < VAULT_MAX_ENTRIES; i++) {
        entry_t *e = &g_entries[i];
        if (!e->in_use || strcmp(e->name, name) != 0) continue;
        uint8_t plain[VAULT_USER_MAX + VAULT_PASS_MAX + 4];
        if (!aes_gcm_decrypt(&g_aes, e->nonce,
                             (const uint8_t *)e->name, (uint32_t)strlen(e->name),
                             e->cipher, plain, e->cipher_len, e->tag)) {
            return -1;
        }
        memset(out, 0, sizeof(*out));
        int n = 0;
        while (e->name[n] && n < VAULT_NAME_MAX - 1) { out->name[n] = e->name[n]; n++; }
        out->name[n] = 0;
        unpack_record(plain, e->cipher_len, out);
        return 0;
    }
    return -1;
}

int vault_count(void) {
    int n = 0;
    for (int i = 0; i < VAULT_MAX_ENTRIES; i++) if (g_entries[i].in_use) n++;
    return n;
}

const char *vault_name(int idx) {
    int seen = 0;
    for (int i = 0; i < VAULT_MAX_ENTRIES; i++) {
        if (!g_entries[i].in_use) continue;
        if (seen == idx) return g_entries[i].name;
        seen++;
    }
    return NULL;
}
