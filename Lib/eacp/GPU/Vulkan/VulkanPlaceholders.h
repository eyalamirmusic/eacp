#pragma once

#include <volk.h>

#include <vk_mem_alloc.h>

#include <cstdint>

namespace eacp::GPU
{
// What a descriptor slot holds when the shader declares it and nothing was
// bound there: every slot a pipeline statically uses has to hold a valid
// descriptor, and a slot left unwritten is undefined behaviour. Transparent
// black and zeros, made once per device and never written by anything eacp
// records; a shader that writes an unbound output writes here.
class VulkanPlaceholders
{
public:
    // Records and waits for one submission that clears everything and leaves
    // each image in the layout its descriptor names.
    bool create(VkDevice device,
                VmaAllocator allocator,
                VkQueue queue,
                std::uint32_t queueFamily,
                const VkPhysicalDeviceLimits& limits);

    void destroy(VkDevice device, VmaAllocator allocator);

    // `type` is the slot's declared descriptor type and `viewType` its
    // dimension; `sampler` goes into a combined image sampler.
    VkDescriptorImageInfo image(VkDescriptorType type,
                                VkImageViewType viewType,
                                VkSampler sampler) const;

    VkDescriptorBufferInfo storageBuffer() const;

    // For a UNIFORM_BUFFER_DYNAMIC binding, at dynamic offset zero.
    VkDescriptorBufferInfo uniformBuffer() const;

private:
    struct Image
    {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkImageView view = VK_NULL_HANDLE;
    };

    struct Buffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkDeviceSize size = 0;
    };

    bool clearAll(VkDevice device, VkQueue queue, std::uint32_t queueFamily);

    Image sampled2D;
    Image sampledCube;
    Image storage2D;

    Buffer storage;
    Buffer uniform;
};
} // namespace eacp::GPU
