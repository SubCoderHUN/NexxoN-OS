/* glibc Xlib + Vulkan loader probe — KHR surface capabilities. */
#include <X11/Xlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

typedef uint32_t VkResult;
typedef void *VkInstance;
typedef void *VkPhysicalDevice;
typedef uint64_t VkSurfaceKHR;
typedef struct { uint32_t sType; const void *pNext; const char *name;
    uint32_t appVersion; const char *engine; uint32_t engineVersion;
    uint32_t apiVersion; } VkApplicationInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags;
    const VkApplicationInfo *app; uint32_t layerCount; const char *const *layers;
    uint32_t extCount; const char *const *exts; } VkInstanceCreateInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags;
    Display *display; Window window; } VkXlibSurfaceCreateInfoKHR;
typedef struct { uint32_t minImages, maxImages, width, height;
    uint32_t minWidth, minHeight, maxWidth, maxHeight, maxLayers;
    uint32_t transforms, currentTransform, alpha, usage;
    } VkSurfaceCapabilitiesKHR;
typedef struct { uint32_t format, colorSpace; } VkSurfaceFormatKHR;

extern VkResult vkCreateInstance(const VkInstanceCreateInfo *, const void *,
                                 VkInstance *);
extern void vkDestroyInstance(VkInstance, const void *);
extern VkResult vkEnumeratePhysicalDevices(VkInstance, uint32_t *,
                                           VkPhysicalDevice *);
extern VkResult vkCreateXlibSurfaceKHR(
    VkInstance, const VkXlibSurfaceCreateInfoKHR *, const void *, VkSurfaceKHR *);
extern void vkDestroySurfaceKHR(VkInstance, VkSurfaceKHR, const void *);
extern VkResult vkGetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice, uint32_t, VkSurfaceKHR, uint32_t *);
extern VkResult vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR *);
extern VkResult vkGetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkSurfaceFormatKHR *);
extern VkResult vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice, VkSurfaceKHR, uint32_t *, uint32_t *);

static int fail(int step) {
    static const char msg[] = "NEXXON_LINUX_VKSURFACE_FAIL\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    return step;
}

int main(void) {
    Display *dpy = XOpenDisplay(":0");
    if (!dpy) return fail(10);
    int screen = DefaultScreen(dpy);
    Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen),
        24, 24, 640, 480, 0, BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    if (!win) return fail(11);
    XMapWindow(dpy, win);
    XSync(dpy, False);

    static const char *exts[] = {
        "VK_KHR_surface", "VK_KHR_xlib_surface"
    };
    VkApplicationInfo app = {0}; app.name = "NexxoN"; app.apiVersion = 1u << 22;
    VkInstanceCreateInfo ici = {0};
    ici.sType = 1; ici.app = &app; ici.extCount = 2; ici.exts = exts;
    VkInstance inst = 0;
    if (vkCreateInstance(&ici, 0, &inst) != 0) return fail(12);
    VkPhysicalDevice phys = 0; uint32_t count = 1;
    if (vkEnumeratePhysicalDevices(inst, &count, &phys) != 0 || !phys)
        return fail(13);

    VkXlibSurfaceCreateInfoKHR sci = {0};
    sci.sType = 1000004000u; sci.display = dpy; sci.window = win;
    VkSurfaceKHR surface = 0;
    if (vkCreateXlibSurfaceKHR(inst, &sci, 0, &surface) != 0 || !surface)
        return fail(14);
    uint32_t supported = 0;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(phys, 0, surface, &supported) != 0 ||
        !supported) return fail(15);
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps) != 0 ||
        caps.minImages < 2 || !caps.width || !caps.height) return fail(16);
    VkSurfaceFormatKHR format; count = 1;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &count, &format) != 0 ||
        count != 1) return fail(17);
    uint32_t mode = 0; count = 1;
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(
            phys, surface, &count, &mode) != 0 || count != 1 || mode != 2)
        return fail(18);

    vkDestroySurfaceKHR(inst, surface, 0);
    vkDestroyInstance(inst, 0);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    static const char ok[] = "NEXXON_LINUX_VKSURFACE_OK\n";
    if (write(STDOUT_FILENO, ok, sizeof(ok) - 1) !=
        (ssize_t)(sizeof(ok) - 1)) return fail(19);
    return 0;
}
