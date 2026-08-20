/* Minimal NexxoN Vulkan ICD shared library (loader negotiation surface). */
#include <stdint.h>
#include <stddef.h>

#define VKAPI_ATTR
#define VKAPI_CALL
#define VK_SUCCESS 0
#define ICD_LOADER_MAGIC ((void *)(uintptr_t)0x01CDC0DEu)

typedef uint32_t VkResult;
typedef void *VkInstance;
typedef void *VkPhysicalDevice;
typedef void *VkDevice;
typedef void *VkQueue;
typedef void *VkCommandBuffer;
typedef uint64_t VkDeviceMemory;
typedef uint64_t VkBuffer;
typedef uint64_t VkCommandPool;
typedef uint64_t VkSurfaceKHR;
typedef uint64_t VkSwapchainKHR;
typedef uint64_t VkImage;
typedef void (*PFN_vkVoidFunction)(void);

typedef struct {
    void *dispatch;
} stub_dispatchable_t;

static stub_dispatchable_t g_instance;
static stub_dispatchable_t g_physical_device;
static stub_dispatchable_t g_device;
static stub_dispatchable_t g_queue;
static stub_dispatchable_t g_command_buffer;
static uint8_t g_memory[1024 * 1024];
static uint64_t g_buffer_token;
static uint64_t g_pool_token;
static uint64_t g_swapchain_token;
static uint64_t g_image_tokens[2];
typedef VkResult (*PFN_SetDeviceLoaderData)(VkDevice device, void *object);
static PFN_SetDeviceLoaderData g_set_device_loader_data;

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void bytes_zero(void *ptr, uint32_t len) {
    uint8_t *p = (uint8_t *)ptr;
    while (len--)
        *p++ = 0;
}

static void bytes_copy(void *dst, const void *src, uint32_t len) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (len--)
        *d++ = *s++;
}

static VkResult stub_enumerate_ext(const char *layer, uint32_t *count,
                                   void *props) {
    (void)layer;
    if (!count)
        return (VkResult)0xFFFFFFFFu;
    if (!props) {
        *count = 2;
        return VK_SUCCESS;
    }
    uint32_t requested = *count;
    uint8_t *p = (uint8_t *)props;
    if (requested > 0) {
        bytes_zero(p, 260);
        bytes_copy(p, "VK_KHR_surface", 15);
        *(uint32_t *)(p + 256) = 25;
    }
    if (requested > 1) {
        bytes_zero(p + 260, 260);
        bytes_copy(p + 260, "VK_KHR_xlib_surface", 20);
        *(uint32_t *)(p + 516) = 6;
    }
    *count = requested < 2 ? requested : 2;
    return VK_SUCCESS;
}

static VkResult stub_create_instance(const void *ci, const void *alloc,
                                     VkInstance *inst) {
    (void)ci;
    (void)alloc;
    if (!inst)
        return (VkResult)0xFFFFFFFFu;
    g_instance.dispatch = ICD_LOADER_MAGIC;
    *inst = (VkInstance)&g_instance;
    return VK_SUCCESS;
}

static void stub_destroy_instance(VkInstance inst, const void *alloc) {
    (void)inst;
    (void)alloc;
}

static VkResult stub_enumerate_physical_devices(
    VkInstance inst, uint32_t *count, VkPhysicalDevice *devices) {
    (void)inst;
    if (!count)
        return (VkResult)0xFFFFFFFFu;
    if (!devices) {
        *count = 1;
        return VK_SUCCESS;
    }
    if (*count >= 1) {
        g_physical_device.dispatch = ICD_LOADER_MAGIC;
        devices[0] = (VkPhysicalDevice)&g_physical_device;
        *count = 1;
    }
    return VK_SUCCESS;
}

static void stub_get_physical_device_properties(VkPhysicalDevice dev,
                                                void *properties) {
    (void)dev;
    if (!properties)
        return;
    uint8_t *p = (uint8_t *)properties;
    bytes_zero(p, 292);
    ((uint32_t *)p)[0] = (1u << 22) | (3u << 12); /* API 1.3 */
    ((uint32_t *)p)[1] = 1;                       /* driver version */
    ((uint32_t *)p)[2] = 0x1AF4u;                 /* virtual vendor */
    ((uint32_t *)p)[3] = 0x1050u;                 /* NexxoN device */
    ((uint32_t *)p)[4] = 4;                       /* CPU */
    bytes_copy(p + 20, "NexxoN Software Vulkan", 23);
}

static void stub_get_physical_device_features(VkPhysicalDevice dev,
                                              void *features) {
    (void)dev;
    if (features)
        bytes_zero(features, 220);
}

static void stub_get_physical_device_format_properties(VkPhysicalDevice dev,
                                                       uint32_t format,
                                                       void *properties) {
    (void)dev;
    (void)format;
    if (properties)
        bytes_zero(properties, 12);
}

static VkResult stub_get_physical_device_image_format_properties(
    VkPhysicalDevice dev, uint32_t format, uint32_t type, uint32_t tiling,
    uint32_t usage, uint32_t flags, void *properties) {
    (void)dev; (void)format; (void)type; (void)tiling;
    (void)usage; (void)flags; (void)properties;
    return (VkResult)-11; /* VK_ERROR_FORMAT_NOT_SUPPORTED */
}

static void stub_get_physical_device_queue_family_properties(
    VkPhysicalDevice dev, uint32_t *count, void *properties) {
    (void)dev;
    if (!count)
        return;
    if (!properties) {
        *count = 1;
        return;
    }
    if (*count >= 1) {
        uint8_t *p = (uint8_t *)properties;
        bytes_zero(p, 24);
        ((uint32_t *)p)[0] = 7;  /* graphics | compute | transfer */
        ((uint32_t *)p)[1] = 1;  /* one queue */
        ((uint32_t *)p)[2] = 64; /* timestamp bits */
        ((uint32_t *)p)[3] = 1;
        ((uint32_t *)p)[4] = 1;
        ((uint32_t *)p)[5] = 1;
        *count = 1;
    }
}

static void stub_get_physical_device_memory_properties(VkPhysicalDevice dev,
                                                       void *properties) {
    (void)dev;
    if (!properties)
        return;
    uint8_t *p = (uint8_t *)properties;
    bytes_zero(p, 520);
    ((uint32_t *)p)[0] = 1;    /* memoryTypeCount */
    ((uint32_t *)p)[1] = 7;    /* device local + host visible + coherent */
    ((uint32_t *)p)[2] = 0;    /* heap index */
    *(uint32_t *)(p + 260) = 1; /* memoryHeapCount */
    *(uint64_t *)(p + 264) = 256u * 1024u * 1024u;
    *(uint32_t *)(p + 272) = 1; /* device-local heap */
}

static void stub_get_physical_device_sparse_image_format_properties(
    VkPhysicalDevice dev, uint32_t format, uint32_t type, uint32_t samples,
    uint32_t usage, uint32_t tiling, uint32_t *count, void *properties) {
    (void)dev; (void)format; (void)type; (void)samples;
    (void)usage; (void)tiling; (void)properties;
    if (count)
        *count = 0;
}

static VkResult stub_create_device(VkPhysicalDevice physical,
                                   const void *create_info,
                                   const void *allocator,
                                   VkDevice *device) {
    (void)physical;
    (void)create_info;
    (void)allocator;
    if (!device)
        return (VkResult)0xFFFFFFFFu;
    g_set_device_loader_data = NULL;
    if (create_info) {
        const uint8_t *ci = (const uint8_t *)create_info;
        const uint8_t *next = *(const uint8_t *const *)(ci + 8);
        while (next) {
            uint32_t s_type = *(const uint32_t *)next;
            const uint8_t *following =
                *(const uint8_t *const *)(next + 8);
            if (s_type == 48 && *(const uint32_t *)(next + 16) == 1) {
                g_set_device_loader_data =
                    *(PFN_SetDeviceLoaderData const *)(next + 24);
                break;
            }
            next = following;
        }
    }
    g_device.dispatch = ICD_LOADER_MAGIC;
    *device = (VkDevice)&g_device;
    return VK_SUCCESS;
}

static VkResult stub_enumerate_device_extension_properties(
    VkPhysicalDevice physical, const char *layer, uint32_t *count,
    void *properties) {
    (void)physical;
    (void)layer;
    if (!count)
        return (VkResult)0xFFFFFFFFu;
    if (!properties) {
        *count = 1;
        return VK_SUCCESS;
    }
    if (*count >= 1) {
        bytes_zero(properties, 260);
        bytes_copy(properties, "VK_KHR_swapchain", 17);
        *(uint32_t *)((uint8_t *)properties + 256) = 70;
        *count = 1;
    }
    return VK_SUCCESS;
}

static void stub_destroy_surface(VkInstance instance, VkSurfaceKHR surface,
                                 const void *allocator) {
    (void)instance; (void)surface; (void)allocator;
}

static VkResult stub_surface_support(VkPhysicalDevice physical,
                                     uint32_t queue_family,
                                     VkSurfaceKHR surface,
                                     uint32_t *supported) {
    (void)physical; (void)queue_family; (void)surface;
    if (!supported)
        return (VkResult)0xFFFFFFFFu;
    *supported = 1;
    return VK_SUCCESS;
}

static VkResult stub_surface_capabilities(VkPhysicalDevice physical,
                                          VkSurfaceKHR surface,
                                          void *capabilities) {
    (void)physical; (void)surface;
    if (!capabilities)
        return (VkResult)0xFFFFFFFFu;
    uint8_t *p = (uint8_t *)capabilities;
    bytes_zero(p, 52);
    *(uint32_t *)(p + 0) = 2;    /* min images */
    *(uint32_t *)(p + 4) = 3;    /* max images */
    *(uint32_t *)(p + 8) = 640;  /* current width */
    *(uint32_t *)(p + 12) = 480;
    *(uint32_t *)(p + 16) = 1;
    *(uint32_t *)(p + 20) = 1;
    *(uint32_t *)(p + 24) = 4096;
    *(uint32_t *)(p + 28) = 4096;
    *(uint32_t *)(p + 32) = 1;   /* layers */
    *(uint32_t *)(p + 36) = 1;   /* identity transform */
    *(uint32_t *)(p + 40) = 1;
    *(uint32_t *)(p + 44) = 1;   /* opaque alpha */
    *(uint32_t *)(p + 48) = 0x10; /* color attachment */
    return VK_SUCCESS;
}

static VkResult stub_surface_formats(VkPhysicalDevice physical,
                                     VkSurfaceKHR surface, uint32_t *count,
                                     void *formats) {
    (void)physical; (void)surface;
    if (!count)
        return (VkResult)0xFFFFFFFFu;
    if (!formats) {
        *count = 1;
        return VK_SUCCESS;
    }
    if (*count >= 1) {
        *(uint32_t *)formats = 44; /* B8G8R8A8_UNORM */
        *(uint32_t *)((uint8_t *)formats + 4) = 0;
        *count = 1;
    }
    return VK_SUCCESS;
}

static VkResult stub_present_modes(VkPhysicalDevice physical,
                                   VkSurfaceKHR surface, uint32_t *count,
                                   uint32_t *modes) {
    (void)physical; (void)surface;
    if (!count)
        return (VkResult)0xFFFFFFFFu;
    if (!modes) {
        *count = 1;
        return VK_SUCCESS;
    }
    if (*count >= 1) {
        modes[0] = 2; /* FIFO */
        *count = 1;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyDevice(VkDevice device, const void *allocator) {
    (void)device;
    (void)allocator;
}

VKAPI_ATTR void VKAPI_CALL
vkGetDeviceQueue(VkDevice device, uint32_t family,
                 uint32_t index, VkQueue *queue) {
    (void)device;
    (void)family;
    (void)index;
    if (queue) {
        g_queue.dispatch = ICD_LOADER_MAGIC;
        *queue = (VkQueue)&g_queue;
        if (g_set_device_loader_data)
            (void)g_set_device_loader_data(device, &g_queue);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit(VkQueue queue, uint32_t submit_count,
              const void *submits, void *fence) {
    (void)queue;
    (void)submit_count;
    (void)submits;
    (void)fence;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkQueueWaitIdle(VkQueue queue) {
    (void)queue;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkDeviceWaitIdle(VkDevice device) {
    (void)device;
    return VK_SUCCESS;
}

static VkResult stub_allocate_memory(VkDevice device, const void *info,
                                     const void *allocator,
                                     VkDeviceMemory *memory) {
    (void)device; (void)info; (void)allocator;
    if (!memory)
        return (VkResult)0xFFFFFFFFu;
    *memory = (VkDeviceMemory)(uintptr_t)g_memory;
    return VK_SUCCESS;
}

static void stub_free_memory(VkDevice device, VkDeviceMemory memory,
                             const void *allocator) {
    (void)device; (void)memory; (void)allocator;
}

static VkResult stub_map_memory(VkDevice device, VkDeviceMemory memory,
                                uint64_t offset, uint64_t size,
                                uint32_t flags, void **data) {
    (void)device; (void)memory; (void)size; (void)flags;
    if (!data || offset >= sizeof(g_memory))
        return (VkResult)0xFFFFFFFFu;
    *data = g_memory + offset;
    return VK_SUCCESS;
}

static void stub_unmap_memory(VkDevice device, VkDeviceMemory memory) {
    (void)device; (void)memory;
}

static VkResult stub_memory_ranges(VkDevice device, uint32_t count,
                                   const void *ranges) {
    (void)device; (void)count; (void)ranges;
    return VK_SUCCESS;
}

static VkResult stub_create_buffer(VkDevice device, const void *info,
                                   const void *allocator, VkBuffer *buffer) {
    (void)device; (void)info; (void)allocator;
    if (!buffer)
        return (VkResult)0xFFFFFFFFu;
    *buffer = (VkBuffer)(uintptr_t)&g_buffer_token;
    return VK_SUCCESS;
}

static void stub_destroy_buffer(VkDevice device, VkBuffer buffer,
                                const void *allocator) {
    (void)device; (void)buffer; (void)allocator;
}

static void stub_get_buffer_memory_requirements(VkDevice device,
                                                VkBuffer buffer,
                                                void *requirements) {
    (void)device; (void)buffer;
    if (!requirements)
        return;
    uint8_t *p = (uint8_t *)requirements;
    bytes_zero(p, 24);
    *(uint64_t *)(p + 0) = 4096;
    *(uint64_t *)(p + 8) = 16;
    *(uint32_t *)(p + 16) = 1;
}

static VkResult stub_bind_buffer_memory(VkDevice device, VkBuffer buffer,
                                        VkDeviceMemory memory,
                                        uint64_t offset) {
    (void)device; (void)buffer; (void)memory; (void)offset;
    return VK_SUCCESS;
}

static VkResult stub_create_command_pool(VkDevice device, const void *info,
                                         const void *allocator,
                                         VkCommandPool *pool) {
    (void)device; (void)info; (void)allocator;
    if (!pool)
        return (VkResult)0xFFFFFFFFu;
    *pool = (VkCommandPool)(uintptr_t)&g_pool_token;
    return VK_SUCCESS;
}

static void stub_destroy_command_pool(VkDevice device, VkCommandPool pool,
                                      const void *allocator) {
    (void)device; (void)pool; (void)allocator;
}

static VkResult stub_reset_command_pool(VkDevice device, VkCommandPool pool,
                                        uint32_t flags) {
    (void)device; (void)pool; (void)flags;
    return VK_SUCCESS;
}

static VkResult stub_allocate_command_buffers(VkDevice device,
                                              const void *info,
                                              VkCommandBuffer *buffers) {
    if (!info || !buffers)
        return (VkResult)0xFFFFFFFFu;
    uint32_t count = *(const uint32_t *)((const uint8_t *)info + 28);
    for (uint32_t i = 0; i < count; i++) {
        g_command_buffer.dispatch = ICD_LOADER_MAGIC;
        buffers[i] = (VkCommandBuffer)&g_command_buffer;
        if (g_set_device_loader_data)
            (void)g_set_device_loader_data(device, &g_command_buffer);
    }
    return VK_SUCCESS;
}

static void stub_free_command_buffers(VkDevice device, VkCommandPool pool,
                                      uint32_t count,
                                      const VkCommandBuffer *buffers) {
    (void)device; (void)pool; (void)count; (void)buffers;
}

static VkResult stub_begin_command_buffer(VkCommandBuffer command,
                                          const void *info) {
    (void)command; (void)info;
    return VK_SUCCESS;
}

static VkResult stub_end_command_buffer(VkCommandBuffer command) {
    (void)command;
    return VK_SUCCESS;
}

static VkResult stub_reset_command_buffer(VkCommandBuffer command,
                                          uint32_t flags) {
    (void)command; (void)flags;
    return VK_SUCCESS;
}

static VkResult stub_create_swapchain(VkDevice device, const void *info,
                                      const void *allocator,
                                      VkSwapchainKHR *swapchain) {
    (void)device; (void)info; (void)allocator;
    if (!swapchain)
        return (VkResult)0xFFFFFFFFu;
    *swapchain = (VkSwapchainKHR)(uintptr_t)&g_swapchain_token;
    return VK_SUCCESS;
}

static void stub_destroy_swapchain(VkDevice device,
                                   VkSwapchainKHR swapchain,
                                   const void *allocator) {
    (void)device; (void)swapchain; (void)allocator;
}

static VkResult stub_get_swapchain_images(VkDevice device,
                                          VkSwapchainKHR swapchain,
                                          uint32_t *count, VkImage *images) {
    (void)device; (void)swapchain;
    if (!count)
        return (VkResult)0xFFFFFFFFu;
    if (!images) {
        *count = 2;
        return VK_SUCCESS;
    }
    uint32_t n = *count < 2 ? *count : 2;
    for (uint32_t i = 0; i < n; i++)
        images[i] = (VkImage)(uintptr_t)&g_image_tokens[i];
    *count = n;
    return VK_SUCCESS;
}

static VkResult stub_acquire_next_image(VkDevice device,
                                        VkSwapchainKHR swapchain,
                                        uint64_t timeout, uint64_t semaphore,
                                        uint64_t fence, uint32_t *index) {
    (void)device; (void)swapchain; (void)timeout;
    (void)semaphore; (void)fence;
    if (!index)
        return (VkResult)0xFFFFFFFFu;
    *index = 0;
    return VK_SUCCESS;
}

static VkResult stub_queue_present(VkQueue queue, const void *present_info) {
    (void)queue; (void)present_info;
    return VK_SUCCESS;
}

static VkResult stub_enumerate_version(uint32_t *version) {
    if (!version)
        return (VkResult)0xFFFFFFFFu;
    *version = (1u << 22) | (3u << 12); /* Vulkan 1.3.0 */
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *version) {
    if (!version)
        return (VkResult)0xFFFFFFFFu;
    if (*version > 5)
        *version = 5;
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *name) {
    (void)device;
    if (!name)
        return NULL;
    if (str_eq(name, "vkDestroyDevice"))
        return (PFN_vkVoidFunction)vkDestroyDevice;
    if (str_eq(name, "vkGetDeviceQueue"))
        return (PFN_vkVoidFunction)vkGetDeviceQueue;
    if (str_eq(name, "vkQueueSubmit"))
        return (PFN_vkVoidFunction)vkQueueSubmit;
    if (str_eq(name, "vkQueueWaitIdle"))
        return (PFN_vkVoidFunction)vkQueueWaitIdle;
    if (str_eq(name, "vkDeviceWaitIdle"))
        return (PFN_vkVoidFunction)vkDeviceWaitIdle;
    if (str_eq(name, "vkAllocateMemory"))
        return (PFN_vkVoidFunction)stub_allocate_memory;
    if (str_eq(name, "vkFreeMemory"))
        return (PFN_vkVoidFunction)stub_free_memory;
    if (str_eq(name, "vkMapMemory"))
        return (PFN_vkVoidFunction)stub_map_memory;
    if (str_eq(name, "vkUnmapMemory"))
        return (PFN_vkVoidFunction)stub_unmap_memory;
    if (str_eq(name, "vkFlushMappedMemoryRanges") ||
        str_eq(name, "vkInvalidateMappedMemoryRanges"))
        return (PFN_vkVoidFunction)stub_memory_ranges;
    if (str_eq(name, "vkCreateBuffer"))
        return (PFN_vkVoidFunction)stub_create_buffer;
    if (str_eq(name, "vkDestroyBuffer"))
        return (PFN_vkVoidFunction)stub_destroy_buffer;
    if (str_eq(name, "vkGetBufferMemoryRequirements"))
        return (PFN_vkVoidFunction)stub_get_buffer_memory_requirements;
    if (str_eq(name, "vkBindBufferMemory"))
        return (PFN_vkVoidFunction)stub_bind_buffer_memory;
    if (str_eq(name, "vkCreateCommandPool"))
        return (PFN_vkVoidFunction)stub_create_command_pool;
    if (str_eq(name, "vkDestroyCommandPool"))
        return (PFN_vkVoidFunction)stub_destroy_command_pool;
    if (str_eq(name, "vkResetCommandPool"))
        return (PFN_vkVoidFunction)stub_reset_command_pool;
    if (str_eq(name, "vkAllocateCommandBuffers"))
        return (PFN_vkVoidFunction)stub_allocate_command_buffers;
    if (str_eq(name, "vkFreeCommandBuffers"))
        return (PFN_vkVoidFunction)stub_free_command_buffers;
    if (str_eq(name, "vkBeginCommandBuffer"))
        return (PFN_vkVoidFunction)stub_begin_command_buffer;
    if (str_eq(name, "vkEndCommandBuffer"))
        return (PFN_vkVoidFunction)stub_end_command_buffer;
    if (str_eq(name, "vkResetCommandBuffer"))
        return (PFN_vkVoidFunction)stub_reset_command_buffer;
    if (str_eq(name, "vkCreateSwapchainKHR"))
        return (PFN_vkVoidFunction)stub_create_swapchain;
    if (str_eq(name, "vkDestroySwapchainKHR"))
        return (PFN_vkVoidFunction)stub_destroy_swapchain;
    if (str_eq(name, "vkGetSwapchainImagesKHR"))
        return (PFN_vkVoidFunction)stub_get_swapchain_images;
    if (str_eq(name, "vkAcquireNextImageKHR"))
        return (PFN_vkVoidFunction)stub_acquire_next_image;
    if (str_eq(name, "vkQueuePresentKHR"))
        return (PFN_vkVoidFunction)stub_queue_present;
    return NULL;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *name) {
    (void)instance;
    if (!name)
        return NULL;
    if (str_eq(name, "vkEnumerateInstanceExtensionProperties"))
        return (PFN_vkVoidFunction)stub_enumerate_ext;
    if (str_eq(name, "vkEnumerateInstanceVersion"))
        return (PFN_vkVoidFunction)stub_enumerate_version;
    if (str_eq(name, "vkCreateInstance"))
        return (PFN_vkVoidFunction)stub_create_instance;
    if (str_eq(name, "vkDestroyInstance"))
        return (PFN_vkVoidFunction)stub_destroy_instance;
    if (str_eq(name, "vkEnumeratePhysicalDevices"))
        return (PFN_vkVoidFunction)stub_enumerate_physical_devices;
    if (str_eq(name, "vkGetPhysicalDeviceProperties"))
        return (PFN_vkVoidFunction)stub_get_physical_device_properties;
    if (str_eq(name, "vkGetPhysicalDeviceFeatures"))
        return (PFN_vkVoidFunction)stub_get_physical_device_features;
    if (str_eq(name, "vkGetPhysicalDeviceFormatProperties"))
        return (PFN_vkVoidFunction)stub_get_physical_device_format_properties;
    if (str_eq(name, "vkGetPhysicalDeviceImageFormatProperties"))
        return (PFN_vkVoidFunction)
            stub_get_physical_device_image_format_properties;
    if (str_eq(name, "vkGetPhysicalDeviceQueueFamilyProperties"))
        return (PFN_vkVoidFunction)
            stub_get_physical_device_queue_family_properties;
    if (str_eq(name, "vkGetPhysicalDeviceMemoryProperties"))
        return (PFN_vkVoidFunction)stub_get_physical_device_memory_properties;
    if (str_eq(name, "vkGetPhysicalDeviceSparseImageFormatProperties"))
        return (PFN_vkVoidFunction)
            stub_get_physical_device_sparse_image_format_properties;
    if (str_eq(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (str_eq(name, "vkCreateDevice"))
        return (PFN_vkVoidFunction)stub_create_device;
    if (str_eq(name, "vkEnumerateDeviceExtensionProperties"))
        return (PFN_vkVoidFunction)
            stub_enumerate_device_extension_properties;
    if (str_eq(name, "vkDestroySurfaceKHR"))
        return (PFN_vkVoidFunction)stub_destroy_surface;
    if (str_eq(name, "vkGetPhysicalDeviceSurfaceSupportKHR"))
        return (PFN_vkVoidFunction)stub_surface_support;
    if (str_eq(name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
        return (PFN_vkVoidFunction)stub_surface_capabilities;
    if (str_eq(name, "vkGetPhysicalDeviceSurfaceFormatsKHR"))
        return (PFN_vkVoidFunction)stub_surface_formats;
    if (str_eq(name, "vkGetPhysicalDeviceSurfacePresentModesKHR"))
        return (PFN_vkVoidFunction)stub_present_modes;
    return vkGetDeviceProcAddr(NULL, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *name) {
    (void)instance;
    (void)name;
    return NULL;
}
