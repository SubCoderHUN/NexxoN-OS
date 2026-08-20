/* musl-dynamic Vulkan ICD probe — dlopen libvulkan_nexxon.so + negotiate. */
#include <dlfcn.h>
#include <stdint.h>
#include <unistd.h>

typedef uint32_t VkResult;
typedef void *VkInstance;
typedef void (*PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (*PFN_vk_icdGetInstanceProcAddr)(VkInstance,
                                                            const char *);
typedef VkResult (*PFN_vk_icdNegotiateLoaderICDInterfaceVersion)(uint32_t *);
typedef VkResult (*PFN_vkEnumerateInstanceExtensionProperties)(const char *,
                                                               uint32_t *,
                                                               void *);

static int fail(int step) {
    static const char msg[] = "NEXXON_LINUX_VULKANSO_FAIL\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    return step;
}

int main(void) {
    void *so = dlopen("/lib/x86_64-linux-gnu/libvulkan_nexxon.so", 1);
    if (!so)
        return fail(10);

    PFN_vk_icdNegotiateLoaderICDInterfaceVersion neg =
        (PFN_vk_icdNegotiateLoaderICDInterfaceVersion)
        dlsym(so, "vk_icdNegotiateLoaderICDInterfaceVersion");
    PFN_vk_icdGetInstanceProcAddr getipa =
        (PFN_vk_icdGetInstanceProcAddr)
        dlsym(so, "vk_icdGetInstanceProcAddr");
    if (!neg || !getipa) {
        dlclose(so);
        return fail(11);
    }

    uint32_t ver = 5;
    if (neg(&ver) != 0) {
        dlclose(so);
        return fail(12);
    }

    PFN_vkEnumerateInstanceExtensionProperties enumerate =
        (PFN_vkEnumerateInstanceExtensionProperties)
        getipa(NULL, "vkEnumerateInstanceExtensionProperties");
    if (!enumerate) {
        dlclose(so);
        return fail(13);
    }

    uint32_t count = 0;
    if (enumerate(NULL, &count, NULL) != 0) {
        dlclose(so);
        return fail(14);
    }

    dlclose(so);

    static const char ok[] = "NEXXON_LINUX_VULKANSO_OK\n";
    if (write(STDOUT_FILENO, ok, sizeof(ok) - 1) != (ssize_t)(sizeof(ok) - 1))
        return fail(15);
    return 0;
}
