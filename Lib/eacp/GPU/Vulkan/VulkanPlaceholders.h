#pragma once

#include <eacp/Core/Utils/Containers.h>

#include <volk.h>

#include <vk_mem_alloc.h>

#include <cstdint>
#include <mutex>
#include <optional>

namespace eacp::GPU
{
struct VulkanTextureBindings;

// The image a texture slot's SPIR-V declares: what a view bound there has to
// be for the shader to read it, as the module's OpTypeImage says.
struct VulkanImageShape
{
    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;

    // The declared storage format where the shader names one, a depth format
    // for a shadow sampler, and otherwise one of the declared component type.
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;

    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;

    bool operator==(const VulkanImageShape&) const = default;
};

// What a descriptor slot holds when the shader declares it and nothing was
// bound there: every slot a pipeline statically uses has to hold a valid
// descriptor, and a slot left unwritten is undefined behaviour. Zeros, made
// once per device and never written by anything eacp records; a shader that
// writes an unbound output writes here.
//
// The buffers are made with the device. An image is made the first time a
// module declares its shape - its view type, format and sample count - so
// a usampler2DArray slot gets a 2D-array R8G8B8A8_UINT image and a
// sampler2DShadow slot a D16 one. A shape the device cannot make (a
// samplerCubeArray without imageCubeArray, a multisampled storage image
// without shaderStorageImageMultisample) is logged and its slot left without
// a placeholder.
class VulkanPlaceholders
{
public:
    // Every submission that clears a placeholder takes this lock.
    explicit VulkanPlaceholders(std::mutex& queueMutexToUse)
        : queueMutex(queueMutexToUse)
    {
    }

    // Where the placeholders are made and how they are cleared.
    struct Target
    {
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        VmaAllocator allocator = nullptr;
        VkQueue queue = VK_NULL_HANDLE;
        std::uint32_t queueFamily = 0;
    };

    // Records and waits for one submission that clears the buffers.
    bool create(const Target& where,
                const VkPhysicalDeviceLimits& limits,
                const VkPhysicalDeviceFeatures& enabledFeatures);

    void destroy();

    // Gives every slot `bindings` declares the descriptor a write names when
    // nothing was bound there. A shape seen for the first time costs one
    // submission, waited on.
    void fill(VulkanTextureBindings& bindings,
              VkSampler sampler,
              VkSampler compareSampler);

    VkDescriptorBufferInfo storageBuffer() const;

    // For a UNIFORM_BUFFER_DYNAMIC binding, at dynamic offset zero.
    VkDescriptorBufferInfo uniformBuffer() const;

private:
    struct Image
    {
        VulkanImageShape shape;
        bool isStorage = false;
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    struct Buffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkDeviceSize size = 0;
    };

    // Nothing where the device cannot make the shape.
    std::optional<Image> findOrMakeImage(const VulkanImageShape& shape,
                                         bool isStorage);

    // The image made, or one with a null view where the device cannot make it.
    Image makeImage(const VulkanImageShape& shape, bool isStorage);

    bool canMake(const VulkanImageShape& shape, bool isStorage) const;
    bool createImageAndView(Image& image);
    bool clearImage(const Image& image);
    void destroyImage(Image& image) const;
    bool clearBuffers();
    void destroyBuffer(Buffer& buffer) const;

    template <typename Record>
    bool submitAndWait(const Record& record);

    std::mutex& queueMutex;
    Target target;
    bool canMakeCubeArrays = false;

    std::mutex imagesMutex;
    Vector<Image> images;

    Buffer storage;
    Buffer uniform;
};
} // namespace eacp::GPU
