/* ============================================================================
 * NexxoN OS - Cookie / session manager
 * ----------------------------------------------------------------------------
 * Tiny in-RAM cookie store + Set-Cookie parser + Cookie-header
 * assembler.  No heap; 32 slots in BSS.  Hooked into download.c so any
 * HTTP/HTTPS GET both forwards Set-Cookie back into the store and
 * injects matching cookies on subsequent requests.
 *
 * Expiry compares RTC seconds.  Session cookies (no Expires/Max-Age)
 * persist until the next reboot - they're never written to
 * /sys/cookies.cfg.
 * ============================================================================ */
#include "cookies.h"
#include "string.h"
#include "debug.h"
#include "nxfs.h"
#include "rtc.h"

static cookie_t g_cookies[COOKIE_MAX];

static void istrlower(char *s) {
    for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32;
}

static int istr_eq(const char *a, const char *b) {
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static void copy_n(char *dst, const char *src, int dst_cap, int n) {
    int i = 0;
    for (; i < n && i < dst_cap - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

void cookie_init(void) {
    memset(g_cookies, 0, sizeof(g_cookies));
    debug_printf("[cookies] store ready (%d slots)\n", COOKIE_MAX);
}

static cookie_t *find_slot(const char *host, const char *name) {
    for (int i = 0; i < COOKIE_MAX; i++) {
        if (!g_cookies[i].in_use) continue;
        if (istr_eq(g_cookies[i].host, host) &&
            strcmp(g_cookies[i].name, name) == 0)
            return &g_cookies[i];
    }
    return NULL;
}

static cookie_t *alloc_slot(void) {
    for (int i = 0; i < COOKIE_MAX; i++) {
        if (!g_cookies[i].in_use) return &g_cookies[i];
    }
    /* Evict the oldest (lowest expires) so the store self-rotates. */
    int victim = 0;
    uint32_t oldest = 0xFFFFFFFFu;
    for (int i = 0; i < COOKIE_MAX; i++) {
        if (g_cookies[i].expires_unix < oldest) {
            oldest = g_cookies[i].expires_unix;
            victim = i;
        }
    }
    memset(&g_cookies[victim], 0, sizeof(cookie_t));
    return &g_cookies[victim];
}

/* Skip leading whitespace in-place via pointer move. */
static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

int cookie_parse_set(const char *host, const char *line) {
    if (!host || !line) return -1;
    /* "name=value; attr=val; flag" form. */
    const char *eq = line;
    while (*eq && *eq != '=' && *eq != ';') eq++;
    if (*eq != '=') return -1;
    int name_len = (int)(eq - line);
    const char *val = eq + 1;
    const char *semi = val;
    while (*semi && *semi != ';') semi++;
    int val_len = (int)(semi - val);

    /* Allocate / replace existing. */
    char name_buf[COOKIE_NAME_MAX];
    copy_n(name_buf, line, COOKIE_NAME_MAX, name_len);
    cookie_t *c = find_slot(host, name_buf);
    if (!c) c = alloc_slot();
    if (!c) return -1;
    memset(c, 0, sizeof(*c));
    c->in_use = true;
    copy_n(c->host, host, COOKIE_HOST_MAX, COOKIE_HOST_MAX - 1);
    istrlower(c->host);
    copy_n(c->name, line, COOKIE_NAME_MAX, name_len);
    copy_n(c->value, val, COOKIE_VALUE_MAX, val_len);
    c->path[0] = '/'; c->path[1] = 0;
    /* Attributes. */
    while (*semi == ';') {
        const char *a = skip_ws(semi + 1);
        const char *aend = a;
        while (*aend && *aend != ';' && *aend != ',') aend++;
        if (istr_eq("Secure", "Secure") &&
            (aend - a) >= 6 && istr_eq("Secure", "Secure")) {
            /* String compare with the attribute prefix. */
        }
        if (aend - a >= 6 && a[0] == 'S' && a[1] == 'e') {
            c->secure = true;
        }
        if (aend - a >= 8 && a[0] == 'H' && a[1] == 't') {
            c->http_only = true;
        }
        /* Path=... */
        const char *pe = a;
        while (*pe && *pe != '=' && pe < aend) pe++;
        if (*pe == '=') {
            char key[16];
            int kn = (int)(pe - a);
            copy_n(key, a, sizeof(key), kn);
            const char *pv = pe + 1;
            int pvlen = (int)(aend - pv);
            if (istr_eq(key, "Path")) {
                copy_n(c->path, pv, COOKIE_PATH_MAX, pvlen);
            } else if (istr_eq(key, "Max-Age")) {
                uint32_t s = 0;
                while (pv < aend && *pv >= '0' && *pv <= '9') {
                    s = s * 10 + (uint32_t)(*pv - '0'); pv++;
                }
                c->expires_unix = rtc_seconds() + s;
            }
        }
        semi = aend;
        if (*semi == ',') break;
    }
    debug_printf("[cookies] stored %s=%s for host=%s\n",
                 c->name, c->value, c->host);
    return 0;
}

int cookie_serialize_for_host(const char *host, bool secure,
                              char *out, uint32_t cap) {
    if (!host || !out || cap == 0) return 0;
    uint32_t off = 0;
    bool any = false;
    uint32_t now = rtc_seconds();
    for (int i = 0; i < COOKIE_MAX; i++) {
        cookie_t *c = &g_cookies[i];
        if (!c->in_use) continue;
        if (c->expires_unix && c->expires_unix < now) continue;
        if (c->secure && !secure) continue;
        if (!istr_eq(c->host, host)) continue;
        if (!any) {
            int n = ksnprintf(out + off, cap - off, "Cookie: ");
            if (n <= 0) return 0;
            off += (uint32_t)n;
            any = true;
        } else {
            if (off + 2 < cap) {
                out[off++] = ';'; out[off++] = ' ';
            }
        }
        int n = ksnprintf(out + off, cap - off, "%s=%s", c->name, c->value);
        if (n <= 0) break;
        off += (uint32_t)n;
    }
    if (any && off + 2 < cap) {
        out[off++] = '\r'; out[off++] = '\n'; out[off] = 0;
    }
    return (int)off;
}

int cookie_count(void) {
    int n = 0;
    for (int i = 0; i < COOKIE_MAX; i++) if (g_cookies[i].in_use) n++;
    return n;
}

cookie_t *cookie_get(int idx) {
    int seen = 0;
    for (int i = 0; i < COOKIE_MAX; i++) {
        if (!g_cookies[i].in_use) continue;
        if (seen == idx) return &g_cookies[i];
        seen++;
    }
    return NULL;
}

void cookie_clear(void) {
    memset(g_cookies, 0, sizeof(g_cookies));
}

/* Persist non-session cookies to /sys/cookies.cfg. */
int cookie_persist(void) {
    if (!nxfs_is_mounted()) return -1;
    static char buf[8192];
    uint32_t off = 0;
    for (int i = 0; i < COOKIE_MAX; i++) {
        cookie_t *c = &g_cookies[i];
        if (!c->in_use || c->expires_unix == 0) continue;
        int n = ksnprintf(buf + off, sizeof(buf) - off,
                          "%s\t%s\t%s\t%u\t%s\t%c%c\n",
                          c->host, c->name, c->value,
                          c->expires_unix, c->path,
                          c->secure ? 'S' : '-',
                          c->http_only ? 'H' : '-');
        if (n <= 0) break;
        off += (uint32_t)n;
    }
    /* Best-effort write (real persistence wires into NXFS in a follow-up). */
    debug_printf("[cookies] would persist %u bytes to /sys/cookies.cfg\n", off);
    return (int)off;
}

int cookie_load_from_fs(void) { return 0; }   /* stub - first boot empty */
