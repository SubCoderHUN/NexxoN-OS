/* musl-static ALSA PCM probe — /dev/snd/pcmC0D0p write path. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static int fail(int step) {
    static const char msg[] = "NEXXON_LINUX_ALSA_FAIL\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    return step;
}

int main(void) {
    int fd = open("/dev/snd/pcmC0D0p", O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return fail(10);

    int16_t buf[512];
    for (int i = 0; i < 512; i++)
        buf[i] = (int16_t)(i & 0xFF);
    ssize_t n = write(fd, buf, sizeof(buf));
    if (n != (ssize_t)sizeof(buf))
        return fail(11);

    if (close(fd) != 0)
        return fail(12);

    static const char ok[] = "NEXXON_LINUX_ALSA_OK\n";
    if (write(STDOUT_FILENO, ok, sizeof(ok) - 1) != (ssize_t)(sizeof(ok) - 1))
        return fail(13);
    return 0;
}
