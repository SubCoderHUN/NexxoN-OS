/* musl-static DRM dumb-buffer probe — create, map, pixel write. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define DRM_IOCTL_MODE_CREATE_DUMB 0xC02064B2u
#define DRM_IOCTL_MODE_MAP_DUMB    0xC01064B3u
#define DRM_IOCTL_MODE_DESTROY_DUMB 0xC00464B4u

struct drm_mode_create_dumb {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
};

struct drm_mode_map_dumb {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
};

struct drm_mode_destroy_dumb {
    uint32_t handle;
};

static int fail(int step) {
    static const char msg[] = "NEXXON_LINUX_DRMFB_FAIL\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    return step;
}

int main(void) {
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return fail(10);

    struct drm_mode_create_dumb cre;
    memset(&cre, 0, sizeof(cre));
    cre.width = 64;
    cre.height = 64;
    cre.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cre) != 0)
        return fail(11);
    if (!cre.handle || !cre.pitch || !cre.size)
        return fail(12);

    struct drm_mode_map_dumb map;
    memset(&map, 0, sizeof(map));
    map.handle = cre.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0)
        return fail(13);

    void *mem = mmap(NULL, cre.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                     fd, (off_t)map.offset);
    if (mem == MAP_FAILED)
        return fail(14);

    volatile uint32_t *px = (volatile uint32_t *)mem;
    px[32] = 0xA55A5AA5u;
    if (px[32] != 0xA55A5AA5u)
        return fail(15);

    if (munmap(mem, cre.size) != 0)
        return fail(16);

    struct drm_mode_destroy_dumb des;
    memset(&des, 0, sizeof(des));
    des.handle = cre.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &des) != 0)
        return fail(17);

    if (close(fd) != 0)
        return fail(18);

    static const char ok[] = "NEXXON_LINUX_DRMFB_OK\n";
    if (write(STDOUT_FILENO, ok, sizeof(ok) - 1) != (ssize_t)(sizeof(ok) - 1))
        return fail(19);
    return 0;
}
