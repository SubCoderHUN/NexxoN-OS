/* ============================================================================
 * NexxoN OS - Cookie / session manager  (v1.0)
 * ----------------------------------------------------------------------------
 * Persists HTTP cookies to /sys/cookies.cfg with one record per line:
 *
 *     host\tname\tvalue\texpires\tpath\tflags
 *
 * The download manager (download.c) parses Set-Cookie response headers
 * and forwards them to cookie_store(); on every outgoing request the
 * fetcher calls cookie_serialize_for_host() to assemble the matching
 * "Cookie:" header.  Expiration uses RTC seconds so the cache evicts
 * itself across reboots once the wall clock crosses each entry's
 * expiry.
 *
 * Limits (BSS-sized, no heap):
 *   COOKIE_MAX           - 32 entries
 *   COOKIE_HOST_MAX      - 64
 *   COOKIE_NAME_MAX      - 48
 *   COOKIE_VALUE_MAX     - 192
 *   COOKIE_PATH_MAX      - 32
 * ============================================================================ */
#ifndef NEXXON_COOKIES_H
#define NEXXON_COOKIES_H

#include "types.h"

#define COOKIE_MAX        32
#define COOKIE_HOST_MAX   64
#define COOKIE_NAME_MAX   48
#define COOKIE_VALUE_MAX  192
#define COOKIE_PATH_MAX   32

typedef struct {
    bool      in_use;
    bool      secure;        /* only sent on HTTPS */
    bool      http_only;
    uint32_t  expires_unix;  /* 0 = session */
    char      host[COOKIE_HOST_MAX];
    char      name[COOKIE_NAME_MAX];
    char      value[COOKIE_VALUE_MAX];
    char      path[COOKIE_PATH_MAX];
} cookie_t;

void cookie_init        (void);
int  cookie_load_from_fs(void);                  /* /sys/cookies.cfg */
int  cookie_persist     (void);

/* Parse one Set-Cookie header line ("name=value; Path=/; Secure; ...") */
int  cookie_parse_set   (const char *host, const char *header_line);

/* Build a "Cookie: a=1; b=2\r\n" header into `out`; returns bytes written. */
int  cookie_serialize_for_host(const char *host, bool secure,
                               char *out, uint32_t cap);

int  cookie_count       (void);
cookie_t *cookie_get    (int idx);
void cookie_clear       (void);

#endif /* NEXXON_COOKIES_H */
