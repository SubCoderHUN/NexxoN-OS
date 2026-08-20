/* glibc-dynamic libX11 probe — open :0 and exercise a window round-trip. */
#include <X11/Xlib.h>
#include <unistd.h>

static int fail(int step) {
    static const char msg[] = "NEXXON_LINUX_XLIB_FAIL\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    return step;
}

int main(void) {
    Display *dpy = XOpenDisplay(":0");
    if (!dpy)
        return fail(10);

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    if (!root)
        return fail(11);

    Window win = XCreateSimpleWindow(
        dpy, root, 8, 8, 320, 180, 0,
        BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    if (!win)
        return fail(12);

    XMapWindow(dpy, win);
    XSync(dpy, False);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);

    static const char ok[] = "NEXXON_LINUX_XLIB_OK\n";
    if (write(STDOUT_FILENO, ok, sizeof(ok) - 1) !=
        (ssize_t)(sizeof(ok) - 1))
        return fail(13);
    return 0;
}
