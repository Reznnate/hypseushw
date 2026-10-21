/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 HypseusHW contributors
 *
 * lr-host --vulkan: a headless Vulkan "frontend" for cores that render with
 * Vulkan.  It creates its own device, hands the core the libretro Vulkan
 * interface, and reads the presented image back for --dump.  Included by
 * lr_host.cpp.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <libretro_vulkan.h>

namespace hostvk {

static SDL_SharedObject *s_lib;
static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance s_instance;
static VkPhysicalDevice s_gpu;
static VkDevice s_device;
static VkQueue s_queue;
static uint32_t s_family;
static retro_hw_render_interface_vulkan s_iface;
static retro_vulkan_image s_image;      // last image the core presented
static bool s_have_image;
static uint32_t s_frame;                // advances once per retro_run

#define HVK(name) static PFN_##name name##_;
HVK(vkQueueWaitIdle) HVK(vkCreateCommandPool) HVK(vkAllocateCommandBuffers) HVK(vkBeginCommandBuffer)
HVK(vkEndCommandBuffer) HVK(vkCmdPipelineBarrier) HVK(vkCmdCopyImageToBuffer) HVK(vkCreateBuffer)
HVK(vkGetBufferMemoryRequirements) HVK(vkAllocateMemory) HVK(vkBindBufferMemory) HVK(vkMapMemory)
HVK(vkQueueSubmit) HVK(vkGetPhysicalDeviceMemoryProperties) HVK(vkDestroyDevice) HVK(vkDestroyInstance)
HVK(vkDeviceWaitIdle) HVK(vkUnmapMemory) HVK(vkDestroyBuffer) HVK(vkFreeMemory) HVK(vkDestroyCommandPool)
#undef HVK

static void set_image(void *, const retro_vulkan_image *img, uint32_t, const VkSemaphore *, uint32_t)
{
    s_image = *img;
    s_have_image = true;
}
static uint32_t get_sync_index(void *) { return s_frame & 1; }
static uint32_t get_sync_index_mask(void *) { return 3; }
static void set_command_buffers(void *, uint32_t, const VkCommandBuffer *) {}
// The previous use of every index is finished once the queue is idle.
static void wait_sync_index(void *) { vkQueueWaitIdle_(s_queue); }
static void lock_queue(void *) {}
static void unlock_queue(void *) {}
static void set_signal_semaphore(void *, VkSemaphore) {}

static bool setup(void)
{
#if defined(_WIN32)
    s_lib = SDL_LoadObject("vulkan-1.dll");
#else
    s_lib = SDL_LoadObject("libvulkan.so");
    if (!s_lib) s_lib = SDL_LoadObject("libvulkan.so.1");
#endif
    if (!s_lib) { hlog("Vulkan: no loader\n"); return false; }
    gipa = (PFN_vkGetInstanceProcAddr)SDL_LoadFunction(s_lib, "vkGetInstanceProcAddr");
    if (!gipa) { hlog("Vulkan: the loader has no vkGetInstanceProcAddr\n"); return false; }
    auto create_instance = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create_instance) { hlog("Vulkan: no vkCreateInstance\n"); return false; }
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "lr-host", 1, "lr-host", 1,
                             VK_API_VERSION_1_0};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    if (create_instance(&ici, NULL, &s_instance) != VK_SUCCESS) { hlog("Vulkan: no instance\n"); return false; }

#define IFN(name) auto name = (PFN_##name)gipa(s_instance, #name)
    IFN(vkEnumeratePhysicalDevices);
    IFN(vkGetPhysicalDeviceQueueFamilyProperties);
    IFN(vkGetPhysicalDeviceProperties);
    IFN(vkCreateDevice);
    IFN(vkGetDeviceQueue);
#undef IFN
    gdpa = (PFN_vkGetDeviceProcAddr)gipa(s_instance, "vkGetDeviceProcAddr");
    vkGetPhysicalDeviceMemoryProperties_ =
        (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(s_instance, "vkGetPhysicalDeviceMemoryProperties");
    vkDestroyInstance_ = (PFN_vkDestroyInstance)gipa(s_instance, "vkDestroyInstance");

    uint32_t n = 1;
    if (vkEnumeratePhysicalDevices(s_instance, &n, &s_gpu) < 0 || n == 0) { hlog("Vulkan: no GPU\n"); return false; }
    uint32_t nf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(s_gpu, &nf, NULL);
    std::vector<VkQueueFamilyProperties> fam(nf);
    vkGetPhysicalDeviceQueueFamilyProperties(s_gpu, &nf, fam.data());
    s_family = UINT32_MAX;
    for (uint32_t i = 0; i < nf; ++i)
        if ((fam[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { s_family = i; break; }
    if (s_family == UINT32_MAX) { hlog("Vulkan: no graphics+compute queue\n"); return false; }

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = s_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (vkCreateDevice(s_gpu, &dci, NULL, &s_device) != VK_SUCCESS) { hlog("Vulkan: no device\n"); return false; }
    vkGetDeviceQueue(s_device, s_family, 0, &s_queue);

#define DFN(name) name##_ = (PFN_##name)gdpa(s_device, #name)
    DFN(vkQueueWaitIdle); DFN(vkCreateCommandPool); DFN(vkAllocateCommandBuffers); DFN(vkBeginCommandBuffer);
    DFN(vkEndCommandBuffer); DFN(vkCmdPipelineBarrier); DFN(vkCmdCopyImageToBuffer); DFN(vkCreateBuffer);
    DFN(vkGetBufferMemoryRequirements); DFN(vkAllocateMemory); DFN(vkBindBufferMemory); DFN(vkMapMemory);
    DFN(vkQueueSubmit); DFN(vkDestroyDevice); DFN(vkDeviceWaitIdle); DFN(vkUnmapMemory); DFN(vkDestroyBuffer);
    DFN(vkFreeMemory); DFN(vkDestroyCommandPool);
#undef DFN

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(s_gpu, &props);
    hlog("Vulkan: %s\n", props.deviceName);

    s_iface.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
    s_iface.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
    s_iface.handle = &s_iface;
    s_iface.instance = s_instance;
    s_iface.gpu = s_gpu;
    s_iface.device = s_device;
    s_iface.get_device_proc_addr = gdpa;
    s_iface.get_instance_proc_addr = gipa;
    s_iface.queue = s_queue;
    s_iface.queue_index = s_family;
    s_iface.set_image = set_image;
    s_iface.get_sync_index = get_sync_index;
    s_iface.get_sync_index_mask = get_sync_index_mask;
    s_iface.set_command_buffers = set_command_buffers;
    s_iface.wait_sync_index = wait_sync_index;
    s_iface.lock_queue = lock_queue;
    s_iface.unlock_queue = unlock_queue;
    s_iface.set_signal_semaphore = set_signal_semaphore;
    return true;
}

// After each retro_run: like a frontend's present, wait for the GPU.
static void end_frame(void)
{
    vkQueueWaitIdle_(s_queue);
    ++s_frame;
}

// Copies the last presented image into 'fb' (XRGB8888, w x h).
static bool readback(std::vector<uint32_t> &fb, int w, int h)
{
    if (!s_have_image || w <= 0 || h <= 0) return false;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    // frees whatever was made, on every way out
    auto done = [&](bool ok) {
        if (buf) vkDestroyBuffer_(s_device, buf, NULL);
        if (mem) vkFreeMemory_(s_device, mem, NULL);
        if (pool) vkDestroyCommandPool_(s_device, pool, NULL);
        return ok;
    };

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = s_family;
    if (vkCreateCommandPool_(s_device, &pci, NULL, &pool) != VK_SUCCESS) return done(false);
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers_(s_device, &cai, &cmd) != VK_SUCCESS) return done(false);

    const VkDeviceSize size = (VkDeviceSize)w * h * 4;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer_(s_device, &bci, NULL, &buf) != VK_SUCCESS) return done(false);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements_(s_device, buf, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties_(s_gpu, &mp);
    uint32_t type = UINT32_MAX;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) { type = i; break; }
    if (type == UINT32_MAX) { hlog("Vulkan: no host-visible memory for the readback\n"); return done(false); }
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory_(s_device, &mai, NULL, &mem) != VK_SUCCESS ||
        vkBindBufferMemory_(s_device, buf, mem, 0) != VK_SUCCESS)
        return done(false);

    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (vkBeginCommandBuffer_(cmd, &bi) != VK_SUCCESS) return done(false);
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = s_image.image_layout;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = s_image.create_info.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier_(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                          NULL, 1, &b);
    VkBufferImageCopy c = {};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyImageToBuffer_(cmd, b.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = s_image.image_layout;
    vkCmdPipelineBarrier_(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                          NULL, 1, &b);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (vkEndCommandBuffer_(cmd) != VK_SUCCESS || vkQueueSubmit_(s_queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS ||
        vkQueueWaitIdle_(s_queue) != VK_SUCCESS)
        return done(false);

    uint8_t *p = NULL;
    if (vkMapMemory_(s_device, mem, 0, VK_WHOLE_SIZE, 0, (void **)&p) != VK_SUCCESS) return done(false);
    fb.resize((size_t)w * h);
    for (size_t i = 0; i < fb.size(); ++i, p += 4) fb[i] = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    vkUnmapMemory_(s_device, mem);
    return done(true);
}

static void shutdown(void)
{
    if (s_device) { vkDeviceWaitIdle_(s_device); vkDestroyDevice_(s_device, NULL); }
    if (s_instance) vkDestroyInstance_(s_instance, NULL);
    if (s_lib) SDL_UnloadObject(s_lib);
    s_device = VK_NULL_HANDLE;
    s_instance = VK_NULL_HANDLE;
    s_lib = NULL;
}

} // namespace hostvk
