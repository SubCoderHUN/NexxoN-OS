/* ============================================================================
 * NexxoN OS - System clipboard implementation
 * ---------------------------------------------------------------------------- */
#include "clipboard.h"
#include "string.h"
#include "debug.h"

static uint8_t     g_buf[CLIPBOARD_MAX_BYTES + 1];   /* +1 keeps a NUL for text peek */
static uint32_t    g_len = 0;
static clip_mime_t g_mime = CLIP_MIME_TEXT_PLAIN;

uint32_t clipboard_set(const void *data, uint32_t len, clip_mime_t mime) {
    if (!data || len == 0) {
        g_len = 0;
        g_buf[0] = 0;
        g_mime = mime;
        return 0;
    }
    if (len > CLIPBOARD_MAX_BYTES) len = CLIPBOARD_MAX_BYTES;
    memcpy(g_buf, data, len);
    g_buf[len] = 0;
    g_len  = len;
    g_mime = mime;
    debug_printf("[clip] set %u bytes (mime=%d)\n", len, (int)mime);
    return len;
}

uint32_t clipboard_get(void *buf, uint32_t cap, clip_mime_t *out_mime) {
    if (!buf || cap == 0) return 0;
    uint32_t n = (g_len < cap) ? g_len : cap;
    memcpy(buf, g_buf, n);
    if (out_mime) *out_mime = g_mime;
    return n;
}

uint32_t clipboard_len(void) { return g_len; }

uint32_t clipboard_set_text(const char *s, uint32_t len) {
    if (!s) return clipboard_set(NULL, 0, CLIP_MIME_TEXT_PLAIN);
    if (len == 0) len = (uint32_t)strlen(s);
    return clipboard_set(s, len, CLIP_MIME_TEXT_PLAIN);
}

const char *clipboard_peek_text(void) {
    return (const char *)g_buf;
}

/* ============================================================================
 * TASK 25: Multi-MIME clipboard slots.  Each MIME tag has its own
 * payload buffer so an app can publish a single object in multiple
 * representations (e.g. "text/html" + "text/plain" + "image/bmp")
 * and consumers can fetch whichever one they understand.
 * ============================================================================ */
typedef struct {
    bool     in_use;
    char     mime[CLIPBOARD_MIME_MAX];
    uint8_t  data[CLIP_TYPED_MAX];
    uint32_t len;
} clip_slot_t;

static clip_slot_t g_slots[CLIP_FORMAT_MAX];

static clip_slot_t *slot_for(const char *mime) {
    for (int i = 0; i < CLIP_FORMAT_MAX; i++) {
        if (g_slots[i].in_use && strcmp(g_slots[i].mime, mime) == 0)
            return &g_slots[i];
    }
    return NULL;
}

static clip_slot_t *alloc_slot(const char *mime) {
    clip_slot_t *exist = slot_for(mime);
    if (exist) return exist;
    for (int i = 0; i < CLIP_FORMAT_MAX; i++) {
        if (!g_slots[i].in_use) {
            g_slots[i].in_use = true;
            int n = 0;
            while (mime[n] && n < CLIPBOARD_MIME_MAX - 1) {
                g_slots[i].mime[n] = mime[n]; n++;
            }
            g_slots[i].mime[n] = 0;
            return &g_slots[i];
        }
    }
    return NULL;
}

int clipboard_set_mime(const char *mime, const void *data, uint32_t len) {
    if (!mime) return -1;
    if (len > CLIP_TYPED_MAX) len = CLIP_TYPED_MAX;
    if (!data || len == 0) {
        clip_slot_t *s = slot_for(mime);
        if (s) s->in_use = false;
        return 0;
    }
    clip_slot_t *s = alloc_slot(mime);
    if (!s) return -1;
    memcpy(s->data, data, len);
    s->len = len;
    /* Mirror text/plain into the legacy single-slot store for backward
     * compatibility with the old clipboard_get_text path. */
    if (strcmp(mime, "text/plain") == 0) {
        clipboard_set(data, len, CLIP_MIME_TEXT_PLAIN);
    }
    debug_printf("[clip] mime '%s' set %u bytes\n", mime, len);
    return (int)len;
}

int clipboard_get_mime(const char *mime, void *buf, uint32_t cap) {
    if (!mime) return -1;
    clip_slot_t *s = slot_for(mime);
    if (!s) return -1;
    uint32_t want = (s->len < cap) ? s->len : cap;
    memcpy(buf, s->data, want);
    return (int)want;
}

int clipboard_list_mimes(int idx, char *out, uint32_t cap) {
    if (idx < 0 || !out || cap == 0) return -1;
    int seen = 0;
    for (int i = 0; i < CLIP_FORMAT_MAX; i++) {
        if (!g_slots[i].in_use) continue;
        if (seen == idx) {
            uint32_t n = 0;
            while (g_slots[i].mime[n] && n < cap - 1) {
                out[n] = g_slots[i].mime[n]; n++;
            }
            out[n] = 0;
            return (int)n;
        }
        seen++;
    }
    return -1;
}
