#include "Texture.h"

#include "../Device/Device.h"
#include "../Vulkan/VulkanContext.h"
#include "../Vulkan/VulkanTypes.h"
#include "TextureImport-Vulkan.h"

#include <cstring>

namespace eacp::GPU
{
namespace
{
VkFormat toVulkan(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::RGBA8Unorm:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case TextureFormat::BGRA8Unorm:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case TextureFormat::R8Unorm:
            return VK_FORMAT_R8_UNORM;
    }

    return VK_FORMAT_R8G8B8A8_UNORM;
}
} // namespace

// Moves an image between layouts. Every upload and every sample needs one, and
// getting the stage masks wrong is the classic source of a texture that reads
// back as garbage on one driver and fine on another, so the transitions are all
// funnelled through here rather than written out at each call site.
void transitionImage(VkCommandBuffer list,
                     VulkanTexture& texture,
                     VkImageLayout target)
{
    if (texture.layout == target)
        return;

    auto barrier =
        makeVulkanInfo<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    barrier.oldLayout = texture.layout;
    barrier.newLayout = target;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    auto sourceStage = VkPipelineStageFlags {VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT};
    auto destinationStage = VkPipelineStageFlags {VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT};

    switch (texture.layout)
    {
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            sourceStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            sourceStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            break;
        default:
            break;
    }

    switch (target)
    {
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            destinationStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            break;
        default:
            break;
    }

    vkCmdPipelineBarrier(
        list, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    texture.layout = target;
}

struct Texture::Native
{
    Native(const TextureDescriptor& descriptor, const void* pixels)
    {
        auto& context = getVulkanContext();

        if (!context.isValid() || descriptor.width <= 0 || descriptor.height <= 0)
            return;

        texture.format = toVulkan(descriptor.format);
        texture.width = descriptor.width;
        texture.height = descriptor.height;
        bytesPerPixelValue = bytesPerPixel(descriptor.format);

        auto info =
            makeVulkanInfo<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = texture.format;
        info.extent = {static_cast<std::uint32_t>(descriptor.width),
                       static_cast<std::uint32_t>(descriptor.height),
                       1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                     | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        auto* device = context.getDevice();

        if (vkCreateImage(device, &info, nullptr, &texture.image) != VK_SUCCESS)
            return;

        auto allocation =
            context.allocateFor(texture.image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (allocation.memory == VK_NULL_HANDLE)
        {
            vkDestroyImage(device, texture.image, nullptr);
            texture.image = VK_NULL_HANDLE;
            return;
        }

        texture.memory = allocation.memory;

        if (!createView())
            return;

        if (pixels != nullptr)
            upload({0.f,
                    0.f,
                    static_cast<float>(descriptor.width),
                    static_cast<float>(descriptor.height)},
                   pixels,
                   0);
        else
            moveToShaderRead();
    }

    // The zero-copy path. The image arrives already backed by the platform's
    // shared surface and already in the layout a sample expects, so there is no
    // allocation, nothing to upload and - importantly for the camera, which
    // wraps a fresh buffer every frame - no transition submit to block on.
    explicit Native(void* pixelBuffer)
    {
        // Transfer usage alongside sampled so update() keeps working on a
        // wrapped buffer, as it does on Metal.
        texture = detail::importPixelBuffer(pixelBuffer,
                                            VK_IMAGE_USAGE_SAMPLED_BIT
                                                | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                                                | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

        if (texture.image != VK_NULL_HANDLE)
            createView();
    }

    ~Native()
    {
        // memory is null for an imported image - the platform surface is the
        // allocation, and the driver releases it with the image.
        if (texture.image != VK_NULL_HANDLE)
            getVulkanContext().deferDestroy(
                texture.image, texture.view, texture.memory);
    }

    bool createView()
    {
        auto info = makeVulkanInfo<VkImageViewCreateInfo>(
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
        info.image = texture.image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = texture.format;
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.layerCount = 1;

        return vkCreateImageView(
                   getVulkanContext().getDevice(), &info, nullptr, &texture.view)
               == VK_SUCCESS;
    }

    // An empty texture still has to reach shader-read layout, or the first draw
    // that samples it binds an image in UNDEFINED and reads undefined data.
    void moveToShaderRead()
    {
        auto& context = getVulkanContext();
        auto* commands = context.acquire();

        if (commands == nullptr)
            return;

        transitionImage(
            commands->list, texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        context.waitFor(context.submit(commands));
    }

    void upload(const Graphics::Rect& region,
                const void* pixels,
                std::size_t bytesPerRow)
    {
        auto& context = getVulkanContext();

        if (!context.isValid() || texture.image == VK_NULL_HANDLE
            || pixels == nullptr)
            return;

        auto regionWidth = static_cast<int>(region.w);
        auto regionHeight = static_cast<int>(region.h);

        if (regionWidth <= 0 || regionHeight <= 0)
            return;

        auto packedRow = static_cast<std::size_t>(regionWidth) * bytesPerPixelValue;
        auto sourceRow = bytesPerRow > 0 ? bytesPerRow : packedRow;
        auto total = sourceRow * static_cast<std::size_t>(regionHeight);

        auto* commands = context.acquire();

        if (commands == nullptr)
            return;

        auto staging = context.makeStagingBuffer(*commands, pixels, total);

        if (staging == VK_NULL_HANDLE)
        {
            context.discard(commands);
            return;
        }

        transitionImage(
            commands->list, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        auto copy = VkBufferImageCopy {};
        copy.bufferRowLength =
            static_cast<std::uint32_t>(sourceRow / bytesPerPixelValue);
        copy.bufferImageHeight = static_cast<std::uint32_t>(regionHeight);
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageOffset = {static_cast<std::int32_t>(region.x),
                            static_cast<std::int32_t>(region.y),
                            0};
        copy.imageExtent = {static_cast<std::uint32_t>(regionWidth),
                            static_cast<std::uint32_t>(regionHeight),
                            1};

        vkCmdCopyBufferToImage(commands->list,
                               staging,
                               texture.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1,
                               &copy);

        transitionImage(
            commands->list, texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        context.waitFor(context.submit(commands));
    }

    VulkanTexture texture;
    int bytesPerPixelValue = 4;
};

Texture::Texture(Device&, const TextureDescriptor& descriptor, const void* pixels)
    : impl(descriptor, pixels)
{
}

Texture::Texture(Device&, void* pixelBuffer)
    : impl(pixelBuffer)
{
}

int Texture::width() const
{
    return impl->texture.width;
}

int Texture::height() const
{
    return impl->texture.height;
}

bool Texture::isValid() const
{
    return impl->texture.image != VK_NULL_HANDLE;
}

void Texture::update(const void* pixels, std::size_t bytesPerRow)
{
    impl->upload({0.f,
                  0.f,
                  static_cast<float>(impl->texture.width),
                  static_cast<float>(impl->texture.height)},
                 pixels,
                 bytesPerRow);
}

void Texture::update(const Graphics::Rect& region,
                     const void* pixels,
                     std::size_t bytesPerRow)
{
    // Deliberately not clamped: a clamped region keeps consuming source rows at
    // the original width and uploads skewed pixels. See the header.
    if (region.x < 0.f || region.y < 0.f
        || region.x + region.w > static_cast<float>(impl->texture.width)
        || region.y + region.h > static_cast<float>(impl->texture.height))
        return;

    impl->upload(region, pixels, bytesPerRow);
}

void* Texture::nativeTexture() const
{
    if (!isValid())
        return nullptr;

    return const_cast<VulkanTexture*>(&impl->texture);
}

void* Texture::nativeReadView() const
{
    return nativeTexture();
}
} // namespace eacp::GPU
