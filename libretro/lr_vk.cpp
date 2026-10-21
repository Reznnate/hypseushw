/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 HypseusHW contributors
 *
 * Vulkan renderer: a compute shader converts the laserdisc frame from YUV
 * and blends the game overlay over it into an image the frontend presents
 * (libretro hardware rendering with RETRO_HW_CONTEXT_VULKAN).  The shader
 * (shaders/compose.comp) uses the CPU renderer's integer arithmetic, so the
 * output is the same as the CPU and GL renderers'.
 *
 * Everything the GPU reads or writes is kept per frontend sync index (one
 * set per frame in flight), so nothing is overwritten while the frontend
 * may still use it.  For the same reason frames are never reported as
 * repeats here: a repeat keeps the frontend reading an older index's image
 * past the point where that index is reused.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "lr_vk.h"
#include "lr_gpu_verify.h"

#include <libretro_vulkan.h>
#include <string.h>

#include "video/video.h"

static const uint32_t SPIRV[] =
#include "shaders/compose.comp.inc"
    ;

static retro_hw_render_callback g_hw;
static retro_environment_t g_environ;
static lr_vk_log_fn g_log;
static const retro_hw_render_interface_vulkan *g_vk;
static bool g_ready;

#define LOG(level, ...) do { if (g_log) g_log(level, __VA_ARGS__); } while (0)
// For failures that would otherwise repeat every frame: once per context.
static bool g_logged_fit, g_logged_index;
#define LOG_ONCE(flag, level, ...) do { if (!flag) { flag = true; LOG(level, __VA_ARGS__); } } while (0)

// ---------------------------------------------------------------------------
// Function table, loaded from the frontend's device
// ---------------------------------------------------------------------------

#define VK_DEVICE_FUNCS(X)                                                                   \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory)  \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateBuffer) X(vkDestroyBuffer)          \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkAllocateMemory)               \
    X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCreateSampler) X(vkDestroySampler)   \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) X(vkCreateShaderModule) X(vkDestroyShaderModule)              \
    X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCreateDescriptorPool)               \
    X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets)         \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers)               \
    X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer)                    \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer)              \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch)   \
    X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkDeviceWaitIdle)

#define VK_DECLARE(name) static PFN_##name name##_;
VK_DEVICE_FUNCS(VK_DECLARE)
#undef VK_DECLARE
static PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties_;

// All entries are cleared first, so after a failure the ones not reached are
// NULL rather than left over from an earlier device; vkDeviceWaitIdle comes
// last, so it being set means the whole table is.
static bool load_functions(void)
{
#define VK_CLEAR(name) name##_ = NULL;
    VK_DEVICE_FUNCS(VK_CLEAR)
#undef VK_CLEAR
#define VK_LOAD(name)                                                                      \
    name##_ = (PFN_##name)g_vk->get_device_proc_addr(g_vk->device, #name);                   \
    if (!name##_) { LOG(RETRO_LOG_ERROR, "Vulkan renderer: no %s", #name); return false; }
    VK_DEVICE_FUNCS(VK_LOAD)
#undef VK_LOAD
    vkGetPhysicalDeviceMemoryProperties_ = (PFN_vkGetPhysicalDeviceMemoryProperties)
        g_vk->get_instance_proc_addr(g_vk->instance, "vkGetPhysicalDeviceMemoryProperties");
    return vkGetPhysicalDeviceMemoryProperties_ != NULL;
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

struct Tex {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageViewCreateInfo view_info = {};
    int w = 0, h = 0;
    bool has_layout = false;   // has been moved out of UNDEFINED
};

// One set per frontend sync index.
struct Slot {
    Tex y, u, v, ov, out;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    uint8_t *staging_map = NULL;
    size_t staging_size = 0;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    bool set_dirty = true;
    uint64_t video_gen = 0, ov_gen = 0;   // what the textures hold
    bool video_valid = false;
};

static const int MAX_SLOTS = 16;
static Slot g_slots[MAX_SLOTS];
static VkSampler g_sampler;
static VkDescriptorSetLayout g_set_layout;
static VkPipelineLayout g_pipe_layout;
static VkPipeline g_pipeline;
static VkDescriptorPool g_pool;
static VkCommandPool g_cmd_pool;
static uint64_t g_video_gen = 1, g_ov_gen = 1;  // bumped when the source changes

// DAPHNE_LR_VERIFY_GPU readback buffer (see lr_gpu_verify.h)
static lr_gpu_verify g_verify;
static VkBuffer g_rb_buf;
static VkDeviceMemory g_rb_mem;
static uint8_t *g_rb_map;
static size_t g_rb_size;

struct Push {
    int32_t size[2];
    int32_t has_video, has_ov;
    int32_t lr[4];
};

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties p;
    vkGetPhysicalDeviceMemoryProperties_(g_vk->gpu, &p);
    for (uint32_t i = 0; i < p.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static void destroy_tex(Tex &t)
{
    if (t.view) vkDestroyImageView_(g_vk->device, t.view, NULL);
    if (t.image) vkDestroyImage_(g_vk->device, t.image, NULL);
    if (t.mem) vkFreeMemory_(g_vk->device, t.mem, NULL);
    t = Tex();
}

static bool create_tex(Tex &t, int w, int h, VkFormat fmt, VkImageUsageFlags usage)
{
    destroy_tex(t);
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {(uint32_t)w, (uint32_t)h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage_(g_vk->device, &ci, NULL, &t.image) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements_(g_vk->device, t.image, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = memory_type(req.memoryTypeBits, 0);
    if (ai.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory_(g_vk->device, &ai, NULL, &t.mem) != VK_SUCCESS ||
        vkBindImageMemory_(g_vk->device, t.image, t.mem, 0) != VK_SUCCESS)
        return false;

    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
                     VK_COMPONENT_SWIZZLE_A};
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView_(g_vk->device, &vi, NULL, &t.view) != VK_SUCCESS) return false;
    t.view_info = vi;
    t.w = w;
    t.h = h;
    return true;
}

// Host-visible buffer, mapped.  'cached' asks for CPU-cached memory, for
// buffers the CPU reads (uncached reads are very slow).
static bool create_buffer(size_t size, VkBufferUsageFlags usage, VkBuffer *buf, VkDeviceMemory *mem,
                          uint8_t **map, bool cached = false)
{
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer_(g_vk->device, &bi, NULL, buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements_(g_vk->device, *buf, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    ai.memoryTypeIndex = cached ? memory_type(req.memoryTypeBits, host | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
                                : UINT32_MAX;
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = memory_type(req.memoryTypeBits, host);
    if (ai.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory_(g_vk->device, &ai, NULL, mem) != VK_SUCCESS ||
        vkBindBufferMemory_(g_vk->device, *buf, *mem, 0) != VK_SUCCESS)
        return false;
    return vkMapMemory_(g_vk->device, *mem, 0, VK_WHOLE_SIZE, 0, (void **)map) == VK_SUCCESS;
}

static void destroy_buffer(VkBuffer &buf, VkDeviceMemory &mem, uint8_t *&map)
{
    if (map) vkUnmapMemory_(g_vk->device, mem);
    if (buf) vkDestroyBuffer_(g_vk->device, buf, NULL);
    if (mem) vkFreeMemory_(g_vk->device, mem, NULL);
    buf = VK_NULL_HANDLE;
    mem = VK_NULL_HANDLE;
    map = NULL;
}

static void destroy_slot(Slot &s)
{
    destroy_tex(s.y);
    destroy_tex(s.u);
    destroy_tex(s.v);
    destroy_tex(s.ov);
    destroy_tex(s.out);
    destroy_buffer(s.staging, s.staging_mem, s.staging_map);
    VkCommandBuffer cmd = s.cmd;
    VkDescriptorSet set = s.set;
    s = Slot();
    s.cmd = cmd;   // freed with their pools
    s.set = set;
}

static void destroy_all(void)
{
    // nothing was created when the function table did not load
    if (!g_vk || !vkDeviceWaitIdle_) return;
    vkDeviceWaitIdle_(g_vk->device);
    for (Slot &s : g_slots)
    {
        destroy_slot(s);
        s.cmd = VK_NULL_HANDLE;
        s.set = VK_NULL_HANDLE;
    }
    destroy_buffer(g_rb_buf, g_rb_mem, g_rb_map);
    g_rb_size = 0;
    if (g_cmd_pool) vkDestroyCommandPool_(g_vk->device, g_cmd_pool, NULL);
    if (g_pool) vkDestroyDescriptorPool_(g_vk->device, g_pool, NULL);
    if (g_pipeline) vkDestroyPipeline_(g_vk->device, g_pipeline, NULL);
    if (g_pipe_layout) vkDestroyPipelineLayout_(g_vk->device, g_pipe_layout, NULL);
    if (g_set_layout) vkDestroyDescriptorSetLayout_(g_vk->device, g_set_layout, NULL);
    if (g_sampler) vkDestroySampler_(g_vk->device, g_sampler, NULL);
    g_cmd_pool = VK_NULL_HANDLE;
    g_pool = VK_NULL_HANDLE;
    g_pipeline = VK_NULL_HANDLE;
    g_pipe_layout = VK_NULL_HANDLE;
    g_set_layout = VK_NULL_HANDLE;
    g_sampler = VK_NULL_HANDLE;
}

static bool create_pipeline(void)
{
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    if (vkCreateSampler_(g_vk->device, &si, NULL, &g_sampler) != VK_SUCCESS) return false;

    VkDescriptorSetLayoutBinding b[5] = {};
    for (int i = 0; i < 4; ++i)
    {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    b[4].binding = 4;
    b[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[4].descriptorCount = 1;
    b[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 5;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout_(g_vk->device, &li, NULL, &g_set_layout) != VK_SUCCESS) return false;

    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &g_set_layout;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout_(g_vk->device, &pi, NULL, &g_pipe_layout) != VK_SUCCESS) return false;

    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof(SPIRV);
    mi.pCode = SPIRV;
    VkShaderModule mod;
    if (vkCreateShaderModule_(g_vk->device, &mi, NULL, &mod) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = mod;
    ci.stage.pName = "main";
    ci.layout = g_pipe_layout;
    const VkResult r = vkCreateComputePipelines_(g_vk->device, VK_NULL_HANDLE, 1, &ci, NULL, &g_pipeline);
    vkDestroyShaderModule_(g_vk->device, mod, NULL);
    if (r != VK_SUCCESS) return false;

    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * MAX_SLOTS},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, MAX_SLOTS}};
    VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = MAX_SLOTS;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = sizes;
    if (vkCreateDescriptorPool_(g_vk->device, &dp, NULL, &g_pool) != VK_SUCCESS) return false;

    VkCommandPoolCreateInfo cp = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = g_vk->queue_index;
    if (vkCreateCommandPool_(g_vk->device, &cp, NULL, &g_cmd_pool) != VK_SUCCESS) return false;

    for (Slot &s : g_slots)
    {
        VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = g_pool;
        da.descriptorSetCount = 1;
        da.pSetLayouts = &g_set_layout;
        if (vkAllocateDescriptorSets_(g_vk->device, &da, &s.set) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = g_cmd_pool;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 1;
        if (vkAllocateCommandBuffers_(g_vk->device, &ca, &s.cmd) != VK_SUCCESS) return false;
    }
    return true;
}

static void context_reset(void)
{
    g_ready = false;
    g_vk = NULL;
    if (!g_environ(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, (void *)&g_vk) || !g_vk ||
        g_vk->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN ||
        g_vk->interface_version != RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION)
    {
        LOG(RETRO_LOG_ERROR, "Vulkan renderer: the frontend's Vulkan interface is missing or of another "
            "version; using the CPU renderer");
        g_vk = NULL;
        return;
    }
    if (!load_functions() || !create_pipeline())
    {
        LOG(RETRO_LOG_ERROR, "Vulkan renderer: setup failed; using the CPU renderer");
        destroy_all();
        g_vk = NULL;
        return;
    }
    // new textures: everything is uploaded again
    ++g_video_gen;
    ++g_ov_gen;
    video::lr_frame_invalidate();
    g_logged_fit = g_logged_index = false;
    g_ready = true;
    unsigned frames = 0;
    for (uint32_t m = g_vk->get_sync_index_mask(g_vk->handle); m; m &= m - 1) ++frames;
    LOG(RETRO_LOG_INFO, "Vulkan renderer ready (%u frames in flight)", frames);
}

// After a failed command buffer or submit (a lost device, say) nothing more
// is sent: the CPU renderer takes over until the frontend resets the context.
static bool give_up(const char *what, VkResult r)
{
    LOG(RETRO_LOG_ERROR, "Vulkan renderer: %s failed (%d); using the CPU renderer", what, (int)r);
    g_ready = false;
    return false;
}

static void context_destroy(void)
{
    destroy_all();
    g_ready = false;
    g_vk = NULL;
}

bool lr_vk_request(retro_environment_t environ_cb, lr_vk_log_fn log)
{
    g_environ = environ_cb;
    g_log = log;
    g_ready = false;
    lr_gpu_verify_start(g_verify);

    memset(&g_hw, 0, sizeof(g_hw));
    g_hw.context_type    = RETRO_HW_CONTEXT_VULKAN;
    g_hw.version_major   = VK_API_VERSION_1_0;
    g_hw.context_reset   = context_reset;
    g_hw.context_destroy = context_destroy;
    return environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &g_hw);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

static void barrier(VkCommandBuffer cmd, Tex &t, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                    VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier_(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

// Copies 'rows' rows of 'row_bytes' from src (pitch 'pitch') to dst, packed.
static size_t pack(uint8_t *dst, const uint8_t *src, int pitch, int row_bytes, int rows)
{
    for (int r = 0; r < rows; ++r) memcpy(dst + (size_t)r * row_bytes, src + (size_t)r * pitch, row_bytes);
    return (size_t)row_bytes * rows;
}

static size_t align_up(size_t v) { return (v + 255) & ~(size_t)255; }

// Records the copy of 'bytes' at 'offset' of the staging buffer into 't'.
static void upload(VkCommandBuffer cmd, Slot &s, Tex &t, size_t offset)
{
    barrier(cmd, t, t.has_layout ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy c = {};
    c.bufferOffset = offset;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {(uint32_t)t.w, (uint32_t)t.h, 1};
    vkCmdCopyBufferToImage_(cmd, s.staging, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(cmd, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    t.has_layout = true;
}

// Textures that have never been written still need a valid layout for the
// descriptor set (the shader does not read them).
static void settle(VkCommandBuffer cmd, Tex &t)
{
    if (t.has_layout) return;
    barrier(cmd, t, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    t.has_layout = true;
}

// Makes the slot's images match the frame's sizes.  The slot is idle here.
// On failure the images involved are dropped and the descriptor set marked
// stale, so the next frame starts over instead of using freed views.
static bool fit_slot(Slot &s, const video::lr_frame &f)
{
    const VkImageUsageFlags tex_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const int cw = f.w / 2 > 0 ? f.w / 2 : 1, ch = f.h / 2 > 0 ? f.h / 2 : 1;
    const int ow = f.ov ? f.ov_w : 1, oh = f.ov ? f.ov_h : 1;
    bool changed = false;

    if (s.y.w != f.w || s.y.h != f.h)
    {
        if (!create_tex(s.y, f.w, f.h, VK_FORMAT_R8_UNORM, tex_usage) ||
            !create_tex(s.u, cw, ch, VK_FORMAT_R8_UNORM, tex_usage) ||
            !create_tex(s.v, cw, ch, VK_FORMAT_R8_UNORM, tex_usage) ||
            !create_tex(s.out, f.w, f.h, VK_FORMAT_R8G8B8A8_UNORM,
                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        {
            destroy_tex(s.y);
            destroy_tex(s.u);
            destroy_tex(s.v);
            destroy_tex(s.out);
            s.set_dirty = true;
            return false;
        }
        s.video_gen = 0;
        s.video_valid = false;
        changed = true;
    }
    if (s.ov.w != ow || s.ov.h != oh)
    {
        if (!create_tex(s.ov, ow, oh, VK_FORMAT_R8G8B8A8_UNORM, tex_usage))
        {
            destroy_tex(s.ov);
            s.set_dirty = true;
            return false;
        }
        s.ov_gen = 0;
        changed = true;
    }

    const size_t need = align_up((size_t)f.w * f.h) + 2 * align_up((size_t)cw * ch) + align_up((size_t)ow * oh * 4);
    if (s.staging_size < need)
    {
        destroy_buffer(s.staging, s.staging_mem, s.staging_map);
        if (!create_buffer(need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &s.staging, &s.staging_mem, &s.staging_map))
            return false;
        s.staging_size = need;
    }

    if (changed || s.set_dirty)
    {
        VkDescriptorImageInfo img[5] = {
            {g_sampler, s.y.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {g_sampler, s.u.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {g_sampler, s.v.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {g_sampler, s.ov.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {VK_NULL_HANDLE, s.out.view, VK_IMAGE_LAYOUT_GENERAL},
        };
        VkWriteDescriptorSet w[5] = {};
        for (int i = 0; i < 5; ++i)
        {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = s.set;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = i < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[i].pImageInfo = &img[i];
        }
        vkUpdateDescriptorSets_(g_vk->device, 5, w, 0, NULL);
        s.set_dirty = false;
    }
    return true;
}

bool lr_vk_render(const video::lr_frame &f)
{
    if (!g_ready || f.w <= 0 || f.h <= 0) return false;

    if (f.video_changed) ++g_video_gen;
    if (f.ov_changed) ++g_ov_gen;

    g_vk->wait_sync_index(g_vk->handle);
    const uint32_t index = g_vk->get_sync_index(g_vk->handle);
    if (index >= (uint32_t)MAX_SLOTS)
    {
        LOG_ONCE(g_logged_index, RETRO_LOG_ERROR,
                 "Vulkan renderer: frontend sync index %u is beyond the %d supported; using the CPU renderer",
                 (unsigned)index, MAX_SLOTS);
        return false;
    }
    Slot &s = g_slots[index];
    if (!fit_slot(s, f))
    {
        LOG_ONCE(g_logged_fit, RETRO_LOG_ERROR,
                 "Vulkan renderer: cannot create the %dx%d frame's images; using the CPU renderer", f.w, f.h);
        return false;
    }

    VkCommandBuffer cmd = s.cmd;
    VkResult r = vkResetCommandBuffer_(cmd, 0);
    if (r != VK_SUCCESS) return give_up("vkResetCommandBuffer", r);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    r = vkBeginCommandBuffer_(cmd, &bi);
    if (r != VK_SUCCESS) return give_up("vkBeginCommandBuffer", r);

    // Uploads: this slot's textures are brought up to date when the source
    // differs from what they hold (and the planes are at hand).
    const int cw = s.u.w, ch = s.u.h;
    size_t off = 0;
    if (s.video_gen != g_video_gen && f.y)
    {
        const size_t oy = off;
        off += align_up(pack(s.staging_map + oy, f.y, f.ypitch, f.w, f.h));
        const size_t ou = off;
        off += align_up(pack(s.staging_map + ou, f.u, f.upitch, cw, ch));
        const size_t ov = off;
        off += align_up(pack(s.staging_map + ov, f.v, f.vpitch, cw, ch));
        upload(cmd, s, s.y, oy);
        upload(cmd, s, s.u, ou);
        upload(cmd, s, s.v, ov);
        s.video_gen = g_video_gen;
        s.video_valid = true;
    }
    if (f.ov && s.ov_gen != g_ov_gen)
    {
        const size_t oo = off;
        off += align_up(pack(s.staging_map + oo, f.ov, f.ov_pitch, f.ov_w * 4, f.ov_h));
        upload(cmd, s, s.ov, oo);
        s.ov_gen = g_ov_gen;
    }
    settle(cmd, s.y);
    settle(cmd, s.u);
    settle(cmd, s.v);
    settle(cmd, s.ov);

    // The frontend finished with this slot's output (wait_sync_index), and
    // it is rewritten in full: its old contents can go.
    barrier(cmd, s.out, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vkCmdBindPipeline_(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipeline);
    vkCmdBindDescriptorSets_(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipe_layout, 0, 1, &s.set, 0, NULL);
    Push p = {{f.w, f.h}, f.has_video && s.video_valid ? 1 : 0, f.ov ? 1 : 0, {f.lr.x, f.lr.y, f.lr.w, f.lr.h}};
    vkCmdPushConstants_(cmd, g_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch_(cmd, (uint32_t)(f.w + 15) / 16, (uint32_t)(f.h + 15) / 16, 1);

    VkImageLayout final_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (g_verify.enabled)
    {
        const size_t need = (size_t)f.w * f.h * 4;
        if (g_rb_size < need)
        {
            destroy_buffer(g_rb_buf, g_rb_mem, g_rb_map);
            g_rb_size = create_buffer(need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &g_rb_buf, &g_rb_mem, &g_rb_map, true)
                            ? need : 0;
        }
        if (g_rb_size)
        {
            barrier(cmd, s.out, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy c = {};
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageExtent = {(uint32_t)f.w, (uint32_t)f.h, 1};
            vkCmdCopyImageToBuffer_(cmd, s.out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_rb_buf, 1, &c);
            barrier(cmd, s.out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, final_layout, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
        else
            barrier(cmd, s.out, VK_IMAGE_LAYOUT_GENERAL, final_layout, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    else
        barrier(cmd, s.out, VK_IMAGE_LAYOUT_GENERAL, final_layout, VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    r = vkEndCommandBuffer_(cmd);
    if (r != VK_SUCCESS) return give_up("vkEndCommandBuffer", r);

    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    const bool readback = g_verify.enabled && g_rb_size;
    g_vk->lock_queue(g_vk->handle);
    r = vkQueueSubmit_(g_vk->queue, 1, &si, VK_NULL_HANDLE);
    if (r == VK_SUCCESS && readback) r = vkQueueWaitIdle_(g_vk->queue);
    g_vk->unlock_queue(g_vk->handle);
    if (r != VK_SUCCESS) return give_up("vkQueueSubmit", r);
    if (readback) lr_gpu_verify_frame(g_verify, f, g_rb_map, false, "Vulkan renderer", g_log);

    retro_vulkan_image img = {};
    img.image_view = s.out.view;
    img.image_layout = final_layout;
    img.create_info = s.out.view_info;
    g_vk->set_image(g_vk->handle, &img, 0, NULL, VK_QUEUE_FAMILY_IGNORED);
    return true;
}

void lr_vk_unload(void)
{
    lr_gpu_verify_report(g_verify, "Vulkan renderer", g_log);
}
