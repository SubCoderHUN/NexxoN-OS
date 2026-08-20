/* glibc-dynamic Vulkan loader probe — instance + physical device. */
#include <stdint.h>
#include <string.h>
#include <unistd.h>

typedef uint32_t VkResult;
typedef void *VkInstance;
typedef void *VkPhysicalDevice;

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

extern VkResult vkCreateInstance(const VkInstanceCreateInfo *create_info,
                                 const void *allocator, VkInstance *instance);
extern void vkDestroyInstance(VkInstance instance, const void *allocator);
extern VkResult vkEnumeratePhysicalDevices(VkInstance instance,
                                           uint32_t *count,
                                           VkPhysicalDevice *devices);

static int fail(int step) {
    static const char msg[] = "NEXXON_LINUX_VKINSTANCE_FAIL\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    return step;
}

int main(void) {
    VkApplicationInfo app;
    memset(&app, 0, sizeof(app));
    app.sType = 0; /* VK_STRUCTURE_TYPE_APPLICATION_INFO */
    app.pApplicationName = "NexxoN";
    app.apiVersion = (1u << 22); /* Vulkan 1.0 */

    VkInstanceCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.sType = 1; /* VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO */
    ci.pApplicationInfo = &app;

    VkInstance instance = 0;
    if (vkCreateInstance(&ci, 0, &instance) != 0 || !instance)
        return fail(10);

    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance, &count, 0) != 0 || count != 1) {
        vkDestroyInstance(instance, 0);
        return fail(11);
    }
    VkPhysicalDevice device = 0;
    count = 1;
    if (vkEnumeratePhysicalDevices(instance, &count, &device) != 0 ||
        count != 1 || !device) {
        vkDestroyInstance(instance, 0);
        return fail(12);
    }

    vkDestroyInstance(instance, 0);
    static const char ok[] = "NEXXON_LINUX_VKINSTANCE_OK\n";
    if (write(STDOUT_FILENO, ok, sizeof(ok) - 1) !=
        (ssize_t)(sizeof(ok) - 1))
        return fail(13);
    return 0;
}
