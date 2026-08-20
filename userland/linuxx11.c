/* musl-static X11 display :0 probe — AF_UNIX abstract socket + setup. */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int fail(int step) {
    static const char msg[] = "NEXXON_LINUX_X11_FAIL\n";
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
    const char *x11 = "/tmp/.X11-unix/X0";
    memcpy(sa.sun_path + 1, x11, strlen(x11));
    socklen_t salen = (socklen_t)(sizeof(sa.sun_family) + 1 + strlen(x11));
    if (connect(fd, (struct sockaddr *)&sa, salen) != 0)
        return fail(11);

    uint8_t req[12];
    memset(req, 0, sizeof(req));
    req[0] = 'l';
    req[2] = 11;
    req[4] = 0;
    if (write(fd, req, sizeof(req)) != (ssize_t)sizeof(req))
        return fail(12);

    uint8_t resp[8];
    ssize_t n = read(fd, resp, sizeof(resp));
    if (n < 1 || resp[0] != 1)
        return fail(13);

    if (close(fd) != 0)
        return fail(14);

    static const char ok[] = "NEXXON_LINUX_X11_OK\n";
    if (write(STDOUT_FILENO, ok, sizeof(ok) - 1) != (ssize_t)(sizeof(ok) - 1))
        return fail(15);
    return 0;
}
