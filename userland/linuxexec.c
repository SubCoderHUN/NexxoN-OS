/* Fixed-address ET_EXEC + MAP_FIXED integration probe for the private Linux
 * process address space.  Linked at 0x20000000, above NexxoN's low kernel
 * image, until the kernel moves to a high-half mapping. */
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
    if ((unsigned long)(void *)&main < 0x20000000UL) return 40;

    void *want = (void *)0x60000000UL;
    void *p = mmap(want, 65536, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p != want) return 41;
    ((volatile unsigned long *)p)[0] = 0x4E4558584F4E5633UL;
    ((volatile unsigned long *)p)[8191] = 0x334D4D5545584543UL;
    if (((volatile unsigned long *)p)[0] != 0x4E4558584F4E5633UL ||
        ((volatile unsigned long *)p)[8191] != 0x334D4D5545584543UL)
        return 42;

    static const char marker[] = "NEXXON_MUSL_EXEC_V3_OK\n";
    if (write(STDOUT_FILENO, marker, sizeof(marker) - 1)
        != (ssize_t)(sizeof(marker) - 1))
        return 43;
    if (munmap(p, 65536) != 0) return 44;
    return 0;
}
