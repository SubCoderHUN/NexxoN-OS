/* glibc Xlib + Vulkan loader probe — swapchain acquire/present. */
#include <X11/Xlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

typedef uint32_t VkResult;
typedef void *VkInstance; typedef void *VkPhysicalDevice;
typedef void *VkDevice; typedef void *VkQueue;
typedef uint64_t VkSurfaceKHR; typedef uint64_t VkSwapchainKHR;
typedef uint64_t VkImage;
typedef struct { uint32_t sType; const void *pNext; const char *name;
    uint32_t appVersion; const char *engine; uint32_t engineVersion;
    uint32_t apiVersion; } VkApplicationInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags;
    const VkApplicationInfo *app; uint32_t layerCount; const char *const *layers;
    uint32_t extCount; const char *const *exts; } VkInstanceCreateInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags;
    Display *display; Window window; } VkXlibSurfaceCreateInfoKHR;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags;
    uint32_t family, count; const float *priorities; } VkDeviceQueueCreateInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags;
    uint32_t qCount; const VkDeviceQueueCreateInfo *queues;
    uint32_t layerCount; const char *const *layers; uint32_t extCount;
    const char *const *exts; const void *features; } VkDeviceCreateInfo;
typedef struct { uint32_t width, height; } VkExtent2D;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags;
    VkSurfaceKHR surface; uint32_t minImages, format, colorSpace;
    VkExtent2D extent; uint32_t layers, usage, sharing, familyCount;
    const uint32_t *families; uint32_t transform, alpha, presentMode, clipped;
    VkSwapchainKHR oldSwapchain; } VkSwapchainCreateInfoKHR;
typedef struct { uint32_t sType; const void *pNext; uint32_t waitCount;
    const uint64_t *waits; uint32_t swapchainCount;
    const VkSwapchainKHR *swapchains; const uint32_t *indices;
    VkResult *results; } VkPresentInfoKHR;

extern VkResult vkCreateInstance(const VkInstanceCreateInfo *, const void *,
                                 VkInstance *);
extern void vkDestroyInstance(VkInstance, const void *);
extern VkResult vkEnumeratePhysicalDevices(VkInstance, uint32_t *,
                                           VkPhysicalDevice *);
extern VkResult vkCreateXlibSurfaceKHR(
    VkInstance, const VkXlibSurfaceCreateInfoKHR *, const void *, VkSurfaceKHR *);
extern void vkDestroySurfaceKHR(VkInstance, VkSurfaceKHR, const void *);
extern VkResult vkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo *,
                               const void *, VkDevice *);
extern void vkDestroyDevice(VkDevice, const void *);
extern void vkGetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue *);
extern VkResult vkCreateSwapchainKHR(
    VkDevice, const VkSwapchainCreateInfoKHR *, const void *, VkSwapchainKHR *);
extern void vkDestroySwapchainKHR(VkDevice, VkSwapchainKHR, const void *);
extern VkResult vkGetSwapchainImagesKHR(VkDevice, VkSwapchainKHR,
                                        uint32_t *, VkImage *);
extern VkResult vkAcquireNextImageKHR(VkDevice, VkSwapchainKHR, uint64_t,
                                      uint64_t, uint64_t, uint32_t *);
extern VkResult vkQueuePresentKHR(VkQueue, const VkPresentInfoKHR *);
extern VkResult vkQueueWaitIdle(VkQueue);

static int fail(int step) {
    static const char msg[] = "NEXXON_LINUX_VKSWAPCHAIN_FAIL\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    return step;
}

int main(void) {
    Display *dpy = XOpenDisplay(":0"); if (!dpy) return fail(10);
    int screen = DefaultScreen(dpy);
    Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen),
        32, 32, 640, 480, 0, BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    XMapWindow(dpy, win); XSync(dpy, False);
    static const char *iexts[] = {"VK_KHR_surface", "VK_KHR_xlib_surface"};
    VkApplicationInfo app = {0}; app.name = "NexxoN"; app.apiVersion = 1u << 22;
    VkInstanceCreateInfo ici = {0}; ici.sType = 1; ici.app = &app;
    ici.extCount = 2; ici.exts = iexts;
    VkInstance inst = 0; if (vkCreateInstance(&ici, 0, &inst)) return fail(11);
    VkPhysicalDevice phys = 0; uint32_t count = 1;
    if (vkEnumeratePhysicalDevices(inst, &count, &phys) || !phys) return fail(12);
    VkXlibSurfaceCreateInfoKHR sci = {0}; sci.sType = 1000004000u;
    sci.display = dpy; sci.window = win;
    VkSurfaceKHR surface = 0;
    if (vkCreateXlibSurfaceKHR(inst, &sci, 0, &surface) || !surface)
        return fail(13);
    float priority = 1.0f; VkDeviceQueueCreateInfo qci = {0};
    qci.sType = 2; qci.count = 1; qci.priorities = &priority;
    static const char *dexts[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci = {0}; dci.sType = 3; dci.qCount = 1;
    dci.queues = &qci; dci.extCount = 1; dci.exts = dexts;
    VkDevice dev = 0; if (vkCreateDevice(phys, &dci, 0, &dev)) return fail(14);
    VkQueue queue = 0; vkGetDeviceQueue(dev, 0, 0, &queue);
    VkSwapchainCreateInfoKHR ci = {0}; ci.sType = 1000001000u;
    ci.surface = surface; ci.minImages = 2; ci.format = 44;
    ci.extent.width = 640; ci.extent.height = 480; ci.layers = 1;
    ci.usage = 0x10; ci.transform = 1; ci.alpha = 1; ci.presentMode = 2;
    ci.clipped = 1;
    VkSwapchainKHR swapchain = 0;
    if (vkCreateSwapchainKHR(dev, &ci, 0, &swapchain) || !swapchain)
        return fail(15);
    VkImage images[2]; count = 2;
    if (vkGetSwapchainImagesKHR(dev, swapchain, &count, images) ||
        count != 2 || !images[0] || !images[1]) return fail(16);
    uint32_t index = 99;
    if (vkAcquireNextImageKHR(dev, swapchain, ~0ull, 0, 0, &index) ||
        index >= count) return fail(17);
    VkPresentInfoKHR present = {0}; present.sType = 1000001001u;
    present.swapchainCount = 1; present.swapchains = &swapchain;
    present.indices = &index;
    if (vkQueuePresentKHR(queue, &present) || vkQueueWaitIdle(queue))
        return fail(18);
    vkDestroySwapchainKHR(dev, swapchain, 0); vkDestroyDevice(dev, 0);
    vkDestroySurfaceKHR(inst, surface, 0); vkDestroyInstance(inst, 0);
    XDestroyWindow(dpy, win); XCloseDisplay(dpy);
    static const char ok[] = "NEXXON_LINUX_VKSWAPCHAIN_OK\n";
    if (write(1, ok, sizeof(ok) - 1) != (ssize_t)(sizeof(ok) - 1))
        return fail(19);
    return 0;
}
