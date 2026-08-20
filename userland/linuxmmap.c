/* musl-static large mmap probe — demand-paged GB-scale arena. */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PROBE_BYTES (256u * 1024u * 1024u)

static int fail(int step) {
    static const char msg[] = "NEXXON_LINUX_MMAP_FAIL\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    return step;
}

int main(void) {
    void *map = mmap(NULL, PROBE_BYTES, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED)
        return fail(10);

    volatile unsigned char *base = (volatile unsigned char *)map;
    base[0] = 0x5A;
    base[PROBE_BYTES - 4096] = 0xA5;

    if (base[0] != 0x5A || base[PROBE_BYTES - 4096] != 0xA5)
        return fail(11);

    if (munmap(map, PROBE_BYTES) != 0)
        return fail(12);

    static const char ok[] = "NEXXON_LINUX_MMAP_OK\n";
    if (write(STDOUT_FILENO, ok, sizeof(ok) - 1) != (ssize_t)(sizeof(ok) - 1))
        return fail(13);
    return 0;
}
