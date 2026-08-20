/* NexxoN Wine launcher stub — validates PE/MZ headers, prints stub marker. */
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static void wstr(int fd, const char *s) {
    size_t n = strlen(s);
    if (n) (void)write(fd, s, n);
}

static int fail(const char *msg) {
    wstr(2, msg);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2)
        return fail("wine: missing program\n");

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0)
        return fail("wine: cannot open file\n");

    uint8_t mz[2];
    if (read(fd, mz, 2) != 2 || mz[0] != 'M' || mz[1] != 'Z') {
        close(fd);
        return fail("wine: not a Windows executable (missing MZ)\n");
    }

    uint32_t peoff = 0;
    if (lseek(fd, 0x3C, SEEK_SET) < 0 || read(fd, &peoff, 4) != 4) {
        close(fd);
        return fail("wine: invalid DOS header\n");
    }

    if (lseek(fd, (off_t)peoff, SEEK_SET) < 0) {
        close(fd);
        return fail("wine: cannot seek to PE header\n");
    }

    uint8_t pe[4];
    if (read(fd, pe, 4) != 4 || memcmp(pe, "PE\0\0", 4) != 0) {
        close(fd);
        return fail("wine: missing PE signature\n");
    }
    close(fd);

    static const char ok[] =
        "wine-nexxon-stub 0.2\n"
        "NEXXON_WINE_PE_STUB_OK\n"
        "PE recognized; full loader + Win32 API land next.\n";
    wstr(1, ok);
    return 0;
}
