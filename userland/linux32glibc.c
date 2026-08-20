/* i386 dynamic glibc probe for Steam bootstrap runtime readiness. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char *p = malloc(4096);
    if (!p)
        return 10;
    memset(p, 0x32, 4096);
    if (p[2048] != 0x32)
        return 11;
    free(p);
    static const char ok[] = "NEXXON_LINUX_I386_GLIBC_OK\n";
    if (write(STDOUT_FILENO, ok, sizeof(ok) - 1) !=
        (ssize_t)(sizeof(ok) - 1))
        return 12;
    return 0;
}
