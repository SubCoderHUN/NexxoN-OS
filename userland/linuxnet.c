/* musl-static probe: HTTP GET via Linux socket syscalls (AF_INET TCP). */
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define APT_PORT 8000

static int fail(void) {
    static const char msg[] = "NEXXON_LINUX_NET_FAIL\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    return 1;
}

int main(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return fail();

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(APT_PORT);
    /* 10.0.2.2 — QEMU slirp gateway (network byte order). */
    sa.sin_addr.s_addr = 0x0202000Au;

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) return fail();

    static const char req[] =
        "GET /Packages HTTP/1.1\r\n"
        "Host: 10.0.2.2\r\n"
        "Connection: close\r\n\r\n";
    if (write(fd, req, sizeof(req) - 1) != (ssize_t)(sizeof(req) - 1))
        return fail();

    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 12) return fail();
    if (memcmp(buf, "HTTP/1", 6) != 0) return fail();

    close(fd);
    static const char ok[] = "NEXXON_LINUX_SOCKET_OK\n";
    if (write(STDOUT_FILENO, ok, sizeof(ok) - 1) != (ssize_t)(sizeof(ok) - 1))
        return fail();
    return 0;
}
