/* ============================================================================
 * NexxoN OS - Framework HTTP / HTTPS download manager  (v1.0)
 * ----------------------------------------------------------------------------
 * A polled, single-buffer streaming fetcher that lives on top of the
 * regular TCP socket API (and, once the TLS handshake is finished, the
 * TLS handle API).  Exposed to Ring 3 as SYS_DOWNLOAD so that any user
 * application - the browser, the package manager, the future image
 * viewer - can fetch a remote resource with a single syscall.
 *
 *   download_request_t req = { "http://example.com/foo.zip", buf, cap };
 *   int got = download_fetch(&req);
 *
 * The fetcher parses the URL, opens TCP, sends a minimal HTTP/1.1 GET
 * (Host header derived from the URL, Connection: close), drains the
 * response, strips the response header, and copies the body into the
 * caller-supplied buffer.  On https:// it forwards to tls_connect()
 * which currently aborts at the handshake (see crypto.c) - the call
 * surface is identical so once RSA / ECDHE lands the apps light up.
 *
 * Capped at 16 MiB per fetch in this milestone; chunked transfer is
 * decoded transparently.
 * ============================================================================ */
#ifndef NEXXON_DOWNLOAD_H
#define NEXXON_DOWNLOAD_H

#include "types.h"

#define DOWNLOAD_MAX_URL    256
#define DOWNLOAD_MAX_HOST   128
#define DOWNLOAD_MAX_BODY   (16u * 1024u * 1024u)

typedef struct {
    char     url[DOWNLOAD_MAX_URL];
    uint8_t *out_buf;
    uint32_t out_cap;
    uint32_t timeout_ms;
} download_request_t;

typedef struct {
    bool     ok;
    int      status_code;            /* 200, 404, ...                    */
    uint32_t content_length;         /* may be 0 for chunked              */
    uint32_t bytes_received;
    char     content_type[64];
    char     error[64];
} download_result_t;

int download_fetch(const download_request_t *req, download_result_t *res);

/* Convenience helper for in-tree clients (browser address bar, nxpkg).
 * Same as download_fetch but takes the URL + buffer/cap directly and
 * returns the bytes received (or -1 on error). */
int download_simple(const char *url, void *buf, uint32_t cap);

#endif /* NEXXON_DOWNLOAD_H */
