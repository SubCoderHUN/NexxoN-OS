/* Minimal i386 libGL.so.1 + GLX for Steam update UI (software noop GL) */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef void *Display;
typedef uint32_t Window;
typedef int GLXDrawable;
typedef int GLXContext;
typedef int XVisualInfo;
typedef int GLXFBConfig;

static GLXContext g_ctx;
static Display *g_dpy;
static Window g_win;

XVisualInfo *glXChooseVisual(Display *dpy, int screen, int *attr) {
    (void)screen; (void)attr;
    static XVisualInfo vi;
    g_dpy = dpy;
    return &vi;
}

GLXContext glXCreateContext(Display *dpy, XVisualInfo *vis,
                             GLXContext share, int direct) {
    (void)vis; (void)share; (void)direct;
    g_dpy = dpy;
    g_ctx = 1;
    return g_ctx;
}

int glXDestroyContext(Display *dpy, GLXContext ctx) {
    (void)dpy; (void)ctx;
    g_ctx = 0;
    return 1;
}

int glXMakeCurrent(Display *dpy, GLXDrawable draw, GLXContext ctx) {
    g_dpy = dpy;
    g_win = (Window)draw;
    g_ctx = ctx;
    return 1;
}

void glXSwapBuffers(Display *dpy, GLXDrawable draw) {
    (void)dpy; (void)draw;
}

void (*glXGetProcAddressARB(const char *name))(void) {
    if (!name) return NULL;
    if (strcmp(name, "glXSwapIntervalEXT") == 0)
        return (void (*)(void))glXSwapBuffers;
    return NULL;
}

void *glXGetProcAddress(const char *name) {
    return (void *)glXGetProcAddressARB(name);
}

GLXFBConfig *glXChooseFBConfig(Display *dpy, int screen,
                               const int *attrib, int *nitems) {
    (void)dpy; (void)screen; (void)attrib;
    static GLXFBConfig cfg;
    if (nitems) *nitems = 1;
    return &cfg;
}

XVisualInfo *glXGetVisualFromFBConfig(Display *dpy, GLXFBConfig config) {
    (void)dpy; (void)config;
    static XVisualInfo vi;
    return &vi;
}

int glXGetFBConfigAttrib(Display *dpy, GLXFBConfig config,
                         int attribute, int *value) {
    (void)dpy; (void)config; (void)attribute;
    if (value) *value = 0;
    return 0;
}

void glClear(unsigned int mask) { (void)mask; }
void glClearColor(float r, float g, float b, float a) {
    (void)r; (void)g; (void)b; (void)a;
}
void glViewport(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
}
void glMatrixMode(unsigned int m) { (void)m; }
void glLoadIdentity(void) {}
void glOrtho(double l, double r, double b, double t, double n, double f) {
    (void)l; (void)r; (void)b; (void)t; (void)n; (void)f;
}
void glBegin(unsigned int mode) { (void)mode; }
void glEnd(void) {}
void glColor3f(float r, float g, float b) { (void)r; (void)g; (void)b; }
void glVertex2f(float x, float y) { (void)x; (void)y; }
void glRectf(float x1, float y1, float x2, float y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;
}
void glEnable(unsigned int cap) { (void)cap; }
void glDisable(unsigned int cap) { (void)cap; }
void glBlendFunc(unsigned int s, unsigned int d) { (void)s; (void)d; }
void glFinish(void) {}

int glXSwapIntervalEXT(Display *dpy, int interval) {
    (void)dpy; (void)interval;
    return 1;
}
