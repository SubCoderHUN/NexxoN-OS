/* glibc-dynamic libX11 event probe — MapNotify + Expose via XNextEvent. */
#include <X11/Xlib.h>
#include <unistd.h>

static int fail(int step) {
    static const char msg[] = "NEXXON_LINUX_XEVENT_FAIL\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    return step;
}

int main(void) {
    Display *dpy = XOpenDisplay(":0");
    if (!dpy)
        return fail(10);
    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    Window win = XCreateSimpleWindow(
        dpy, root, 16, 16, 400, 240, 0,
        BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    if (!win)
        return fail(11);

    XSelectInput(dpy, win, ExposureMask | StructureNotifyMask);
    XMapWindow(dpy, win);

    int mapped = 0;
    int exposed = 0;
    for (int i = 0; i < 4 && (!mapped || !exposed); i++) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == MapNotify)
            mapped = 1;
        else if (ev.type == Expose)
            exposed = 1;
    }
    if (!mapped || !exposed)
        return fail(12);

    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);

    static const char ok[] = "NEXXON_LINUX_XEVENT_OK\n";
    if (write(STDOUT_FILENO, ok, sizeof(ok) - 1) !=
        (ssize_t)(sizeof(ok) - 1))
        return fail(13);
    return 0;
}
