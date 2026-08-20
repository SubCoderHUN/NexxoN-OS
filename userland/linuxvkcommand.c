/* glibc Vulkan loader probe — memory, buffer, command buffer, submit. */
#include <stdint.h>
#include <string.h>
#include <unistd.h>

typedef uint32_t VkResult;
typedef void *VkInstance;
typedef void *VkPhysicalDevice;
typedef void *VkDevice;
typedef void *VkQueue;
typedef void *VkCommandBuffer;
typedef uint64_t VkDeviceMemory;
typedef uint64_t VkBuffer;
typedef uint64_t VkCommandPool;

typedef struct { uint32_t sType; const void *pNext; const char *name;
    uint32_t appVersion; const char *engine; uint32_t engineVersion;
    uint32_t apiVersion; } VkApplicationInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags;
    const VkApplicationInfo *app; uint32_t layerCount; const char *const *layers;
    uint32_t extCount; const char *const *exts; } VkInstanceCreateInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags;
    uint32_t family; uint32_t count; const float *priorities;
    } VkDeviceQueueCreateInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags;
    uint32_t qCount; const VkDeviceQueueCreateInfo *queues;
    uint32_t layerCount; const char *const *layers; uint32_t extCount;
    const char *const *exts; const void *features; } VkDeviceCreateInfo;
typedef struct { uint32_t sType; const void *pNext; uint64_t size;
    uint32_t type; } VkMemoryAllocateInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags;
    uint64_t size; uint32_t usage; uint32_t sharing; uint32_t familyCount;
    const uint32_t *families; } VkBufferCreateInfo;
typedef struct { uint64_t size; uint64_t alignment; uint32_t typeBits;
    } VkMemoryRequirements;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags;
    uint32_t family; } VkCommandPoolCreateInfo;
typedef struct { uint32_t sType; const void *pNext; VkCommandPool pool;
    uint32_t level; uint32_t count; } VkCommandBufferAllocateInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags;
    const void *inheritance; } VkCommandBufferBeginInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t waitCount;
    const uint64_t *waits; const uint32_t *stages; uint32_t commandCount;
    const VkCommandBuffer *commands; uint32_t signalCount;
    const uint64_t *signals; } VkSubmitInfo;

extern VkResult vkCreateInstance(const VkInstanceCreateInfo *, const void *,
                                 VkInstance *);
extern void vkDestroyInstance(VkInstance, const void *);
extern VkResult vkEnumeratePhysicalDevices(VkInstance, uint32_t *,
                                           VkPhysicalDevice *);
extern VkResult vkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo *,
                               const void *, VkDevice *);
extern void vkDestroyDevice(VkDevice, const void *);
extern void vkGetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue *);
extern VkResult vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo *,
                                 const void *, VkDeviceMemory *);
extern void vkFreeMemory(VkDevice, VkDeviceMemory, const void *);
extern VkResult vkMapMemory(VkDevice, VkDeviceMemory, uint64_t, uint64_t,
                            uint32_t, void **);
extern void vkUnmapMemory(VkDevice, VkDeviceMemory);
extern VkResult vkCreateBuffer(VkDevice, const VkBufferCreateInfo *,
                               const void *, VkBuffer *);
extern void vkDestroyBuffer(VkDevice, VkBuffer, const void *);
extern void vkGetBufferMemoryRequirements(VkDevice, VkBuffer,
                                          VkMemoryRequirements *);
extern VkResult vkBindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory,
                                   uint64_t);
extern VkResult vkCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo *,
                                    const void *, VkCommandPool *);
extern void vkDestroyCommandPool(VkDevice, VkCommandPool, const void *);
extern VkResult vkAllocateCommandBuffers(
    VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *);
extern void vkFreeCommandBuffers(VkDevice, VkCommandPool, uint32_t,
                                 const VkCommandBuffer *);
extern VkResult vkBeginCommandBuffer(VkCommandBuffer,
                                     const VkCommandBufferBeginInfo *);
extern VkResult vkEndCommandBuffer(VkCommandBuffer);
extern VkResult vkQueueSubmit(VkQueue, uint32_t, const VkSubmitInfo *, void *);
extern VkResult vkQueueWaitIdle(VkQueue);

static int fail(int step) {
    static const char msg[] = "NEXXON_LINUX_VKCOMMAND_FAIL\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    return step;
}

int main(void) {
    VkApplicationInfo app = {0};
    app.name = "NexxoN"; app.apiVersion = 1u << 22;
    VkInstanceCreateInfo ici = {0}; ici.sType = 1; ici.app = &app;
    VkInstance inst = 0;
    if (vkCreateInstance(&ici, 0, &inst) != 0) return fail(10);
    VkPhysicalDevice phys = 0; uint32_t count = 1;
    if (vkEnumeratePhysicalDevices(inst, &count, &phys) != 0 || !phys)
        return fail(11);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {0};
    qci.sType = 2; qci.count = 1; qci.priorities = &priority;
    VkDeviceCreateInfo dci = {0};
    dci.sType = 3; dci.qCount = 1; dci.queues = &qci;
    VkDevice dev = 0;
    if (vkCreateDevice(phys, &dci, 0, &dev) != 0) return fail(12);
    VkQueue queue = 0; vkGetDeviceQueue(dev, 0, 0, &queue);
    if (!queue) return fail(13);

    VkMemoryAllocateInfo mai = {0};
    mai.sType = 5; mai.size = 4096; mai.type = 0;
    VkDeviceMemory memory = 0;
    if (vkAllocateMemory(dev, &mai, 0, &memory) != 0) return fail(14);
    uint32_t *mapped = 0;
    if (vkMapMemory(dev, memory, 0, 4096, 0, (void **)&mapped) != 0 ||
        !mapped) return fail(15);
    mapped[0] = 0x4E58564Bu;
    if (mapped[0] != 0x4E58564Bu) return fail(16);
    vkUnmapMemory(dev, memory);

    VkBufferCreateInfo bci = {0};
    bci.sType = 12; bci.size = 4096; bci.usage = 0x10u;
    VkBuffer buffer = 0;
    if (vkCreateBuffer(dev, &bci, 0, &buffer) != 0) return fail(17);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buffer, &req);
    if (req.size < 4096 || !(req.typeBits & 1)) return fail(18);
    if (vkBindBufferMemory(dev, buffer, memory, 0) != 0) return fail(19);

    VkCommandPoolCreateInfo pci = {0}; pci.sType = 39;
    VkCommandPool pool = 0;
    if (vkCreateCommandPool(dev, &pci, 0, &pool) != 0) return fail(20);
    VkCommandBufferAllocateInfo cai = {0};
    cai.sType = 40; cai.pool = pool; cai.count = 1;
    VkCommandBuffer command = 0;
    if (vkAllocateCommandBuffers(dev, &cai, &command) != 0 || !command)
        return fail(21);
    VkCommandBufferBeginInfo cbi = {0}; cbi.sType = 42;
    if (vkBeginCommandBuffer(command, &cbi) != 0 ||
        vkEndCommandBuffer(command) != 0) return fail(22);
    VkSubmitInfo submit = {0}; submit.sType = 4;
    submit.commandCount = 1; submit.commands = &command;
    if (vkQueueSubmit(queue, 1, &submit, 0) != 0 ||
        vkQueueWaitIdle(queue) != 0) return fail(23);

    vkFreeCommandBuffers(dev, pool, 1, &command);
    vkDestroyCommandPool(dev, pool, 0);
    vkDestroyBuffer(dev, buffer, 0);
    vkFreeMemory(dev, memory, 0);
    vkDestroyDevice(dev, 0);
    vkDestroyInstance(inst, 0);
    static const char ok[] = "NEXXON_LINUX_VKCOMMAND_OK\n";
    if (write(STDOUT_FILENO, ok, sizeof(ok) - 1) !=
        (ssize_t)(sizeof(ok) - 1)) return fail(24);
    return 0;
}
