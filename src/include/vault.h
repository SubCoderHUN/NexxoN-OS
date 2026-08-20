/* ============================================================================
 * NexxoN OS - Encrypted credential vault  (v1.0)
 * ----------------------------------------------------------------------------
 * Stores secrets (URL, username, password) encrypted with AES-128-GCM
 * using a key derived from the logged-in user's password via PBKDF2-
 * style HKDF expansion of HMAC-SHA256(login_password, "vault-key").
 *
 *   vault_unlock(password)    - derives the key, decrypts /sys/vault.db
 *   vault_store(name,u,p)     - encrypts + appends a credential record
 *   vault_lookup(name, out)   - returns matching credential
 *   vault_list(idx, name)     - iterate
 *
 * Records on disk:
 *     uint16_t name_len; char name[name_len];
 *     uint16_t blob_len; uint8_t nonce[12]; uint8_t cipher[blob_len];
 *     uint8_t  tag[16];
 *
 * Capacity: 64 entries.  Lock with vault_lock() to wipe the in-RAM key.
 * ============================================================================ */
#ifndef NEXXON_VAULT_H
#define NEXXON_VAULT_H

#include "types.h"

#define VAULT_NAME_MAX    48
#define VAULT_USER_MAX    64
#define VAULT_PASS_MAX    96
#define VAULT_MAX_ENTRIES 64

typedef struct {
    char name[VAULT_NAME_MAX];
    char username[VAULT_USER_MAX];
    char password[VAULT_PASS_MAX];
} vault_record_t;

void vault_init   (void);
int  vault_unlock (const char *login_password);
void vault_lock   (void);
bool vault_unlocked(void);

int  vault_store  (const vault_record_t *r);
int  vault_lookup (const char *name, vault_record_t *out);
int  vault_count  (void);
const char *vault_name(int idx);

#endif /* NEXXON_VAULT_H */
