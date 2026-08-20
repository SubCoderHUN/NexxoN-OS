/* musl-static DRM /dev/dri probe for the NexxoN display backend milestone. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DRM_IOCTL_VERSION 0xC0406400u
#define DRM_IOCTL_GET_CAP 0xC010640Cu
#define DRM_CAP_DUMB_BUFFER 0x1u

struct drm_version {
    int version_major;
    int version_minor;
    int version_patchlevel;
    size_t name_len;
    char *name;
    size_t date_len;
    char *date;
    size_t desc_len;
    char *desc;
};

struct drm_get_cap {
    uint64_t capability;
    uint64_t value;
};

static int fail(int step) {
    static const char msg[] = "NEXXON_LINUX_DRI_FAIL\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    return step;
}

int main(void) {
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return fail(10);

    struct drm_version ver;
    memset(&ver, 0, sizeof(ver));
    char name[64], date[64], desc[64];
    ver.name_len = sizeof(name);
    ver.name = name;
    ver.date_len = sizeof(date);
    ver.date = date;
    ver.desc_len = sizeof(desc);
    ver.desc = desc;

    if (ioctl(fd, DRM_IOCTL_VERSION, &ver) != 0)
        return fail(11);
    if (ver.version_major < 1)
        return fail(12);
    if (ver.name_len == 0 || name[0] == 0)
        return fail(13);

    struct drm_get_cap cap;
    memset(&cap, 0, sizeof(cap));
    cap.capability = DRM_CAP_DUMB_BUFFER;
    if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) != 0)
        return fail(14);
    if (cap.value == 0)
        return fail(15);

    if (close(fd) != 0)
        return fail(16);

    static const char ok[] = "NEXXON_LINUX_DRI_OK\n";
    if (write(STDOUT_FILENO, ok, sizeof(ok) - 1) != (ssize_t)(sizeof(ok) - 1))
        return fail(17);
    return 0;
}
