/* ============================================================================
 * NexxoN OS - Minimal X11 display :0 (drawing + VGA present + events)
 * ============================================================================ */
#include "lin_x11.h"
#include "debug.h"
#include "font.h"
#include "keyboard.h"
#include "mouse.h"
#include "string.h"
#include "pit.h"

extern void lin_unix_link_reply(int link_ix, const void *data, uint32_t len);
extern int lin_unix_register_x11_listener(void);
extern void vga_putpixel(int x, int y, uint32_t c);
extern uint32_t vga_width(void);
extern uint32_t vga_height(void);

#define X11_LINK_MAX 16
#define X11_IN_CAP   32768
#define X11_EVT_CAP  32
#define X11_ROOT     0x00000100u
#define X11_VISUAL   0x00000020u
#define X11_CMAP     0x00000021u
#define X11_WINDOW_MAX 16
#define X11_FB_W     800u
#define X11_FB_H     600u
/* Re-present cadence: the compositor owns the LFB and repaints over the
 * X surface on its own damage; a periodic re-blit keeps X content visible
 * without marking WM damage (which would just invite an overwrite). */
#define X11_REPRESENT_MS 100u
#define X11_EVENT_EXPOSURE          (1u << 15)
#define X11_EVENT_STRUCTURE_NOTIFY  (1u << 17)
#define X11_EVENT_KEY_PRESS         (1u << 0)
#define X11_EVENT_BUTTON_PRESS      (1u << 2)
#define X11_EVENT_BUTTON_RELEASE    (1u << 3)
#define X11_EVENT_POINTER_MOTION    (1u << 6)

typedef struct {
    bool used;
    uint32_t id;
    int16_t x, y;
    uint16_t width, height;
    uint32_t event_mask;
    bool mapped;
} lin_x11_window_t;

typedef struct {
    uint8_t data[32];
} lin_x11_event_t;

#define X11_GC_MAX 16

typedef struct {
    bool used;
    uint32_t id;
    uint32_t foreground;
} lin_x11_gc_t;

typedef struct {
    bool active;
    bool setup_done;
    bool little;
    uint16_t sequence;
    uint8_t in[X11_IN_CAP];
    uint32_t in_len;
    uint32_t next_atom;
    uint32_t next_win_id;
    lin_x11_window_t windows[X11_WINDOW_MAX];
    lin_x11_gc_t gcs[X11_GC_MAX];
    lin_x11_event_t events[X11_EVT_CAP];
    uint32_t evt_head;
    uint32_t evt_tail;
    int16_t last_mx, last_my;
    uint8_t last_btn;
} lin_x11_client_t;

static lin_x11_client_t g_x11[X11_LINK_MAX];
static uint32_t g_x11_screen[X11_FB_W * X11_FB_H];
static bool g_x11_dirty;
static bool g_x11_have_content;
static uint32_t g_x11_last_present_ms;

static uint16_t rd16(const uint8_t *p, bool little) {
    if (little)
        return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static uint32_t rd32(const uint8_t *p, bool little) {
    if (little)
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wr16(uint8_t *p, uint16_t v, bool little) {
    if (little) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
    else { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
}

static void wr32(uint8_t *p, uint32_t v, bool little) {
    if (little) {
        p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
    } else {
        p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
        p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
    }
}

static lin_x11_window_t *window_find(lin_x11_client_t *c, uint32_t id) {
    for (int i = 0; i < X11_WINDOW_MAX; i++)
        if (c->windows[i].used && c->windows[i].id == id)
            return &c->windows[i];
    return NULL;
}

static void x11_put_zpixmap(int16_t ox, int16_t oy, uint16_t w, uint16_t h,
                            uint8_t depth, const uint8_t *data,
                            uint32_t data_len) {
    uint32_t bpp = depth <= 16 ? 2u : 4u;
    uint32_t stride = (uint32_t)w * bpp;
    for (uint16_t row = 0; row < h; row++) {
        uint32_t src_off = (uint32_t)row * stride;
        if (src_off + stride > data_len) break;
        int32_t dy = (int32_t)oy + (int32_t)row;
        if (dy < 0 || (uint32_t)dy >= X11_FB_H) continue;
        for (uint16_t col = 0; col < w; col++) {
            int32_t dx = (int32_t)ox + (int32_t)col;
            if (dx < 0 || (uint32_t)dx >= X11_FB_W) continue;
            uint32_t src = src_off + (uint32_t)col * bpp;
            if (src + bpp > data_len) break;
            uint32_t px = 0xFF1B2838u;
            if (bpp == 4) {
                px = 0xFF000000u | ((uint32_t)data[src + 2] << 16) |
                     ((uint32_t)data[src + 1] << 8) | (uint32_t)data[src];
            }
            g_x11_screen[(uint32_t)dy * X11_FB_W + (uint32_t)dx] = px;
        }
    }
    g_x11_dirty = true;
}

static void x11_fill_rect(int16_t ox, int16_t oy, uint16_t w, uint16_t h,
                          uint32_t pixel) {
    for (uint16_t row = 0; row < h; row++) {
        int32_t dy = (int32_t)oy + (int32_t)row;
        if (dy < 0 || (uint32_t)dy >= X11_FB_H) continue;
        for (uint16_t col = 0; col < w; col++) {
            int32_t dx = (int32_t)ox + (int32_t)col;
            if (dx < 0 || (uint32_t)dx >= X11_FB_W) continue;
            g_x11_screen[(uint32_t)dy * X11_FB_W + (uint32_t)dx] = pixel;
        }
    }
    g_x11_dirty = true;
}

/* 8x8 kernel glyphs; the wire y is the BASELINE (X core text semantics). */
static void x11_draw_glyph(int16_t ox, int16_t oy, uint8_t ch, uint32_t color) {
    const uint8_t *glyph = font8x8[ch];
    for (int row = 0; row < 8; row++) {
        int32_t dy = (int32_t)oy - 8 + row;
        if (dy < 0 || (uint32_t)dy >= X11_FB_H) continue;
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (!(bits & (0x80u >> col))) continue;
            int32_t dx = (int32_t)ox + col;
            if (dx < 0 || (uint32_t)dx >= X11_FB_W) continue;
            g_x11_screen[(uint32_t)dy * X11_FB_W + (uint32_t)dx] = color;
        }
    }
    g_x11_dirty = true;
}

static void lin_x11_present(void) {
    if (!g_x11_dirty) return;
    uint32_t sw = vga_width(), sh = vga_height();
    if (!sw || !sh) return;
    int ox = ((int)sw - (int)X11_FB_W) / 2;
    int oy = ((int)sh - (int)X11_FB_H) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    for (uint32_t y = 0; y < X11_FB_H; y++) {
        if ((uint32_t)oy + y >= sh) break;
        for (uint32_t x = 0; x < X11_FB_W; x++) {
            if ((uint32_t)ox + x >= sw) break;
            vga_putpixel(ox + (int)x, oy + (int)y,
                         g_x11_screen[y * X11_FB_W + x]);
        }
    }
    g_x11_dirty = false;
    g_x11_have_content = true;
    g_x11_last_present_ms = pit_ms();
}

/* Periodic re-blit from the service pump: restores the X surface after
 * the compositor's own repaints without creating a damage feedback loop. */
static void lin_x11_represent_tick(void) {
    if (!g_x11_have_content || g_x11_dirty)
        return;
    if ((uint32_t)(pit_ms() - g_x11_last_present_ms) < X11_REPRESENT_MS)
        return;
    g_x11_dirty = true;
    lin_x11_present();
}

static void x11_queue_event(lin_x11_client_t *c, uint8_t type,
                            uint32_t win_id, uint16_t w, uint16_t h) {
    uint32_t next = (c->evt_tail + 1) % X11_EVT_CAP;
    if (next == c->evt_head)
        return;
    uint8_t *ev = c->events[c->evt_tail].data;
    memset(ev, 0, 32);
    ev[0] = type;
    wr16(ev + 2, c->sequence, c->little);
    wr32(ev + 4, win_id, c->little);
    if (type == 12) { /* Expose */
        wr16(ev + 8, 0, c->little);
        wr16(ev + 10, 0, c->little);
        wr16(ev + 12, w, c->little);
        wr16(ev + 14, h, c->little);
    } else if (type == 19) { /* MapNotify */
        wr32(ev + 8, win_id, c->little);
        ev[16] = 0;
    }
    c->evt_tail = next;
}

static void x11_flush_events(int link_ix, lin_x11_client_t *c) {
    while (c->evt_head != c->evt_tail) {
        lin_unix_link_reply(link_ix, c->events[c->evt_head].data, 32);
        c->evt_head = (c->evt_head + 1) % X11_EVT_CAP;
    }
}

static void x11_emit_map_events(int link_ix, lin_x11_client_t *c,
                                lin_x11_window_t *w) {
    if (!w || !w->mapped)
        return;
    if (w->event_mask & X11_EVENT_STRUCTURE_NOTIFY)
        x11_queue_event(c, 19, w->id, w->width, w->height);
    if (w->event_mask & X11_EVENT_EXPOSURE)
        x11_queue_event(c, 12, w->id, w->width, w->height);
    x11_flush_events(link_ix, c);
}

static uint8_t ascii_to_keycode(int ch) {
    if (ch >= 'a' && ch <= 'z') return (uint8_t)(ch - 'a' + 38);
    if (ch >= 'A' && ch <= 'Z') return (uint8_t)(ch - 'A' + 38);
    if (ch >= '0' && ch <= '9') return (uint8_t)(ch - '0' + 10);
    if (ch == ' ') return 65;
    if (ch == KEY_ENTER) return 36;
    if (ch == KEY_ESCAPE) return 9;
    return 65;
}

static void x11_emit_pointer(int link_ix, lin_x11_client_t *c,
                             lin_x11_window_t *w, uint8_t type,
                             int mx, int my, uint8_t btn) {
    if (!w || !w->mapped)
        return;
    if (type == 6 && !(w->event_mask & X11_EVENT_POINTER_MOTION))
        return;
    if (type == 4 && !(w->event_mask & X11_EVENT_BUTTON_PRESS))
        return;
    if (type == 5 && !(w->event_mask & X11_EVENT_BUTTON_RELEASE))
        return;
    uint8_t ev[32];
    memset(ev, 0, sizeof(ev));
    ev[0] = type;
    wr16(ev + 2, c->sequence, c->little);
    wr32(ev + 4, X11_ROOT, c->little);
    wr32(ev + 8, w->id, c->little);
    wr32(ev + 12, 0, c->little);
    int16_t ex = (int16_t)(mx - w->x);
    int16_t ey = (int16_t)(my - w->y);
    wr16(ev + 16, (uint16_t)mx, c->little);
    wr16(ev + 18, (uint16_t)my, c->little);
    wr16(ev + 20, (uint16_t)ex, c->little);
    wr16(ev + 22, (uint16_t)ey, c->little);
    if (type == 4 || type == 5)
        ev[1] = btn ? btn : 1;
    lin_unix_link_reply(link_ix, ev, 32);
}

static void x11_emit_keypress(int link_ix, lin_x11_client_t *c,
                            lin_x11_window_t *w, int ch) {
    if (!w || !w->mapped || !(w->event_mask & X11_EVENT_KEY_PRESS))
        return;
    uint8_t ev[32];
    memset(ev, 0, sizeof(ev));
    ev[0] = 2;
    ev[1] = ascii_to_keycode(ch);
    wr16(ev + 2, c->sequence, c->little);
    wr32(ev + 4, X11_ROOT, c->little);
    wr32(ev + 8, w->id, c->little);
    wr32(ev + 12, 0, c->little);
    lin_unix_link_reply(link_ix, ev, 32);
}

static lin_x11_window_t *x11_focus_window(lin_x11_client_t *c) {
    for (int i = X11_WINDOW_MAX - 1; i >= 0; i--) {
        if (c->windows[i].used && c->windows[i].mapped)
            return &c->windows[i];
    }
    return NULL;
}

void lin_x11_service(int link_ix) {
    if (link_ix < 0 || link_ix >= X11_LINK_MAX)
        return;
    lin_x11_represent_tick();
    lin_x11_client_t *c = &g_x11[link_ix];
    if (!c->active || !c->setup_done)
        return;
    lin_x11_window_t *w = x11_focus_window(c);
    if (!w)
        return;

    int mx = 0, my = 0;
    uint8_t btn = 0;
    if (mouse_poll(&mx, &my, &btn)) {
        if (mx != c->last_mx || my != c->last_my)
            x11_emit_pointer(link_ix, c, w, 6, mx, my, btn);
        if (btn && !c->last_btn)
            x11_emit_pointer(link_ix, c, w, 4, mx, my, btn);
        else if (!btn && c->last_btn)
            x11_emit_pointer(link_ix, c, w, 5, mx, my, c->last_btn);
        c->last_mx = (int16_t)mx;
        c->last_my = (int16_t)my;
        c->last_btn = btn;
    }
    if (keyboard_has_data()) {
        int ch = keyboard_getc();
        if (ch > 0)
            x11_emit_keypress(link_ix, c, w, ch);
    }
}

void lin_x11_display_init(void) {
    memset(g_x11, 0, sizeof(g_x11));
    memset(g_x11_screen, 0, sizeof(g_x11_screen));
    if (lin_unix_register_x11_listener() == 0)
        debug_printf("[linux/x11] display :0 @/tmp/.X11-unix/X0 ready\n");
}

void lin_x11_on_connect(int link_ix) {
    if (link_ix < 0 || link_ix >= X11_LINK_MAX) return;
    memset(&g_x11[link_ix], 0, sizeof(g_x11[link_ix]));
    g_x11[link_ix].active = true;
    g_x11[link_ix].next_atom = 0x100u;
    g_x11[link_ix].next_win_id = 0x200u;
}

static void lin_x11_send_success(int link_ix, bool little) {
    uint8_t resp[128];
    memset(resp, 0, sizeof(resp));
    resp[0] = 1;
    wr16(resp + 2, 11, little);
    wr16(resp + 6, (uint16_t)((sizeof(resp) - 8) / 4), little);
    wr32(resp + 56, X11_ROOT, little);
    wr32(resp + 60, X11_CMAP, little);
    wr32(resp + 88, X11_VISUAL, little);
    wr16(resp + 76, 1024, little);
    wr16(resp + 78, 768, little);
    memcpy(resp + 40, "NexxoN", 6);
    lin_unix_link_reply(link_ix, resp, sizeof(resp));
}

static void send_reply(int link_ix, lin_x11_client_t *c, uint8_t data1,
                       uint32_t length, uint8_t reply[32]) {
    reply[0] = 1;
    wr16(reply + 2, c->sequence, c->little);
    wr32(reply + 4, length, c->little);
    lin_unix_link_reply(link_ix, reply, 32);
}

static lin_x11_window_t *window_create(lin_x11_client_t *c, uint32_t id) {
    lin_x11_window_t *w = window_find(c, id);
    if (w) return w;
    for (int i = 0; i < X11_WINDOW_MAX; i++) {
        if (!c->windows[i].used) {
            memset(&c->windows[i], 0, sizeof(c->windows[i]));
            c->windows[i].used = true;
            c->windows[i].id = id ? id : c->next_win_id++;
            return &c->windows[i];
        }
    }
    return NULL;
}

static uint32_t request_value(const uint8_t *req, uint32_t req_len,
                              uint32_t value_mask, uint32_t wanted_bit,
                              uint32_t values_off, bool little) {
    if (!(value_mask & wanted_bit)) return 0;
    uint32_t before = value_mask & (wanted_bit - 1u), index = 0;
    while (before) { index += before & 1u; before >>= 1; }
    uint32_t off = values_off + index * 4u;
    return off + 4u <= req_len ? rd32(req + off, little) : 0;
}

static void x11_process_request(int link_ix, lin_x11_client_t *c,
                                const uint8_t *req, uint32_t req_len) {
    uint8_t op = req[0], reply[32];
    memset(reply, 0, sizeof(reply));
    c->sequence++;
    switch (op) {
    case 1:
        if (req_len >= 32) {
            uint32_t wid = rd32(req + 4, c->little);
            if (!wid) wid = c->next_win_id++;
            lin_x11_window_t *w = window_create(c, wid);
            if (w) {
                w->x = (int16_t)rd16(req + 12, c->little);
                w->y = (int16_t)rd16(req + 14, c->little);
                w->width = rd16(req + 16, c->little);
                w->height = rd16(req + 18, c->little);
                w->event_mask = request_value(req, req_len,
                    rd32(req + 28, c->little), 1u << 11, 32, c->little);
            }
        }
        break;
    case 2:
        if (req_len >= 12) {
            lin_x11_window_t *w = window_find(c, rd32(req + 4, c->little));
            if (w) {
                w->event_mask = request_value(req, req_len,
                    rd32(req + 8, c->little), 1u << 11, 12, c->little);
            }
        }
        break;
    case 8: {
        lin_x11_window_t *w = req_len >= 8
            ? window_find(c, rd32(req + 4, c->little)) : NULL;
        if (w) {
            w->mapped = true;
            x11_emit_map_events(link_ix, c, w);
        }
        break;
    }
    case 14: wr32(reply + 8, X11_ROOT, c->little); send_reply(link_ix, c, 24, 0, reply); break;
    case 16: wr32(reply + 8, c->next_atom++, c->little); send_reply(link_ix, c, 0, 0, reply); break;
    case 20: case 23: case 26: case 31: case 38: case 43: case 84: case 97: case 98: case 99: case 119:
        send_reply(link_ix, c, 0, 0, reply); break;
    case 55: /* CreateGC: cid @4, drawable @8, value-mask @12, values @16 */
        if (req_len >= 16) {
            uint32_t cid = rd32(req + 4, c->little);
            lin_x11_gc_t *gc = NULL;
            for (int i = 0; i < X11_GC_MAX; i++) {
                if (c->gcs[i].used && c->gcs[i].id == cid) { gc = &c->gcs[i]; break; }
                if (!c->gcs[i].used && !gc) gc = &c->gcs[i];
            }
            if (gc) {
                gc->used = true;
                gc->id = cid;
                gc->foreground = 0xFF000000u | request_value(req, req_len,
                    rd32(req + 12, c->little), 1u << 2, 16, c->little);
            }
        }
        break;
    case 56: /* ChangeGC: gc @4, value-mask @8, values @12 */
        if (req_len >= 12) {
            uint32_t cid = rd32(req + 4, c->little);
            uint32_t mask = rd32(req + 8, c->little);
            if (mask & (1u << 2)) {
                for (int i = 0; i < X11_GC_MAX; i++) {
                    if (c->gcs[i].used && c->gcs[i].id == cid) {
                        c->gcs[i].foreground = 0xFF000000u |
                            request_value(req, req_len, mask, 1u << 2, 12,
                                          c->little);
                        break;
                    }
                }
            }
        }
        break;
    case 60: /* FreeGC */
        if (req_len >= 8) {
            uint32_t cid = rd32(req + 4, c->little);
            for (int i = 0; i < X11_GC_MAX; i++)
                if (c->gcs[i].used && c->gcs[i].id == cid)
                    c->gcs[i].used = false;
        }
        break;
    case 61: /* ClearArea: window @4, x @8, y @10, w @12, h @14 */
        if (req_len >= 16) {
            lin_x11_window_t *w = window_find(c, rd32(req + 4, c->little));
            uint16_t cw = rd16(req + 12, c->little);
            uint16_t ch = rd16(req + 14, c->little);
            if (w && !cw) cw = w->width;
            if (w && !ch) ch = w->height;
            x11_fill_rect(w ? (int16_t)(w->x + rd16(req + 8, c->little)) : (int16_t)rd16(req + 8, c->little),
                          w ? (int16_t)(w->y + rd16(req + 10, c->little)) : (int16_t)rd16(req + 10, c->little),
                          cw, ch, 0xFF1B2838u);
        }
        break;
    case 70: /* PolyFillRectangle: drawable @4, gc @8, rects @12.. */
        if (req_len >= 20) {
            lin_x11_window_t *w = window_find(c, rd32(req + 4, c->little));
            uint32_t gcid = rd32(req + 8, c->little);
            uint32_t color = 0xFF66C0F4u;
            for (int i = 0; i < X11_GC_MAX; i++)
                if (c->gcs[i].used && c->gcs[i].id == gcid)
                    color = c->gcs[i].foreground;
            for (uint32_t off = 12; off + 8 <= req_len; off += 8)
                x11_fill_rect(w ? (int16_t)(w->x + (int16_t)rd16(req + off, c->little)) : (int16_t)rd16(req + off, c->little),
                              w ? (int16_t)(w->y + (int16_t)rd16(req + off + 2, c->little)) : (int16_t)rd16(req + off + 2, c->little),
                              rd16(req + off + 4, c->little), rd16(req + off + 6, c->little), color);
        }
        break;
    case 72: /* PutImage: fmt @1, drawable @4, gc @8, w @12, h @14,
              * dst-x @16, dst-y @18, left-pad @20, depth @21, data @24 */
        if (req_len >= 24 && req[1] == 2) {
            lin_x11_window_t *w = window_find(c, rd32(req + 4, c->little));
            x11_put_zpixmap(w ? (int16_t)(w->x + (int16_t)rd16(req + 16, c->little)) : (int16_t)rd16(req + 16, c->little),
                           w ? (int16_t)(w->y + (int16_t)rd16(req + 18, c->little)) : (int16_t)rd16(req + 18, c->little),
                           rd16(req + 12, c->little), rd16(req + 14, c->little), req[21],
                           req + 24, req_len - 24);
        }
        break;
    case 73: {
        uint32_t bytes = (uint32_t)rd16(req + 12, c->little) * rd16(req + 14, c->little) * 4u;
        if (bytes > 4096u) bytes = 4096u;
        send_reply(link_ix, c, 24, (bytes + 3u) / 4u, reply);
        uint8_t zero[256];
        memset(zero, 0, sizeof(zero));
        while (bytes) {
            uint32_t n = bytes > sizeof(zero) ? sizeof(zero) : bytes;
            lin_unix_link_reply(link_ix, zero, n);
            bytes -= n;
        }
        break;
    }
    case 101: {
        uint32_t count = req_len >= 6 ? req[5] : 0;
        send_reply(link_ix, c, 1, count, reply);
        uint8_t z[256]; memset(z, 0, sizeof(z));
        while (count) { uint32_t n = count > 64 ? 64 : count; lin_unix_link_reply(link_ix, z, n * 4); count -= n; }
        break;
    }
    default: break;
    }
}

void lin_x11_on_client_write(int link_ix, const void *data, uint64_t len) {
    if (link_ix < 0 || link_ix >= X11_LINK_MAX || !data || !len) return;
    lin_x11_client_t *c = &g_x11[link_ix];
    if (!c->active) lin_x11_on_connect(link_ix);
    uint32_t n = len > X11_IN_CAP - c->in_len ? X11_IN_CAP - c->in_len : (uint32_t)len;
    memcpy(c->in + c->in_len, data, n);
    c->in_len += n;
    if (!c->setup_done) {
        if (c->in_len < 12) return;
        c->little = c->in[0] == 'l';
        uint16_t auth_name = rd16(c->in + 6, c->little);
        uint16_t auth_data = rd16(c->in + 8, c->little);
        uint32_t setup_len = 12u + ((auth_name + 3u) & ~3u) + ((auth_data + 3u) & ~3u);
        if (c->in_len < setup_len) return;
        if (c->in_len > setup_len) memmove(c->in, c->in + setup_len, c->in_len - setup_len);
        c->in_len -= setup_len;
        c->setup_done = true;
        lin_x11_send_success(link_ix, c->little);
    }
    while (c->in_len >= 4) {
        uint32_t req_len = (uint32_t)rd16(c->in + 2, c->little) * 4u;
        if (req_len < 4 || req_len > X11_IN_CAP) { c->in_len = 0; return; }
        if (c->in_len < req_len) return;
        x11_process_request(link_ix, c, c->in, req_len);
        if (c->in_len > req_len) memmove(c->in, c->in + req_len, c->in_len - req_len);
        c->in_len -= req_len;
    }
    /* One present per request batch, not per drawing op. */
    lin_x11_present();
    lin_x11_service(link_ix);
}

void lin_x11_on_disconnect(int link_ix) {
    if (link_ix < 0 || link_ix >= X11_LINK_MAX)
        return;
    memset(&g_x11[link_ix], 0, sizeof(g_x11[link_ix]));
    for (int i = 0; i < X11_LINK_MAX; i++)
        if (g_x11[i].active)
            return;
    /* Last X client gone: stop re-blitting so the desktop wins back. */
    g_x11_have_content = false;
}
