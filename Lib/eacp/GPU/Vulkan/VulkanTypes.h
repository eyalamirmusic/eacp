#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

// The concrete shapes behind the GPU layer's opaque void* native handles on the
// Vulkan backend. Vulkan has no single object standing for "a texture" -- an
// image, its view, its memory and its *current layout* are four separate things
// that must travel together -- so each handle points at one of these instead.
// The D3D12 backend needs the same treatment for its resource states; see
// D3D12Types.h. Not part of GPU.h.

namespace eacp::GPU
{
struct VulkanBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize bytes = 0;

    // Buffers are host-visible and permanently mapped. On Apple the memory is
    // unified so this costs nothing, and it makes update()/read() a memcpy. A
    // discrete GPU would rather have device-local memory with staging copies;
    // that is a performance change, not a correctness one.
    void* mapped = nullptr;
};

struct VulkanTexture
{
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    int width = 0;
    int height = 0;

    // Tracked rather than assumed: every barrier needs the layout the image is
    // actually in, and a texture crosses several (undefined on creation,
    // transfer-dst while uploading, shader-read while sampled).
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct VulkanShaderProgram
{
    VkShaderModule module = VK_NULL_HANDLE;
};

struct VulkanPipeline
{
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::uint32_t pushConstantBytes = 0;
};

// What Frame hands a RenderPass so the pass can record into the right target.
struct VulkanRenderTarget
{
    VkCommandBuffer list = VK_NULL_HANDLE;
    int width = 0;
    int height = 0;
};
} // namespace eacp::GPU
