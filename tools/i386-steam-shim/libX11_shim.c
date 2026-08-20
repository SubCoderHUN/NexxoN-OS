/* Minimal i386 libX11.so.6 for Steam update UI — raw X11 protocol to :0 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

typedef uint32_t Window;
typedef uint32_t Atom;
typedef uint32_t Colormap;
typedef uint32_t VisualID;
typedef uint32_t Pixmap;
typedef uint32_t Cursor;
typedef uint32_t KeySym;
typedef uint8_t KeyCode;
typedef void *GC;
typedef void *Display;
typedef void *XID;
typedef char *XPointer;
typedef int Bool;
typedef int Status;

/* Real Xlib public struct layouts — callers read these fields directly. */
typedef struct {
    void *visual;
    VisualID visualid;
    int screen;
    int depth;
    int class;
    unsigned long red_mask, green_mask, blue_mask;
    int colormap_size;
    int bits_per_rgb;
} XVisualInfo;

typedef struct {
    Pixmap background_pixmap;
    unsigned long background_pixel;
    Pixmap border_pixmap;
    unsigned long border_pixel;
    int bit_gravity;
    int win_gravity;
    int backing_store;
    unsigned long backing_planes;
    unsigned long backing_pixel;
    Bool save_under;
    long event_mask;
    long do_not_propagate_mask;
    Bool override_redirect;
    Colormap colormap;
    Cursor cursor;
} XSetWindowAttributes;

typedef struct {
    int x, y;
    int width, height;
    int border_width;
    int depth;
    void *visual;
    Window root;
    int class;
    int bit_gravity, win_gravity;
    int backing_store;
    unsigned long backing_planes;
    unsigned long backing_pixel;
    Bool save_under;
    Colormap colormap;
    Bool map_installed;
    int map_state;
    long all_event_masks;
    long your_event_mask;
    long do_not_propagate_mask;
    Bool override_redirect;
    void *screen;
} XWindowAttributes;

struct _XImage;
typedef struct _XImage {
    int width, height;
    int xoffset;
    int format;
    char *data;
    int byte_order;
    int bitmap_unit;
    int bitmap_bit_order;
    int bitmap_pad;
    int depth;
    int bytes_per_line;
    int bits_per_pixel;
    unsigned long red_mask, green_mask, blue_mask;
    XPointer obdata;
    struct funcs {
        struct _XImage *(*create_image)(void);
        int (*destroy_image)(struct _XImage *);
        unsigned long (*get_pixel)(struct _XImage *, int, int);
        int (*put_pixel)(struct _XImage *, int, int, unsigned long);
        struct _XImage *(*sub_image)(struct _XImage *, int, int,
                                     unsigned int, unsigned int);
        int (*add_pixel)(struct _XImage *, long);
    } f;
} XImage;

typedef struct {
    unsigned long pixel;
    unsigned short red, green, blue;
    char flags;
    char pad;
} XColor;

/* Steam's updateui_xwin was built against real Xlib and reaches the Display
 * internals through the standard macros (DefaultScreen → dpy->default_screen,
 * RootWindow/ScreenOfDisplay → dpy->screens[n].root, etc.).  An opaque handle
 * made it deref null+8.  So the object XOpenDisplay returns must carry the
 * REAL _XDisplay / Screen / Visual byte layout up front (offsets computed
 * from the 32-bit libX11 ABI — stable for 20+ years) with our private state
 * appended after.  Fields are poked by offset to avoid re-deriving the full
 * struct. */
#define X_DPY_SIZE     2400
#define X_SCREEN_SIZE  80
#define X_VISUAL_SIZE  32
/* _XDisplay offsets (32-bit) */
#define XD_FD              8
#define XD_PROTO_MAJOR     16
#define XD_VENDOR          24
#define XD_RESOURCE_BASE   28
#define XD_RESOURCE_MASK   32
#define XD_BYTE_ORDER      48
#define XD_LAST_REQ_READ   92
#define XD_REQUEST         96
#define XD_MAX_REQ_SIZE    116
#define XD_DISPLAY_NAME    128
#define XD_DEFAULT_SCREEN  132
#define XD_NSCREENS        136
#define XD_SCREENS         140
/* Screen offsets (32-bit) */
#define XS_DISPLAY         4
#define XS_ROOT            8
#define XS_WIDTH           12
#define XS_HEIGHT          16
#define XS_MWIDTH          20
#define XS_MHEIGHT         24
#define XS_ROOT_DEPTH      36
#define XS_ROOT_VISUAL     40
#define XS_CMAP            48
#define XS_WHITE_PIXEL     52
#define XS_BLACK_PIXEL     56
#define XS_MAX_MAPS        60
#define XS_MIN_MAPS        64
#define XS_ROOT_INPUT_MASK 76
/* Visual offsets (32-bit) */
#define XV_VISUALID        4
#define XV_CLASS           8
#define XV_RED_MASK        12
#define XV_GREEN_MASK      16
#define XV_BLUE_MASK       20
#define XV_BITS_PER_RGB    24
#define XV_MAP_ENTRIES     28

typedef struct {
    /* Real Xlib-ABI header, read by the client's Xlib macros. MUST be first. */
    uint8_t xdpy[X_DPY_SIZE];
    uint8_t xscreen[X_SCREEN_SIZE];
    uint8_t xvisual[X_VISUAL_SIZE];
    /* Private state (invisible to the client). */
    int fd;
    int screen;
    Window root;
    Window focus;
    uint16_t seq;
    uint8_t evq[32][32];
    int evn;
    uint16_t win_w, win_h;
} NexDisplay;

static void xpoke32(uint8_t *base, unsigned off, uint32_t v) {
    base[off] = (uint8_t)v; base[off + 1] = (uint8_t)(v >> 8);
    base[off + 2] = (uint8_t)(v >> 16); base[off + 3] = (uint8_t)(v >> 24);
}

/* Build the real _XDisplay/Screen/Visual view so Xlib macros resolve. */
static void nex_build_xdisplay(NexDisplay *d, unsigned w, unsigned h) {
    xpoke32(d->xdpy, XD_FD, (uint32_t)d->fd);
    xpoke32(d->xdpy, XD_PROTO_MAJOR, 11);
    xpoke32(d->xdpy, XD_RESOURCE_BASE, 0x00400000u);
    xpoke32(d->xdpy, XD_RESOURCE_MASK, 0x001FFFFFu);
    xpoke32(d->xdpy, XD_BYTE_ORDER, 0);          /* LSBFirst */
    xpoke32(d->xdpy, XD_MAX_REQ_SIZE, 65535);
    xpoke32(d->xdpy, XD_REQUEST, (uint32_t)d->seq);
    xpoke32(d->xdpy, XD_LAST_REQ_READ, (uint32_t)d->seq);
    xpoke32(d->xdpy, XD_DEFAULT_SCREEN, 0);
    xpoke32(d->xdpy, XD_NSCREENS, 1);
    xpoke32(d->xdpy, XD_SCREENS, (uint32_t)(uintptr_t)d->xscreen);
    xpoke32(d->xdpy, XD_VENDOR, (uint32_t)(uintptr_t)"NexxoN");
    xpoke32(d->xdpy, XD_DISPLAY_NAME, (uint32_t)(uintptr_t)":0");

    xpoke32(d->xscreen, XS_DISPLAY, (uint32_t)(uintptr_t)d);
    xpoke32(d->xscreen, XS_ROOT, d->root);
    xpoke32(d->xscreen, XS_WIDTH, w);
    xpoke32(d->xscreen, XS_HEIGHT, h);
    xpoke32(d->xscreen, XS_MWIDTH, (w * 254u) / 960u);   /* ~96 dpi */
    xpoke32(d->xscreen, XS_MHEIGHT, (h * 254u) / 960u);
    xpoke32(d->xscreen, XS_ROOT_DEPTH, 24);
    xpoke32(d->xscreen, XS_ROOT_VISUAL, (uint32_t)(uintptr_t)d->xvisual);
    xpoke32(d->xscreen, XS_CMAP, 33);
    xpoke32(d->xscreen, XS_WHITE_PIXEL, 0xFFFFFFu);
    xpoke32(d->xscreen, XS_BLACK_PIXEL, 0);
    xpoke32(d->xscreen, XS_MAX_MAPS, 256);
    xpoke32(d->xscreen, XS_MIN_MAPS, 256);
    xpoke32(d->xscreen, XS_ROOT_INPUT_MASK, 0);

    xpoke32(d->xvisual, XV_VISUALID, 32);
    xpoke32(d->xvisual, XV_CLASS, 4);            /* TrueColor */
    xpoke32(d->xvisual, XV_RED_MASK, 0xFF0000u);
    xpoke32(d->xvisual, XV_GREEN_MASK, 0x00FF00u);
    xpoke32(d->xvisual, XV_BLUE_MASK, 0x0000FFu);
    xpoke32(d->xvisual, XV_BITS_PER_RGB, 8);
    xpoke32(d->xvisual, XV_MAP_ENTRIES, 256);
}

static uint16_t rd16le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void wr32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static int x_send(NexDisplay *d, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) {
        ssize_t w = write(d->fd, p, n);
        if (w <= 0) return -1;
        p += (size_t)w;
        n -= (size_t)w;
    }
    d->seq++;
    return 0;
}

static int x_read(NexDisplay *d, void *buf, size_t n) {
    /* The fd is non-blocking (kernel X service replies synchronously in
     * the common case); spin briefly on EAGAIN for expected replies. */
    uint8_t *p = buf;
    int idle = 0;
    while (n) {
        ssize_t r = read(d->fd, p, n);
        if (r > 0) {
            p += (size_t)r;
            n -= (size_t)r;
            idle = 0;
            continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) {
            if (++idle > 5000) return -1;
            usleep(1000);
            continue;
        }
        return -1;
    }
    return 0;
}

Display *XOpenDisplay(const char *name) {
    /* Honour real Xlib semantics: no DISPLAY (arg or env) = no server.
     * Callers (Steam update UI) probe this to pick console vs xwin. */
    if (!name || !*name) {
        name = getenv("DISPLAY");
        if (!name || !*name)
            return NULL;
    }
    NexDisplay *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (d->fd < 0) { free(d); return NULL; }
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    const char *x11 = "/tmp/.X11-unix/X0";
    sa.sun_path[0] = 0;
    memcpy(sa.sun_path + 1, x11, strlen(x11));
    socklen_t salen = (socklen_t)(sizeof(sa.sun_family) + 1 + strlen(x11));
    if (connect(d->fd, (struct sockaddr *)&sa, salen) != 0) {
        close(d->fd); free(d); return NULL;
    }
    int fl = fcntl(d->fd, F_GETFL, 0);
    if (fl >= 0)
        (void)fcntl(d->fd, F_SETFL, fl | O_NONBLOCK);
    uint8_t req[12] = { 'l', 0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (x_send(d, req, sizeof(req)) != 0) { close(d->fd); free(d); return NULL; }
    uint8_t resp[128];
    if (x_read(d, resp, sizeof(resp)) != 0) { close(d->fd); free(d); return NULL; }
    if (resp[0] != 1) { close(d->fd); free(d); return NULL; }
    d->root = rd32le(resp + 56);
    d->screen = 0;
    d->win_w = 1024;
    d->win_h = 768;
    nex_build_xdisplay(d, 1024, 768);
    return (Display *)d;
}

int XCloseDisplay(Display *dpy) {
    NexDisplay *d = (NexDisplay *)dpy;
    if (!d) return 0;
    close(d->fd);
    free(d);
    return 0;
}

int DefaultScreen(Display *dpy) {
    NexDisplay *d = (NexDisplay *)dpy;
    return d ? d->screen : 0;
}

Window RootWindow(Display *dpy, int screen) {
    (void)screen;
    NexDisplay *d = (NexDisplay *)dpy;
    return d ? d->root : 0;
}

unsigned long BlackPixel(Display *dpy, int screen) {
    (void)dpy; (void)screen; return 0;
}
unsigned long WhitePixel(Display *dpy, int screen) {
    (void)dpy; (void)screen; return 0xFFFFFF;
}

Window XCreateSimpleWindow(Display *dpy, Window parent, int x, int y,
                           unsigned int w, unsigned int h,
                           unsigned int border, unsigned long bpx,
                           unsigned long bgpx) {
    (void)border; (void)bpx; (void)bgpx;
    NexDisplay *d = (NexDisplay *)dpy;
    if (!d) return 0;
    static uint32_t next_id = 0x400;
    Window wid = next_id++;
    uint8_t req[64];
    memset(req, 0, sizeof(req));
    req[0] = 1;
    wr16le(req + 2, 15);
    wr32le(req + 4, wid);
    wr32le(req + 8, parent ? parent : d->root);
    wr16le(req + 12, (uint16_t)x);
    wr16le(req + 14, (uint16_t)y);
    wr16le(req + 16, (uint16_t)w);
    wr16le(req + 18, (uint16_t)h);
    wr16le(req + 20, 1);
    wr32le(req + 24, 24);
    wr32le(req + 28, 0x800); /* CWEventMask */
    wr32le(req + 32, 0x28000); /* Exposure|StructureNotify */
    if (x_send(d, req, sizeof(req)) != 0) return 0;
    d->focus = wid;
    d->win_w = (uint16_t)w;
    d->win_h = (uint16_t)h;
    return wid;
}

int XMapWindow(Display *dpy, Window w) {
    NexDisplay *d = (NexDisplay *)dpy;
    if (!d) return 0;
    uint8_t req[8] = { 8, 0, 2, 0 };
    wr32le(req + 4, w);
    return x_send(d, req, sizeof(req)) == 0 ? 1 : 0;
}

int XSelectInput(Display *dpy, Window w, long mask) {
    NexDisplay *d = (NexDisplay *)dpy;
    if (!d) return 0;
    uint8_t req[16] = { 2, 0, 3, 0 };
    wr32le(req + 4, w);
    wr32le(req + 8, 0x800);
    wr32le(req + 12, (uint32_t)mask);
    return x_send(d, req, sizeof(req)) == 0 ? 1 : 0;
}

int XPending(Display *dpy) {
    NexDisplay *d = (NexDisplay *)dpy;
    if (!d) return 0;
    if (d->evn > 0) return d->evn;
    uint8_t hdr[1];
    ssize_t r = read(d->fd, hdr, 1);
    if (r == 1) {
        uint8_t rest[31];
        if (read(d->fd, rest, sizeof(rest)) == (ssize_t)sizeof(rest)) {
            d->evq[d->evn][0] = hdr[0];
            memcpy(d->evq[d->evn] + 1, rest, sizeof(rest));
            d->evn++;
            return d->evn;
        }
    }
    return 0;
}

int XNextEvent(Display *dpy, void *ev_out) {
    NexDisplay *d = (NexDisplay *)dpy;
    if (!d || !ev_out) return 0;
    while (!d->evn) {
        if (XPending(dpy) <= 0)
            usleep(1000);
    }
    memcpy(ev_out, d->evq[0], 32);
    memmove(d->evq, d->evq + 1, sizeof(d->evq[0]) * (size_t)(d->evn - 1));
    d->evn--;
    return 1;
}

int XSync(Display *dpy, int discard) {
    (void)discard;
    NexDisplay *d = (NexDisplay *)dpy;
    if (!d) return 0;
    while (XPending(dpy) > 0) {
        uint8_t junk[32];
        XNextEvent(dpy, junk);
    }
    return 1;
}

int XFlush(Display *dpy) {
    (void)dpy;
    return 1;
}

int XDestroyWindow(Display *dpy, Window w) {
    (void)dpy; (void)w;
    return 1;
}

Atom XInternAtom(Display *dpy, const char *name, int only_if_exists) {
    (void)only_if_exists;
    NexDisplay *d = (NexDisplay *)dpy;
    if (!d || !name) return 0;
    uint8_t req[24] = { 16, 0, 4, 0 };
    size_t nlen = strlen(name);
    if (nlen > 16) nlen = 16;
    memcpy(req + 4, name, nlen);
    req[20] = 0;
    if (x_send(d, req, sizeof(req)) != 0) return 0;
    uint8_t reply[32];
    if (x_read(d, reply, sizeof(reply)) != 0) return 0;
    return rd32le(reply + 8);
}

GC XCreateGC(Display *dpy, Window w, unsigned long mask, void *vals) {
    (void)mask; (void)vals;
    NexDisplay *d = (NexDisplay *)dpy;
    static int gc = 1;
    int id = gc++;
    if (d) {
        uint8_t req[16] = { 55, 0, 4, 0 };
        wr32le(req + 4, (uint32_t)id);
        wr32le(req + 8, w);
        wr32le(req + 12, 0);
        (void)x_send(d, req, sizeof(req));
    }
    return (GC)(intptr_t)id;
}

int XFreeGC(Display *dpy, GC gc) {
    NexDisplay *d = (NexDisplay *)dpy;
    if (d) {
        uint8_t req[8] = { 60, 0, 2, 0 };
        wr32le(req + 4, (uint32_t)(intptr_t)gc);
        (void)x_send(d, req, sizeof(req));
    }
    return 1;
}

int XSetForeground(Display *dpy, GC gc, unsigned long px) {
    NexDisplay *d = (NexDisplay *)dpy;
    if (!d) return 0;
    /* ChangeGC: gc @4, value-mask @8 (GCForeground), value @12. */
    uint8_t req[16] = { 56, 0, 4, 0 };
    wr32le(req + 4, (uint32_t)(intptr_t)gc);
    wr32le(req + 8, 1u << 2);
    wr32le(req + 12, (uint32_t)px);
    return x_send(d, req, sizeof(req)) == 0 ? 1 : 0;
}

int XFillRectangle(Display *dpy, Window w, GC gc, int x, int y,
                   unsigned int width, unsigned int height) {
    NexDisplay *d = (NexDisplay *)dpy;
    if (!d) return 0;
    /* Standard PolyFillRectangle: drawable @4, gc @8, one rect @12. */
    uint8_t req[20] = { 70, 0, 5, 0 };
    wr32le(req + 4, w);
    wr32le(req + 8, (uint32_t)(intptr_t)gc);
    wr16le(req + 12, (uint16_t)x);
    wr16le(req + 14, (uint16_t)y);
    wr16le(req + 16, (uint16_t)width);
    wr16le(req + 18, (uint16_t)height);
    return x_send(d, req, sizeof(req)) == 0 ? 1 : 0;
}

int XDrawString(Display *dpy, Window w, GC gc, int x, int y,
                const char *s, int len) {
    (void)dpy; (void)w; (void)gc; (void)x; (void)y; (void)s; (void)len;
    return 1;
}

int XStoreName(Display *dpy, Window w, const char *name) {
    (void)dpy; (void)w; (void)name;
    return 1;
}

int XRaiseWindow(Display *dpy, Window w) {
    (void)dpy; (void)w;
    return 1;
}

int XMoveWindow(Display *dpy, Window w, int x, int y) {
    (void)dpy; (void)w; (void)x; (void)y;
    return 1;
}

int XResizeWindow(Display *dpy, Window w, unsigned int width,
                  unsigned int height) {
    (void)dpy; (void)w; (void)width; (void)height;
    return 1;
}

int XClearWindow(Display *dpy, Window w) {
    (void)dpy; (void)w;
    return 1;
}

int XChangeProperty(Display *dpy, Window w, Atom prop, Atom type,
                    int fmt, int mode, const unsigned char *data, int nelem) {
    (void)dpy; (void)w; (void)prop; (void)type; (void)fmt;
    (void)mode; (void)data; (void)nelem;
    return 1;
}

int XSetWMProtocols(Display *dpy, Window w, Atom *protos, int count) {
    (void)dpy; (void)w; (void)protos; (void)count;
    return 1;
}

int XIfEvent(Display *dpy, void *ev, int (*pred)(Display *, void *, void *),
             void *arg) {
    (void)pred; (void)arg;
    return XNextEvent(dpy, ev);
}

int XCheckMaskEvent(Display *dpy, long mask, void *ev) {
    (void)mask;
    if (XPending(dpy) <= 0) return 0;
    return XNextEvent(dpy, ev);
}

int XEventsQueued(Display *dpy, int mode) {
    (void)mode;
    return XPending(dpy);
}

Colormap DefaultColormap(Display *dpy, int screen) {
    (void)dpy; (void)screen;
    return 33;
}

VisualID XVisualIDFromVisual(void *v) {
    return v ? rd32le((const uint8_t *)v + XV_VISUALID) : 32;
}

void *DefaultVisual(Display *dpy, int screen) {
    (void)screen;
    NexDisplay *d = (NexDisplay *)dpy;
    return d ? d->xvisual : NULL;
}

void *DefaultRootWindowPtr(Display *dpy) {
    return dpy;
}

int DefaultDepth(Display *dpy, int screen) {
    (void)dpy; (void)screen;
    return 24;
}

void *DefaultScreenOfDisplay(Display *dpy) {
    NexDisplay *d = (NexDisplay *)dpy;
    return d ? d->xscreen : NULL;
}

void *ScreenOfDisplay(Display *dpy, int screen) {
    (void)screen;
    NexDisplay *d = (NexDisplay *)dpy;
    return d ? d->xscreen : NULL;
}

void *DefaultGC(Display *dpy, int screen) {
    (void)dpy; (void)screen;
    static int gc = 7;
    return &gc;
}

int DisplayWidth(Display *dpy, int screen) {
    (void)dpy; (void)screen;
    return 1024;
}
int DisplayHeight(Display *dpy, int screen) {
    (void)dpy; (void)screen;
    return 768;
}

int XLookupString(void *ev, char *buf, int len, void *ks, void *comp) {
    (void)ev; (void)ks; (void)comp;
    if (buf && len > 0) buf[0] = 0;
    return 0;
}

int XkbQueryExtension(Display *dpy, int *op, int *ev, int *err,
                      int *maj, int *min) {
    (void)dpy;
    if (op) *op = 0;
    if (ev) *ev = 0;
    if (err) *err = 0;
    if (maj) *maj = 0;
    if (min) *min = 0;
    return 0;
}

int _XFlush(Display *dpy) { return XFlush(dpy); }
int _XRead(Display *dpy, char *data, long size) {
    NexDisplay *d = (NexDisplay *)dpy;
    return d && x_read(d, data, (size_t)size) == 0 ? 1 : 0;
}

/* ---- Extended surface for the Steam update UI (updateui_xwin) ---- */

static void *g_default_visual_ptr(void) {
    static int vis;
    return &vis;
}

Status XMatchVisualInfo(Display *dpy, int screen, int depth, int class,
                        XVisualInfo *out) {
    (void)dpy;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    out->visual = g_default_visual_ptr();
    out->visualid = 32;
    out->screen = screen;
    out->depth = depth ? depth : 24;
    out->class = class;
    out->red_mask = 0xFF0000u;
    out->green_mask = 0x00FF00u;
    out->blue_mask = 0x0000FFu;
    out->colormap_size = 256;
    out->bits_per_rgb = 8;
    return 1;
}

XVisualInfo *XGetVisualInfo(Display *dpy, long mask, XVisualInfo *tmpl,
                            int *nitems) {
    (void)mask;
    XVisualInfo *vi = calloc(1, sizeof(*vi));
    if (!vi) { if (nitems) *nitems = 0; return NULL; }
    XMatchVisualInfo(dpy, tmpl ? tmpl->screen : 0,
                     tmpl && tmpl->depth ? tmpl->depth : 24,
                     tmpl ? tmpl->class : 4, vi);
    if (nitems) *nitems = 1;
    return vi;
}

int XFree(void *p) {
    free(p);
    return 1;
}

Colormap XCreateColormap(Display *dpy, Window w, void *visual, int alloc) {
    (void)dpy; (void)w; (void)visual; (void)alloc;
    return 33;
}

int XFreeColormap(Display *dpy, Colormap c) {
    (void)dpy; (void)c;
    return 1;
}

Window XCreateWindow(Display *dpy, Window parent, int x, int y,
                     unsigned int width, unsigned int height,
                     unsigned int border, int depth, unsigned int class,
                     void *visual, unsigned long valuemask,
                     XSetWindowAttributes *attrs) {
    (void)depth; (void)class; (void)visual;
    Window wid = XCreateSimpleWindow(dpy, parent, x, y, width, height,
                                     border, 0, 0);
    if (wid && attrs && (valuemask & (1UL << 11)))
        XSelectInput(dpy, wid, attrs->event_mask);
    return wid;
}

Status XGetWindowAttributes(Display *dpy, Window w, XWindowAttributes *out) {
    NexDisplay *d = (NexDisplay *)dpy;
    if (!d || !out) return 0;
    (void)w;
    memset(out, 0, sizeof(*out));
    out->width = d->win_w ? d->win_w : 640;
    out->height = d->win_h ? d->win_h : 480;
    out->depth = 24;
    out->visual = g_default_visual_ptr();
    out->root = d->root;
    out->map_state = 2; /* IsViewable */
    out->screen = NULL;
    return 1;
}

Status XGetGeometry(Display *dpy, XID drawable, Window *root, int *x, int *y,
                    unsigned int *width, unsigned int *height,
                    unsigned int *border, unsigned int *depth) {
    NexDisplay *d = (NexDisplay *)dpy;
    (void)drawable;
    if (!d) return 0;
    if (root) *root = d->root;
    if (x) *x = 0;
    if (y) *y = 0;
    if (width) *width = d->win_w ? d->win_w : 640;
    if (height) *height = d->win_h ? d->win_h : 480;
    if (border) *border = 0;
    if (depth) *depth = 24;
    return 1;
}

static int nex_img_destroy(XImage *img) {
    if (!img) return 0;
    free(img->data);
    free(img);
    return 1;
}

static unsigned long nex_img_get_pixel(XImage *img, int x, int y) {
    if (!img || !img->data || x < 0 || y < 0 ||
        x >= img->width || y >= img->height)
        return 0;
    return rd32le((const uint8_t *)img->data +
                  (size_t)y * (size_t)img->bytes_per_line + (size_t)x * 4u);
}

static int nex_img_put_pixel(XImage *img, int x, int y, unsigned long px) {
    if (!img || !img->data || x < 0 || y < 0 ||
        x >= img->width || y >= img->height)
        return 0;
    wr32le((uint8_t *)img->data +
           (size_t)y * (size_t)img->bytes_per_line + (size_t)x * 4u,
           (uint32_t)px);
    return 1;
}

XImage *XCreateImage(Display *dpy, void *visual, unsigned int depth,
                     int format, int offset, char *data,
                     unsigned int width, unsigned int height,
                     int bitmap_pad, int bytes_per_line) {
    (void)dpy; (void)visual; (void)offset;
    XImage *img = calloc(1, sizeof(*img));
    if (!img) return NULL;
    img->width = (int)width;
    img->height = (int)height;
    img->format = format;
    img->data = data;
    img->byte_order = 0;
    img->bitmap_unit = 32;
    img->bitmap_bit_order = 0;
    img->bitmap_pad = bitmap_pad ? bitmap_pad : 32;
    img->depth = (int)depth;
    img->bytes_per_line = bytes_per_line ? bytes_per_line
                                         : (int)width * 4;
    img->bits_per_pixel = 32;
    img->red_mask = 0xFF0000u;
    img->green_mask = 0x00FF00u;
    img->blue_mask = 0x0000FFu;
    img->f.destroy_image = nex_img_destroy;
    img->f.get_pixel = nex_img_get_pixel;
    img->f.put_pixel = nex_img_put_pixel;
    return img;
}

/* PutImage in row chunks so a full-window frame stays inside the kernel
 * X service's per-request cap (32 KiB). */
int XPutImage(Display *dpy, Window w, GC gc, XImage *img,
              int src_x, int src_y, int dst_x, int dst_y,
              unsigned int width, unsigned int height) {
    (void)gc;
    NexDisplay *d = (NexDisplay *)dpy;
    if (!d || !img || !img->data) return 0;
    if (src_x < 0 || src_y < 0) return 0;
    if ((int)width > img->width - src_x)
        width = (unsigned int)(img->width - src_x);
    uint32_t row_bytes = width ? width * 4u : 4u;
    unsigned int rows_per_req = 24576u / row_bytes;
    if (rows_per_req == 0) rows_per_req = 1;
    unsigned int row = 0;
    while (row < height) {
        unsigned int n = height - row;
        if (n > rows_per_req) n = rows_per_req;
        uint32_t data_len = width * 4u * n;
        uint32_t req_len = 24u + ((data_len + 3u) & ~3u);
        uint8_t *req = malloc(req_len);
        if (!req) return 0;
        memset(req, 0, req_len);
        req[0] = 72;               /* PutImage */
        req[1] = 2;                /* ZPixmap */
        wr16le(req + 2, (uint16_t)(req_len / 4u));
        wr32le(req + 4, w);
        wr32le(req + 8, 0);        /* gc */
        wr16le(req + 12, (uint16_t)width);
        wr16le(req + 14, (uint16_t)n);
        wr16le(req + 16, (uint16_t)dst_x);
        wr16le(req + 18, (uint16_t)(dst_y + (int)row));
        req[20] = 0;               /* left-pad */
        req[21] = 24;              /* depth */
        for (unsigned int r = 0; r < n; r++)
            memcpy(req + 24 + (size_t)r * width * 4u,
                   img->data +
                   (size_t)(src_y + (int)(row + r)) *
                       (size_t)img->bytes_per_line +
                   (size_t)src_x * 4u,
                   (size_t)width * 4u);
        int rc = x_send(d, req, req_len);
        free(req);
        if (rc != 0) return 0;
        row += n;
    }
    return 1;
}

Status XAllocColor(Display *dpy, Colormap cmap, XColor *c) {
    (void)dpy; (void)cmap;
    if (!c) return 0;
    c->pixel = ((unsigned long)(c->red >> 8) << 16) |
               ((unsigned long)(c->green >> 8) << 8) |
               (unsigned long)(c->blue >> 8);
    return 1;
}

Status XAllocNamedColor(Display *dpy, Colormap cmap, const char *name,
                        XColor *screen_def, XColor *exact_def) {
    (void)dpy; (void)cmap; (void)name;
    if (screen_def) memset(screen_def, 0, sizeof(*screen_def));
    if (exact_def) memset(exact_def, 0, sizeof(*exact_def));
    return 1;
}

int XStoreColors(Display *dpy, Colormap cmap, XColor *colors, int n) {
    (void)dpy; (void)cmap; (void)colors; (void)n;
    return 1;
}

Pixmap XCreatePixmap(Display *dpy, XID drawable, unsigned int w,
                     unsigned int h, unsigned int depth) {
    (void)dpy; (void)drawable; (void)w; (void)h; (void)depth;
    static uint32_t next_pixmap = 0x1000;
    return next_pixmap++;
}

int XFreePixmap(Display *dpy, Pixmap p) {
    (void)dpy; (void)p;
    return 1;
}

int XCopyArea(Display *dpy, XID src, XID dst, GC gc, int sx, int sy,
              unsigned int w, unsigned int h, int dx, int dy) {
    (void)dpy; (void)src; (void)dst; (void)gc;
    (void)sx; (void)sy; (void)w; (void)h; (void)dx; (void)dy;
    return 1;
}

int XUnmapWindow(Display *dpy, Window w) {
    NexDisplay *d = (NexDisplay *)dpy;
    if (!d) return 0;
    uint8_t req[8] = { 10, 0, 2, 0 };
    wr32le(req + 4, w);
    return x_send(d, req, sizeof(req)) == 0 ? 1 : 0;
}

int XMoveResizeWindow(Display *dpy, Window w, int x, int y,
                      unsigned int width, unsigned int height) {
    NexDisplay *d = (NexDisplay *)dpy;
    (void)w; (void)x; (void)y;
    if (d) {
        d->win_w = (uint16_t)width;
        d->win_h = (uint16_t)height;
    }
    return 1;
}

int XClearArea(Display *dpy, Window w, int x, int y, unsigned int width,
               unsigned int height, Bool exposures) {
    (void)dpy; (void)w; (void)x; (void)y;
    (void)width; (void)height; (void)exposures;
    return 1;
}

int XSetWMNormalHints(Display *dpy, Window w, void *hints) {
    (void)dpy; (void)w; (void)hints;
    return 1;
}

int XSetWMHints(Display *dpy, Window w, void *hints) {
    (void)dpy; (void)w; (void)hints;
    return 1;
}

int XSetClassHint(Display *dpy, Window w, void *hint) {
    (void)dpy; (void)w; (void)hint;
    return 1;
}

int XSetTransientForHint(Display *dpy, Window w, Window parent) {
    (void)dpy; (void)w; (void)parent;
    return 1;
}

int XSetNormalHints(Display *dpy, Window w, void *hints) {
    (void)dpy; (void)w; (void)hints;
    return 1;
}

Cursor XCreateFontCursor(Display *dpy, unsigned int shape) {
    (void)dpy; (void)shape;
    return 0x2000;
}

int XDefineCursor(Display *dpy, Window w, Cursor c) {
    (void)dpy; (void)w; (void)c;
    return 1;
}

int XUndefineCursor(Display *dpy, Window w) {
    (void)dpy; (void)w;
    return 1;
}

int XFreeCursor(Display *dpy, Cursor c) {
    (void)dpy; (void)c;
    return 1;
}

KeyCode XKeysymToKeycode(Display *dpy, KeySym ks) {
    (void)dpy;
    if (ks >= 'a' && ks <= 'z') return (KeyCode)(ks - 'a' + 38);
    if (ks >= '0' && ks <= '9') return (KeyCode)(ks - '0' + 10);
    return 0;
}

KeySym XKeycodeToKeysym(Display *dpy, KeyCode kc, int index) {
    (void)dpy; (void)index;
    if (kc >= 38 && kc < 64) return (KeySym)('a' + kc - 38);
    if (kc >= 10 && kc < 20) return (KeySym)('0' + kc - 10);
    if (kc == 65) return (KeySym)' ';
    if (kc == 36) return 0xFF0D; /* XK_Return */
    if (kc == 9)  return 0xFF1B; /* XK_Escape */
    return 0;
}

KeySym XLookupKeysym(void *key_event, int index) {
    (void)index;
    if (!key_event) return 0;
    const uint8_t *ev = (const uint8_t *)key_event;
    /* Wire event detail byte (offset 1) holds the keycode. */
    return XKeycodeToKeysym(NULL, ev[1], 0);
}

int XConnectionNumber(Display *dpy) {
    NexDisplay *d = (NexDisplay *)dpy;
    return d ? d->fd : -1;
}

int XTranslateCoordinates(Display *dpy, Window src, Window dst,
                          int sx, int sy, int *dx, int *dy, Window *child) {
    (void)dpy; (void)src; (void)dst;
    if (dx) *dx = sx;
    if (dy) *dy = sy;
    if (child) *child = 0;
    return 1;
}

Status XSendEvent(Display *dpy, Window w, Bool propagate, long mask,
                  void *event) {
    (void)dpy; (void)w; (void)propagate; (void)mask; (void)event;
    return 1;
}

typedef int (*XErrorHandlerFn)(Display *, void *);
static XErrorHandlerFn g_err_handler;
static XErrorHandlerFn g_io_err_handler;

XErrorHandlerFn XSetErrorHandler(XErrorHandlerFn fn) {
    XErrorHandlerFn old = g_err_handler;
    g_err_handler = fn;
    return old;
}

XErrorHandlerFn XSetIOErrorHandler(XErrorHandlerFn fn) {
    XErrorHandlerFn old = g_io_err_handler;
    g_io_err_handler = fn;
    return old;
}

int XGetErrorText(Display *dpy, int code, char *out, int cap) {
    (void)dpy; (void)code;
    if (out && cap > 0) out[0] = 0;
    return 0;
}

char *XDisplayName(const char *name) {
    static char buf[64];
    const char *v = (name && *name) ? name : getenv("DISPLAY");
    if (!v) v = "";
    strncpy(buf, v, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    return buf;
}

char *XServerVendor(Display *dpy) {
    (void)dpy;
    static char vendor[] = "NexxoN";
    return vendor;
}

int XBell(Display *dpy, int percent) {
    (void)dpy; (void)percent;
    return 1;
}

int XGrabServer(Display *dpy) { (void)dpy; return 1; }
int XUngrabServer(Display *dpy) { (void)dpy; return 1; }

int XCheckTypedEvent(Display *dpy, int type, void *ev_out) {
    NexDisplay *d = (NexDisplay *)dpy;
    if (!d || !ev_out) return 0;
    if (XPending(dpy) <= 0) return 0;
    for (int i = 0; i < d->evn; i++) {
        if ((d->evq[i][0] & 0x7F) == type) {
            memcpy(ev_out, d->evq[i], 32);
            memmove(d->evq + i, d->evq + i + 1,
                    sizeof(d->evq[0]) * (size_t)(d->evn - i - 1));
            d->evn--;
            return 1;
        }
    }
    return 0;
}

int XCheckWindowEvent(Display *dpy, Window w, long mask, void *ev_out) {
    (void)w; (void)mask;
    if (XPending(dpy) <= 0) return 0;
    return XNextEvent(dpy, ev_out);
}

int XCheckTypedWindowEvent(Display *dpy, Window w, int type, void *ev_out) {
    (void)w;
    return XCheckTypedEvent(dpy, type, ev_out);
}

/* ---- Full import surface of the official 1.0.0.x bootstrap ---- */

typedef uint32_t Font;

typedef struct {
    short lbearing, rbearing, width, ascent, descent;
    unsigned short attributes;
} XCharStruct;

typedef struct {
    void *ext_data;
    Font fid;
    unsigned direction;
    unsigned min_char_or_byte2, max_char_or_byte2;
    unsigned min_byte1, max_byte1;
    Bool all_chars_exist;
    unsigned default_char;
    int n_properties;
    void *properties;
    XCharStruct min_bounds, max_bounds;
    XCharStruct *per_char;
    int ascent, descent;
} XFontStruct;

typedef struct {
    unsigned char *value;
    Atom encoding;
    int format;
    unsigned long nitems;
} XTextProperty;

#define NEX_FONT_W 8
#define NEX_FONT_ASCENT 12
#define NEX_FONT_DESCENT 4

Status XInitThreads(void) {
    return 1;
}

int XGetErrorDatabaseText(Display *dpy, const char *name, const char *msg,
                          const char *def, char *out, int cap) {
    (void)dpy; (void)name; (void)msg;
    if (!out || cap <= 0) return 0;
    const char *v = def ? def : "";
    strncpy(out, v, (size_t)cap - 1);
    out[cap - 1] = 0;
    return 0;
}

Bool XFilterEvent(void *ev, Window w) {
    (void)ev; (void)w;
    return 0;
}

int XPeekEvent(Display *dpy, void *ev_out) {
    NexDisplay *d = (NexDisplay *)dpy;
    if (!d || !ev_out) return 0;
    while (!d->evn) {
        if (XPending(dpy) <= 0)
            usleep(1000);
    }
    memcpy(ev_out, d->evq[0], 32);
    return 1;
}

Bool XQueryExtension(Display *dpy, const char *name, int *major_op,
                     int *first_event, int *first_error) {
    (void)dpy; (void)name;
    if (major_op) *major_op = 0;
    if (first_event) *first_event = 0;
    if (first_error) *first_error = 0;
    return 0;
}

char **XListExtensions(Display *dpy, int *count) {
    (void)dpy;
    if (count) *count = 0;
    return calloc(1, sizeof(char *));
}

int XFreeExtensionList(char **list) {
    free(list);
    return 1;
}

char **XListFonts(Display *dpy, const char *pattern, int max, int *count) {
    (void)dpy; (void)pattern; (void)max;
    static char fixed_name[] = "fixed";
    char **list = calloc(2, sizeof(char *));
    if (!list) { if (count) *count = 0; return NULL; }
    list[0] = fixed_name;
    if (count) *count = 1;
    return list;
}

int XFreeFontNames(char **list) {
    free(list);
    return 1;
}

Font XLoadFont(Display *dpy, const char *name) {
    (void)dpy; (void)name;
    static uint32_t next_font = 0x3000;
    return next_font++;
}

int XUnloadFont(Display *dpy, Font f) {
    (void)dpy; (void)f;
    return 1;
}

XFontStruct *XQueryFont(Display *dpy, XID fid) {
    (void)dpy;
    XFontStruct *fs = calloc(1, sizeof(*fs));
    if (!fs) return NULL;
    fs->fid = (Font)(uintptr_t)fid;
    fs->min_char_or_byte2 = 32;
    fs->max_char_or_byte2 = 255;
    fs->all_chars_exist = 1;
    fs->default_char = ' ';
    fs->min_bounds.width = NEX_FONT_W;
    fs->min_bounds.ascent = NEX_FONT_ASCENT;
    fs->min_bounds.descent = NEX_FONT_DESCENT;
    fs->min_bounds.rbearing = NEX_FONT_W;
    fs->max_bounds = fs->min_bounds;
    fs->per_char = NULL; /* NULL = every char uses max_bounds (Xlib rule) */
    fs->ascent = NEX_FONT_ASCENT;
    fs->descent = NEX_FONT_DESCENT;
    return fs;
}

int XFreeFontInfo(char **names, XFontStruct *info, int count) {
    (void)count;
    free(names);
    free(info);
    return 1;
}

typedef struct {
    unsigned char byte1, byte2;
} XChar2b;

int XTextWidth16(XFontStruct *fs, const XChar2b *s, int len) {
    (void)fs; (void)s;
    return len * NEX_FONT_W;
}

/* ImageText16 (op 77): byte1 = nchars, drawable @4, gc @8, x @12, y @14,
 * XChar2b string @16. */
int XDrawString16(Display *dpy, XID drawable, GC gc, int x, int y,
                  const XChar2b *s, int len) {
    NexDisplay *d = (NexDisplay *)dpy;
    if (!d || !s || len <= 0) return 0;
    if (len > 255) len = 255;
    uint32_t req_len = 16u + (((uint32_t)len * 2u + 3u) & ~3u);
    uint8_t *req = calloc(1, req_len);
    if (!req) return 0;
    req[0] = 77;
    req[1] = (uint8_t)len;
    wr16le(req + 2, (uint16_t)(req_len / 4u));
    wr32le(req + 4, (uint32_t)(uintptr_t)drawable);
    wr32le(req + 8, (uint32_t)(intptr_t)gc);
    wr16le(req + 12, (uint16_t)x);
    wr16le(req + 14, (uint16_t)y);
    memcpy(req + 16, s, (size_t)len * 2u);
    int rc = x_send(d, req, req_len);
    free(req);
    return rc == 0 ? 1 : 0;
}

int XDrawRectangle(Display *dpy, XID drawable, GC gc, int x, int y,
                   unsigned int w, unsigned int h) {
    /* Outline as four 1-px fills (the kernel service has no PolyRectangle). */
    XFillRectangle(dpy, (Window)(uintptr_t)drawable, gc, x, y, w, 1);
    XFillRectangle(dpy, (Window)(uintptr_t)drawable, gc, x, y + (int)h - 1, w, 1);
    XFillRectangle(dpy, (Window)(uintptr_t)drawable, gc, x, y, 1, h);
    XFillRectangle(dpy, (Window)(uintptr_t)drawable, gc, x + (int)w - 1, y, 1, h);
    return 1;
}

int XSetInputFocus(Display *dpy, Window w, int revert_to, uint32_t time) {
    (void)dpy; (void)w; (void)revert_to; (void)time;
    return 1;
}

void XSetWMName(Display *dpy, Window w, XTextProperty *prop) {
    (void)dpy; (void)w; (void)prop;
}

void XSetWMSizeHints(Display *dpy, Window w, void *hints, Atom prop) {
    (void)dpy; (void)w; (void)hints; (void)prop;
}

void *XAllocSizeHints(void) {
    return calloc(1, 128);
}

int Xutf8TextListToTextProperty(Display *dpy, char **list, int count,
                                int style, XTextProperty *out) {
    (void)dpy; (void)style;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (count > 0 && list && list[0]) {
        size_t n = strlen(list[0]);
        out->value = malloc(n + 1);
        if (out->value) {
            memcpy(out->value, list[0], n + 1);
            out->nitems = n;
        }
    }
    out->encoding = 31; /* XA_STRING */
    out->format = 8;
    return 0;
}
