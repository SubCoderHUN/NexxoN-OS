/* ============================================================================
 * NexxoN OS - Authentication / User Manager  (v1.0)
 * ----------------------------------------------------------------------------
 * Stores a credential database at /sys/users.cfg (one user per line:
 * "name|salt|sha256hex|role"), reads it at boot, exposes a verify()
 * helper used by the graphical login screen + shell `login` command,
 * and offers an admin API so the User Manager app can add / remove /
 * change passwords.  Passwords are salted + SHA-256 hashed via the
 * existing crypto.c module - we never store cleartext on disk.
 * ============================================================================ */
#ifndef NEXXON_AUTH_H
#define NEXXON_AUTH_H

#include "types.h"

#define AUTH_NAME_MAX 24
#define AUTH_SALT_LEN 8
#define AUTH_MAX_USERS 8

typedef enum {
    ROLE_USER  = 0,
    ROLE_ADMIN = 1,
} auth_role_t;

typedef struct {
    bool        in_use;
    char        name[AUTH_NAME_MAX];
    uint8_t     salt[AUTH_SALT_LEN];
    uint8_t     hash[32];
    auth_role_t role;
} auth_user_t;

void          auth_init(void);
int           auth_user_count(void);
bool          auth_get_user(int idx, auth_user_t *out);
bool          auth_add_user(const char *name, const char *password,
                            auth_role_t role);
bool          auth_remove_user(const char *name);
bool          auth_change_password(const char *name, const char *new_pwd);
bool          auth_verify(const char *name, const char *password,
                          auth_role_t *out_role);
const char   *auth_current_user(void);
auth_role_t   auth_current_role(void);
void          auth_set_current_user(const char *name);

#endif /* NEXXON_AUTH_H */
