/* i386 PIE + dynamic glibc probe matching the Steam bootstrap ELF class. */
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    void *p = malloc(8192);
    if (!p)
        return 10;
    free(p);
    static const char ok[] = "NEXXON_LINUX_I386_PIE_OK\n";
    if (write(STDOUT_FILENO, ok, sizeof(ok) - 1) !=
        (ssize_t)(sizeof(ok) - 1))
        return 11;
    return 0;
}
