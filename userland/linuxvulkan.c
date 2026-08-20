/* musl-static Vulkan readiness probe — renderD128 + ICD manifest. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static int fail(int step) {
    static const char msg[] = "NEXXON_LINUX_VULKAN_FAIL\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    return step;
}

static int has_token(const char *hay, const char *needle) {
    return strstr(hay, needle) != NULL;
}

int main(void) {
    int dri = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (dri < 0)
        return fail(10);

    int icd = open("/usr/share/vulkan/icd.d/nexxon_icd.json", O_RDONLY | O_CLOEXEC);
    if (icd < 0) {
        close(dri);
        return fail(11);
    }

    char buf[384];
    ssize_t n = read(icd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        close(icd);
        close(dri);
        return fail(12);
    }
    buf[n] = '\0';
    if (!has_token(buf, "library_path") || !has_token(buf, "nexxon")) {
        close(icd);
        close(dri);
        return fail(13);
    }

    if (close(icd) != 0 || close(dri) != 0)
        return fail(14);

    static const char ok[] = "NEXXON_LINUX_VULKAN_OK\n";
    if (write(STDOUT_FILENO, ok, sizeof(ok) - 1) != (ssize_t)(sizeof(ok) - 1))
        return fail(15);
    return 0;
}
