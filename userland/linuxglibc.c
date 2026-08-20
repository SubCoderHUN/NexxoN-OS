/* Dynamically linked glibc probe for the NexxoN /lib seed milestone. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char *heap = malloc(4096);
    if (!heap)
        return 1;
    memset(heap, 0x5A, 4096);
    if (heap[1024] != 0x5A)
        return 2;
    free(heap);

    static const char msg[] = "NEXXON_GLIBC_DYNAMIC_OK\n";
    if (write(STDOUT_FILENO, msg, sizeof(msg) - 1) != (ssize_t)(sizeof(msg) - 1))
        return 3;
    return 0;
}
