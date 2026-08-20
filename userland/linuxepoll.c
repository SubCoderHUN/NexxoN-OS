/* musl-static epoll probe: pipe + epoll_wait. */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

static int fail(int step) {
    static const char msg[] = "NEXXON_LINUX_EPOLL_FAIL\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    return step;
}

int main(void) {
    int epfd = epoll_create1(0);
    if (epfd < 0)
        return fail(10);

    int fds[2];
    if (pipe(fds) != 0)
        return fail(11);

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u64 = 42;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fds[0], &ev) != 0)
        return fail(12);

    if (write(fds[1], "x", 1) != 1)
        return fail(13);

    struct epoll_event out;
    memset(&out, 0, sizeof(out));
    int n = epoll_wait(epfd, &out, 1, 2000);
    if (n != 1)
        return fail(14);
    if (!(out.events & EPOLLIN))
        return fail(15);
    if (out.data.u64 != 42)
        return fail(16);

    char ch;
    if (read(fds[0], &ch, 1) != 1 || ch != 'x')
        return fail(17);

    static const char ok[] = "NEXXON_LINUX_EPOLL_OK\n";
    if (write(STDOUT_FILENO, ok, sizeof(ok) - 1) != (ssize_t)(sizeof(ok) - 1))
        return fail(18);
    return 0;
}
