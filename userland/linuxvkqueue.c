/* glibc-dynamic Vulkan probe — logical device, queue, submit, idle. */
#include <stdint.h>
#include <string.h>
#include <unistd.h>

typedef uint32_t VkResult;
typedef void *VkInstance;
typedef void *VkPhysicalDevice;
typedef void *VkDevice;
typedef void *VkQueue;

typedef struct {
    uint32_t sType;
    const void *pNext;
    const char *pApplicationName;
    uint32_t applicationVersion;
    const char *pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
} VkApplicationInfo;

typedef struct {
    uint32_t sType;
    const void *pNext;
    uint32_t flags;
    const VkApplicationInfo *pApplicationInfo;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
} VkInstanceCreateInfo;

typedef struct {
    uint32_t sType;
    const void *pNext;
    uint32_t flags;
    uint32_t queueFamilyIndex;
    uint32_t queueCount;
    const float *pQueuePriorities;
} VkDeviceQueueCreateInfo;

typedef struct {
    uint32_t sType;
    const void *pNext;
    uint32_t flags;
    uint32_t queueCreateInfoCount;
    const VkDeviceQueueCreateInfo *pQueueCreateInfos;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
    const void *pEnabledFeatures;
} VkDeviceCreateInfo;

extern VkResult vkCreateInstance(const VkInstanceCreateInfo *, const void *,
                                 VkInstance *);
extern void vkDestroyInstance(VkInstance, const void *);
extern VkResult vkEnumeratePhysicalDevices(VkInstance, uint32_t *,
                                           VkPhysicalDevice *);
extern VkResult vkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo *,
                               const void *, VkDevice *);
extern void vkDestroyDevice(VkDevice, const void *);
extern void vkGetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue *);
extern VkResult vkQueueSubmit(VkQueue, uint32_t, const void *, void *);
extern VkResult vkQueueWaitIdle(VkQueue);
extern VkResult vkDeviceWaitIdle(VkDevice);
extern void (*vkGetDeviceProcAddr(VkDevice, const char *))(void);

static int fail(int step) {
    static const char msg[] = "NEXXON_LINUX_VKQUEUE_FAIL\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    return step;
}

int main(void) {
    VkApplicationInfo app;
    memset(&app, 0, sizeof(app));
    app.sType = 0;
    app.pApplicationName = "NexxoN";
    app.apiVersion = 1u << 22;

    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = 1;
    ici.pApplicationInfo = &app;
    VkInstance instance = 0;
    if (vkCreateInstance(&ici, 0, &instance) != 0)
        return fail(10);

    VkPhysicalDevice physical = 0;
    uint32_t count = 1;
    if (vkEnumeratePhysicalDevices(instance, &count, &physical) != 0 ||
        count != 1 || !physical)
        return fail(11);

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof(qci));
    qci.sType = 2;
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof(dci));
    dci.sType = 3;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;

    VkDevice device = 0;
    if (vkCreateDevice(physical, &dci, 0, &device) != 0 || !device)
        return fail(12);
    if (!*(void **)device)
        return fail(17);
    if ((uintptr_t)*(void **)device == 0x01CDC0DEu)
        return fail(18);
    if (!vkGetDeviceProcAddr(device, "vkGetDeviceQueue"))
        return fail(19);
    if (!vkGetDeviceProcAddr(device, "vkDestroyDevice"))
        return fail(22);
    if (!vkGetDeviceProcAddr(device, "vkDeviceWaitIdle"))
        return fail(20);
    if (vkDeviceWaitIdle(device) != 0)
        return fail(21);

    VkQueue queue = 0;
    vkGetDeviceQueue(device, 0, 0, &queue);
    if (!queue)
        return fail(13);
    if (vkQueueSubmit(queue, 0, 0, 0) != 0)
        return fail(14);
    if (vkQueueWaitIdle(queue) != 0 || vkDeviceWaitIdle(device) != 0)
        return fail(15);

    vkDestroyDevice(device, 0);
    vkDestroyInstance(instance, 0);
    static const char ok[] = "NEXXON_LINUX_VKQUEUE_OK\n";
    if (write(STDOUT_FILENO, ok, sizeof(ok) - 1) !=
        (ssize_t)(sizeof(ok) - 1))
        return fail(16);
    return 0;
}
