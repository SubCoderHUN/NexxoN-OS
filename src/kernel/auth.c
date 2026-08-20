/* ============================================================================
 * NexxoN OS - Authentication core
 * ----------------------------------------------------------------------------
 * Compact credential store using SHA-256 (from crypto.c) + per-user
 * salts.  Default install seeds an "admin" account with password
 * "admin" so a fresh boot can log in; the User Manager + Settings
 * panel both let the user change this.
 *
 * On-disk format (one user per line):
 *   name|saltHex|hashHex|role
 *
 * "Encryption" in this milestone means SHA-256 with per-user salt -
 * standard practice for password storage, not full FDE.
 * ============================================================================ */
#include "auth.h"
#include "crypto.h"
#include "string.h"
#include "debug.h"
#include "nxfs.h"
#include "pit.h"

#define AUTH_FILE "users.cfg"

static auth_user_t g_users[AUTH_MAX_USERS];
static int         g_user_count = 0;
static char        g_cur_user[AUTH_NAME_MAX] = "(none)";

static void hex_encode(const uint8_t *src, int len, char *dst) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < len; i++) {
        dst[i*2]   = hex[(src[i] >> 4) & 0xF];
        dst[i*2+1] = hex[src[i] & 0xF];
    }
    dst[len*2] = 0;
}
static int hex_decode(const char *src, int dst_len, uint8_t *dst) {
    int n = 0;
    for (int i = 0; n < dst_len && src[i] && src[i+1]; i += 2) {
        uint8_t b = 0;
        for (int k = 0; k < 2; k++) {
            char c = src[i + k];
            b <<= 4;
            if (c >= '0' && c <= '9') b |= (uint8_t)(c - '0');
            else if (c >= 'a' && c <= 'f') b |= (uint8_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') b |= (uint8_t)(c - 'A' + 10);
            else return n;
        }
        dst[n++] = b;
    }
    return n;
}

static void hash_password(const uint8_t salt[AUTH_SALT_LEN],
                          const char *pwd, uint8_t out[32]) {
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, salt, AUTH_SALT_LEN);
    sha256_update(&c, pwd, (uint32_t)strlen(pwd));
    sha256_final(&c, out);
}

static uint32_t ensure_sys_dir(void) {
    if (!nxfs_is_mounted()) return 0;
    uint32_t sys_ino;
    if (nxfs_resolve(0, "sys", &sys_ino) != NXFS_OK) {
        if (nxfs_create_dir(0, "sys", &sys_ino) != NXFS_OK) return 0;
    }
    return sys_ino;
}

static void save_users(void) {
    uint32_t sys = ensure_sys_dir();
    if (!sys) return;
    uint32_t cfg;
    if (nxfs_resolve(sys, AUTH_FILE, &cfg) != NXFS_OK) {
        if (nxfs_create_file(sys, AUTH_FILE, &cfg) != NXFS_OK) return;
    }
    char buf[2048];
    uint32_t pos = 0;
    for (int i = 0; i < AUTH_MAX_USERS; i++) {
        if (!g_users[i].in_use) continue;
        char salt_h[2*AUTH_SALT_LEN+1];
        char hash_h[65];
        hex_encode(g_users[i].salt, AUTH_SALT_LEN, salt_h);
        hex_encode(g_users[i].hash, 32,             hash_h);
        int n = ksnprintf(buf + pos, sizeof(buf) - pos,
                          "%s|%s|%s|%d\n",
                          g_users[i].name, salt_h, hash_h,
                          (int)g_users[i].role);
        if (n <= 0) break;
        pos += (uint32_t)n;
    }
    nxfs_write_file(cfg, buf, pos);
    debug_printf("[auth] saved %d byte(s) to /sys/%s\n", pos, AUTH_FILE);
}

static void load_users(void) {
    memset(g_users, 0, sizeof(g_users));
    g_user_count = 0;
    if (!nxfs_is_mounted()) return;
    uint32_t sys;
    if (nxfs_resolve(0, "sys", &sys) != NXFS_OK) return;
    uint32_t cfg;
    if (nxfs_resolve(sys, AUTH_FILE, &cfg) != NXFS_OK) return;
    char buf[2048];
    uint32_t got = 0;
    if (nxfs_read_file(cfg, buf, sizeof(buf) - 1, &got) != NXFS_OK) return;
    buf[got] = 0;

    char *p = buf;
    while (*p && g_user_count < AUTH_MAX_USERS) {
        char *line_end = p;
        while (*line_end && *line_end != '\n') line_end++;
        char saved_eol = *line_end;
        *line_end = 0;

        char *bar1 = strchr(p, '|');
        if (!bar1) goto next;
        char *bar2 = strchr(bar1 + 1, '|');
        if (!bar2) goto next;
        char *bar3 = strchr(bar2 + 1, '|');
        if (!bar3) goto next;
        *bar1 = *bar2 = *bar3 = 0;

        auth_user_t *u = &g_users[g_user_count];
        u->in_use = true;
        strncpy(u->name, p, AUTH_NAME_MAX - 1);
        u->name[AUTH_NAME_MAX - 1] = 0;
        hex_decode(bar1 + 1, AUTH_SALT_LEN, u->salt);
        hex_decode(bar2 + 1, 32, u->hash);
        u->role = (auth_role_t)(*(bar3 + 1) - '0');
        g_user_count++;

next:
        *line_end = saved_eol;
        if (saved_eol == '\n') line_end++;
        p = line_end;
    }
    debug_printf("[auth] loaded %d user(s) from /sys/%s\n",
                 g_user_count, AUTH_FILE);
}

void auth_init(void) {
    crypto_seed(pit_ms());
    load_users();
    if (g_user_count == 0) {
        debug_printf("[auth] seeding default admin/admin credential\n");
        auth_add_user("admin", "admin", ROLE_ADMIN);
    }
    strncpy(g_cur_user, "(none)", sizeof(g_cur_user) - 1);
}

int auth_user_count(void) { return g_user_count; }

bool auth_get_user(int idx, auth_user_t *out) {
    if (idx < 0 || idx >= AUTH_MAX_USERS) return false;
    if (!g_users[idx].in_use) return false;
    if (out) *out = g_users[idx];
    return true;
}

bool auth_add_user(const char *name, const char *password, auth_role_t role) {
    if (!name || !password || !name[0] || !password[0]) return false;
    if (strlen(name) >= AUTH_NAME_MAX) return false;
    /* No duplicates. */
    for (int i = 0; i < AUTH_MAX_USERS; i++) {
        if (g_users[i].in_use && strcmp(g_users[i].name, name) == 0) return false;
    }
    int slot = -1;
    for (int i = 0; i < AUTH_MAX_USERS; i++) {
        if (!g_users[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return false;
    auth_user_t *u = &g_users[slot];
    u->in_use = true;
    strncpy(u->name, name, AUTH_NAME_MAX - 1);
    u->name[AUTH_NAME_MAX - 1] = 0;
    crypto_random(u->salt, AUTH_SALT_LEN);
    hash_password(u->salt, password, u->hash);
    u->role = role;
    g_user_count++;
    save_users();
    return true;
}

bool auth_remove_user(const char *name) {
    for (int i = 0; i < AUTH_MAX_USERS; i++) {
        if (g_users[i].in_use && strcmp(g_users[i].name, name) == 0) {
            g_users[i].in_use = false;
            g_users[i].name[0] = 0;
            g_user_count--;
            save_users();
            return true;
        }
    }
    return false;
}

bool auth_change_password(const char *name, const char *new_pwd) {
    if (!name || !new_pwd) return false;
    for (int i = 0; i < AUTH_MAX_USERS; i++) {
        if (g_users[i].in_use && strcmp(g_users[i].name, name) == 0) {
            crypto_random(g_users[i].salt, AUTH_SALT_LEN);
            hash_password(g_users[i].salt, new_pwd, g_users[i].hash);
            save_users();
            return true;
        }
    }
    return false;
}

bool auth_verify(const char *name, const char *password,
                 auth_role_t *out_role) {
    if (!name || !password) return false;
    for (int i = 0; i < AUTH_MAX_USERS; i++) {
        if (!g_users[i].in_use) continue;
        if (strcmp(g_users[i].name, name) != 0) continue;
        uint8_t h[32];
        hash_password(g_users[i].salt, password, h);
        if (memcmp(h, g_users[i].hash, 32) == 0) {
            if (out_role) *out_role = g_users[i].role;
            return true;
        }
        return false;
    }
    return false;
}

const char *auth_current_user(void) { return g_cur_user; }

/* Look up the active user's role.  Returns ROLE_USER for "no current
 * user" or "user not found" so callers can default-deny privileged UI. */
auth_role_t auth_current_role(void) {
    if (!g_cur_user[0]) return ROLE_USER;
    for (int i = 0; i < AUTH_MAX_USERS; i++) {
        if (!g_users[i].in_use) continue;
        if (strcmp(g_users[i].name, g_cur_user) == 0) {
            return g_users[i].role;
        }
    }
    return ROLE_USER;
}

void auth_set_current_user(const char *name) {
    if (!name) name = "(none)";
    strncpy(g_cur_user, name, sizeof(g_cur_user) - 1);
    g_cur_user[sizeof(g_cur_user) - 1] = 0;
}
