/* musl-static PulseAudio native socket probe. */
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define PA_NATIVE_COOKIE ((uint32_t)(('P' << 24) | ('A' << 16) | ('S' << 8) | 'O'))

static int fail(int step) {
    static const char msg[] = "NEXXON_LINUX_PULSE_FAIL\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    return step;
}

int main(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return fail(10);

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    sa.sun_path[0] = '\0';
    const char *pulse = "/run/user/0/pulse/native";
    memcpy(sa.sun_path + 1, pulse, strlen(pulse));
    socklen_t salen = (socklen_t)(sizeof(sa.sun_family) + 1 + strlen(pulse));
    if (connect(fd, (struct sockaddr *)&sa, salen) != 0)
        return fail(11);

    uint32_t cookie = 0;
    uint32_t version = 0;
    if (read(fd, &cookie, sizeof(cookie)) != (ssize_t)sizeof(cookie))
        return fail(12);
    if (read(fd, &version, sizeof(version)) != (ssize_t)sizeof(version))
        return fail(13);
    if (cookie != PA_NATIVE_COOKIE || version < 8)
        return fail(14);

    if (close(fd) != 0)
        return fail(15);

    static const char ok[] = "NEXXON_LINUX_PULSE_OK\n";
    if (write(STDOUT_FILENO, ok, sizeof(ok) - 1) != (ssize_t)(sizeof(ok) - 1))
        return fail(16);
    return 0;
}
