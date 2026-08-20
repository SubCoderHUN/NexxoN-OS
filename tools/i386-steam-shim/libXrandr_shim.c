/* Minimal i386 libXrandr.so.2 stub for Steam update UI */
int XRRQueryVersion(void *dpy, int *maj, int *min) {
    (void)dpy;
    if (maj) *maj = 1;
    if (min) *min = 0;
    return 1;
}
