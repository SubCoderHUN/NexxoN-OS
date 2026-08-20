/* glibc-dynamic real Vulkan loader probe — enumerate through NexxoN ICD. */
#include <stdint.h>
#include <unistd.h>

typedef uint32_t VkResult;
typedef struct {
    char extensionName[256];
    uint32_t specVersion;
} VkExtensionProperties;

extern VkResult vkEnumerateInstanceExtensionProperties(
    const char *layer_name, uint32_t *count, VkExtensionProperties *properties);

static int fail(int step) {
    static const char msg[] = "NEXXON_LINUX_VKLOADER_FAIL\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    return step;
}

int main(void) {
    uint32_t count = 0;
    VkResult rc = vkEnumerateInstanceExtensionProperties(0, &count, 0);
    if (rc != 0)
        return fail(10);

    static const char ok[] = "NEXXON_LINUX_VKLOADER_OK\n";
    if (write(STDOUT_FILENO, ok, sizeof(ok) - 1) !=
        (ssize_t)(sizeof(ok) - 1))
        return fail(11);
    return 0;
}
