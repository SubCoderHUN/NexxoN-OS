/* Dynamically linked musl probe for the NexxoN PT_INTERP milestone. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char *heap = malloc(4096);
    if (!heap) return 1;
    memset(heap, 0x3C, 4096);
    if (heap[2048] != 0x3C) return 2;
    free(heap);
    const char msg[] = "NEXXON_MUSL_DYNAMIC_V4_OK\n";
    if (write(STDOUT_FILENO, msg, sizeof(msg) - 1) != (ssize_t)(sizeof(msg) - 1))
        return 3;
    return 0;
}
