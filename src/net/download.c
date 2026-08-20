/* ============================================================================
 * NexxoN OS - HTTP / HTTPS download manager (system-level)
 * ----------------------------------------------------------------------------
 * Streams a remote resource into a caller-supplied buffer.  Supports:
 *
 *   * http:// over the regular TCP socket API (net.c).
 *   * https:// via tls_connect() (TLS is still gated on RSA - see crypto.c).
 *   * HTTP/1.1 chunked transfer-encoding (transparently de-chunked).
 *   * Hosts written as a dotted-quad IPv4 literal.  DNS resolution is not
 *     yet wired into the kernel; once it is, the resolver simply replaces
 *     parse_host_to_ip() and the rest of this file keeps working.
 *
 * The fetcher is synchronous: download_fetch() blocks the calling task
 * until the response body has been fully received or the timeout fires.
 * That's fine for everything that uses it today (nxpkg install, browser
 * page load) because they all run in their own task context.
 * ============================================================================ */
#include "download.h"
#include "net.h"
#include "crypto.h"
#include "cookies.h"
#include "string.h"
#include "debug.h"
#include "pit.h"

static int istarts(const char *s, const char *prefix) {
    while (*prefix) {
        char a = *s++, b = *prefix++;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

/* Parse "http://1.2.3.4:80/path" into scheme + host + port + path. */
typedef struct {
    bool   secure;
    char   host[DOWNLOAD_MAX_HOST];
    uint16_t port;
    char   path[DOWNLOAD_MAX_URL];
} parsed_url_t;

static bool parse_url(const char *url, parsed_url_t *out) {
    memset(out, 0, sizeof(*out));
    if (istarts(url, "https://"))      { out->secure = true;  url += 8; out->port = 443; }
    else if (istarts(url, "http://"))  { out->secure = false; url += 7; out->port = 80;  }
    else                               { return false; }
    int n = 0;
    /* Host part: until ':' or '/'. */
    while (*url && *url != ':' && *url != '/' && n < DOWNLOAD_MAX_HOST - 1) {
        out->host[n++] = *url++;
    }
    out->host[n] = 0;
    if (*url == ':') {
        url++;
        uint32_t p = 0;
        while (*url >= '0' && *url <= '9') {
            p = p * 10 + (uint32_t)(*url - '0');
            url++;
        }
        out->port = (uint16_t)p;
    }
    if (*url == 0) {
        out->path[0] = '/'; out->path[1] = 0;
        return true;
    }
    n = 0;
    while (*url && n < DOWNLOAD_MAX_URL - 1) out->path[n++] = *url++;
    out->path[n] = 0;
    return true;
}

/* Build a minimal GET request into `buf`.  Returns the number of bytes
 * written. */
static int build_get(const parsed_url_t *p, char *buf, uint32_t cap) {
    /* Cookie injection: ask the store for matching cookies and append
     * them to the request header block before the terminating CRLF. */
    char cookie_hdr[1024];
    cookie_hdr[0] = 0;
    cookie_serialize_for_host(p->host, p->secure, cookie_hdr,
                              sizeof(cookie_hdr));
    return ksnprintf(buf, cap,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: NexxoN/1.0\r\n"
                     "Accept: */*\r\n"
                     "%s"
                     "Connection: close\r\n"
                     "\r\n",
                     p->path, p->host, cookie_hdr);
}

/* Host being fetched - stashed before parse_header() so the Set-Cookie
 * loop can attribute incoming cookies to the right origin. */
static const char *g_active_host = NULL;

/* Locate the end of the response header ("\r\n\r\n").  Returns the
 * offset of the first body byte, or -1 if not found. */
static int find_header_end(const uint8_t *buf, uint32_t len) {
    for (uint32_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return (int)(i + 4);
    }
    return -1;
}

/* Parse the HTTP status line + Content-Length / Content-Type / Transfer-
 * Encoding into `out`.  `hdr` is a NUL-terminated header block. */
static void parse_header(const char *hdr, download_result_t *out, bool *chunked) {
    /* Buffer hostname so we can attribute Set-Cookie back to it. */
    /* parse_header is called with the response header block already
     * captured; the host comes from the caller via a thread-local stash
     * since this static fn doesn't see the parsed URL.  We use a static
     * pointer cached by download_fetch right before calling us. */
    *chunked = false;
    out->status_code = 0;
    /* Status line: "HTTP/1.1 200 OK". */
    if (istarts(hdr, "HTTP/")) {
        while (*hdr && *hdr != ' ') hdr++;
        while (*hdr == ' ') hdr++;
        uint32_t code = 0;
        while (*hdr >= '0' && *hdr <= '9') {
            code = code * 10 + (uint32_t)(*hdr - '0');
            hdr++;
        }
        out->status_code = (int)code;
    }
    const char *line = hdr;
    while (*line) {
        const char *eol = line;
        while (*eol && !(eol[0] == '\r' && eol[1] == '\n')) eol++;
        if (line[0] == '\r') break;
        if (istarts(line, "Content-Length:")) {
            const char *v = line + 15;
            while (*v == ' ') v++;
            uint32_t cl = 0;
            while (*v >= '0' && *v <= '9') {
                cl = cl * 10 + (uint32_t)(*v - '0');
                v++;
            }
            out->content_length = cl;
        } else if (istarts(line, "Content-Type:")) {
            const char *v = line + 13;
            while (*v == ' ') v++;
            int n = 0;
            while (v < eol && n < (int)sizeof(out->content_type) - 1) {
                out->content_type[n++] = *v++;
            }
            out->content_type[n] = 0;
        } else if (istarts(line, "Transfer-Encoding:")) {
            const char *v = line + 18;
            while (*v == ' ') v++;
            if (istarts(v, "chunked")) *chunked = true;
        } else if (istarts(line, "Set-Cookie:") && g_active_host) {
            const char *v = line + 11;
            while (*v == ' ') v++;
            /* Copy just the header value into a scratch + parse. */
            char scratch[512];
            int n = 0;
            while (v < eol && n < (int)sizeof(scratch) - 1) {
                scratch[n++] = *v++;
            }
            scratch[n] = 0;
            cookie_parse_set(g_active_host, scratch);
        }
        line = eol;
        if (*line == '\r') line += 2;
    }
}

/* Decode a chunked body in place.  Returns the decoded length. */
static uint32_t decode_chunked(uint8_t *buf, uint32_t len) {
    uint32_t out = 0;
    uint32_t off = 0;
    while (off < len) {
        /* Read the chunk-size line in hex. */
        uint32_t size = 0;
        while (off < len && buf[off] != '\r') {
            char c = (char)buf[off++];
            if (c >= '0' && c <= '9') size = (size << 4) | (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') size = (size << 4) | (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') size = (size << 4) | (uint32_t)(c - 'A' + 10);
            else if (c == ';') break;     /* chunk-ext */
        }
        while (off < len && buf[off] != '\n') off++;
        if (off < len) off++;
        if (size == 0) break;
        if (off + size > len) size = len - off;
        memmove(buf + out, buf + off, size);
        out += size;
        off += size;
        /* Trailing CRLF after each chunk. */
        if (off + 2 <= len) off += 2;
    }
    return out;
}

int download_fetch(const download_request_t *req, download_result_t *res) {
    if (!req || !res) return -1;
    memset(res, 0, sizeof(*res));
    parsed_url_t p;
    if (!parse_url(req->url, &p)) {
        ksnprintf(res->error, sizeof(res->error), "invalid URL");
        return -1;
    }
    uint32_t timeout = req->timeout_ms ? req->timeout_ms : 10000;
    /* Resolve hostname: dotted-quad first, then DNS (incl. static hosts). */
    uint32_t ip = net_ip_aton(p.host);
    if (ip == 0) {
        if (net_dns_resolve(p.host, &ip, timeout) != 0 || ip == 0) {
            ksnprintf(res->error, sizeof(res->error), "DNS resolve failed");
            return -1;
        }
        debug_printf("[download] DNS %s -> %u.%u.%u.%u\n", p.host,
                     (ip) & 0xFF, (ip >> 8) & 0xFF,
                     (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
    }

    char request[512];
    int reqlen = build_get(&p, request, sizeof(request));
    if (reqlen <= 0) {
        ksnprintf(res->error, sizeof(res->error), "request too large");
        return -1;
    }

    /* Send. */
    if (p.secure) {
        tls_handle_t h = tls_connect(ip, p.port, p.host, timeout);
        if (h == TLS_INVALID) {
            ksnprintf(res->error, sizeof(res->error),
                      "TLS unavailable (handshake aborted)");
            return -1;
        }
        if (tls_send(h, request, reqlen) != reqlen) {
            tls_close(h);
            ksnprintf(res->error, sizeof(res->error), "TLS send failed");
            return -1;
        }
        uint32_t total = 0;
        while (total < req->out_cap) {
            int n = tls_recv(h, req->out_buf + total,
                             req->out_cap - total, timeout);
            if (n <= 0) break;
            total += (uint32_t)n;
        }
        tls_close(h);
        res->bytes_received = total;
    } else {
        tcp_handle_t s = tcp_connect(ip, p.port, timeout);
        if (s == TCP_INVALID) {
            ksnprintf(res->error, sizeof(res->error), "TCP connect failed");
            return -1;
        }
        int sent = tcp_send(s, request, reqlen);
        if (sent != reqlen) {
            tcp_close(s);
            ksnprintf(res->error, sizeof(res->error), "TCP send failed");
            return -1;
        }
        uint32_t total = 0;
        while (total < req->out_cap) {
            int n = tcp_recv(s, req->out_buf + total,
                             req->out_cap - total, timeout);
            if (n <= 0) break;
            total += (uint32_t)n;
        }
        tcp_close(s);
        res->bytes_received = total;
    }

    /* Split header / body. */
    int body_off = find_header_end(req->out_buf, res->bytes_received);
    if (body_off < 0) {
        ksnprintf(res->error, sizeof(res->error), "no header terminator");
        return -1;
    }
    /* Null-terminate the header for parsing. */
    if ((uint32_t)body_off <= req->out_cap) {
        char saved = (char)req->out_buf[body_off - 4];
        req->out_buf[body_off - 4] = 0;
        bool chunked = false;
        g_active_host = p.host;
        parse_header((const char *)req->out_buf, res, &chunked);
        g_active_host = NULL;
        req->out_buf[body_off - 4] = (uint8_t)saved;
        uint32_t body_len = res->bytes_received - (uint32_t)body_off;
        if (!chunked && res->content_length &&
            body_len < res->content_length) {
            ksnprintf(res->error, sizeof(res->error),
                      "truncated body (%u/%u)", body_len,
                      res->content_length);
            debug_printf("[download] truncated %s%s: %u/%u bytes\n",
                         p.secure ? "https://" : "http://", p.host,
                         body_len, res->content_length);
            return -1;
        }
        if (chunked) {
            body_len = decode_chunked(req->out_buf + body_off, body_len);
        }
        memmove(req->out_buf, req->out_buf + body_off, body_len);
        res->bytes_received = body_len;
    }
    res->ok = (res->status_code >= 200 && res->status_code < 300);
    debug_printf("[download] %s%s ok=%d code=%d bytes=%u type='%s'\n",
                 p.secure ? "https://" : "http://", p.host,
                 res->ok, res->status_code, res->bytes_received,
                 res->content_type);
    return res->ok ? (int)res->bytes_received : -1;
}

int download_simple(const char *url, void *buf, uint32_t cap) {
    download_request_t req = { {0}, (uint8_t *)buf, cap, 10000 };
    int n = 0;
    while (url[n] && n < DOWNLOAD_MAX_URL - 1) {
        req.url[n] = url[n];
        n++;
    }
    req.url[n] = 0;
    download_result_t res;
    return download_fetch(&req, &res);
}
