/* Static-musl integration probe for the NexxoN Linux compatibility runtime.
 * A zero exit status is the machine-readable result; the language-neutral
 * marker makes the QEMU screendump equally deterministic in EN and HU. */
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static int fail(int code) {
    static const char marker[] = "NEXXON_MUSL_STATIC_V2_FAIL\n";
    (void)write(STDERR_FILENO, marker, sizeof(marker) - 1);
    return code;
}

int main(int argc, char **argv) {
    if (argc < 1 || !argv || !argv[0]) return fail(10);
    if (argc > 1 && strcmp(argv[1], "fault-test") == 0) {
        *(volatile unsigned long *)0x5000000000ULL = 1;
        return fail(99);
    }
    if (argc > 1 && strcmp(argv[1], "kernel-read-test") == 0) {
        volatile unsigned long leaked =
            *(volatile const unsigned long *)0x00100000UL;
        (void)leaked;
        return fail(98);
    }

    struct utsname uts;
    if (uname(&uts) != 0 || strcmp(uts.machine, "x86_64") != 0)
        return fail(11);

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return fail(12);

    char *heap = malloc(8192);
    if (!heap) return fail(13);
    memset(heap, 0x5A, 8192);
    if ((unsigned char)heap[4095] != 0x5A) return fail(14);

    void *map = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) return fail(15);
    memset(map, 0xA5, 8192);
    if (((unsigned char *)map)[8191] != 0xA5) return fail(16);

    int zero = open("/dev/zero", O_RDONLY);
    if (zero < 0) return fail(17);
    unsigned char z[64];
    memset(z, 1, sizeof(z));
    if (read(zero, z, sizeof(z)) != (ssize_t)sizeof(z)) return fail(18);
    if (close(zero) != 0) return fail(19);
    for (unsigned i = 0; i < sizeof(z); i++)
        if (z[i] != 0) return fail(20);

    char cwd[256];
    if (!getcwd(cwd, sizeof(cwd)) || cwd[0] != '/') return fail(21);

    struct stat st;
    if (fstat(STDOUT_FILENO, &st) != 0) return fail(22);

    /* Path-launched copies also verify NXFS open/read/stat/getdents.  The
     * embedded copy has no backing inode and intentionally skips this block. */
    if (argv[0][0] == '/') {
        int self = open("/proc/self/exe", O_RDONLY);
        if (self < 0) return fail(26);
        unsigned char magic[4];
        if (read(self, magic, sizeof(magic)) != (ssize_t)sizeof(magic) ||
            magic[0] != 0x7F || magic[1] != 'E' ||
            magic[2] != 'L' || magic[3] != 'F')
            return fail(27);
        if (fstat(self, &st) != 0 || st.st_size <= 0) return fail(28);
        if (close(self) != 0) return fail(29);

        DIR *dir = opendir("/programs");
        if (!dir) return fail(30);
        int found = 0;
        struct dirent *de;
        while ((de = readdir(dir)) != NULL)
            if (strcmp(de->d_name, "linuxdemo") == 0) found = 1;
        if (closedir(dir) != 0 || !found) return fail(31);
    }

    struct timespec pause = { .tv_sec = 0, .tv_nsec = 1000000 };
    if (nanosleep(&pause, NULL) != 0) return fail(23);

    if (munmap(map, 8192) != 0) return fail(24);
    free(heap);

    static const char marker[] = "NEXXON_MUSL_STATIC_V2_OK\n";
    if (write(STDOUT_FILENO, marker, sizeof(marker) - 1)
        != (ssize_t)(sizeof(marker) - 1))
        return fail(25);
    return 0;
}
