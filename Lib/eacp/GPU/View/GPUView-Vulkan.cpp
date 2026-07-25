#include "GPUView.h"

#include "../Device/Device.h"
#include "../Frame/Frame.h"
#include "../Vulkan/VulkanContext.h"
#include "../Vulkan/VulkanTypes.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// Vulkan backend. Off-screen only: render() runs into images this view owns and
// the pixels come back as a Graphics::Image, which is exactly the seam
// View::renderToImage already uses, so the platform's own 2D layer composites
// the result unchanged. Nothing here presents -- see Frame-Vulkan.cpp.

namespace eacp::GPU
{
// Defined in Texture-Vulkan.cpp.
void transitionImage(VkCommandBuffer list,
                     VulkanTexture& texture,
                     VkImageLayout target);

namespace
{
// A render-target image, unlike the sampled ones Texture creates: it carries
// colour-attachment usage and stays in whatever layout the frame leaves it.
VulkanTexture makeTarget(int width, int height, VkFormat format, int samples)
{
    auto& context = getVulkanContext();
    auto* device = context.getDevice();

    auto texture = VulkanTexture {};
    texture.format = format;
    texture.width = width;
    texture.height = height;

    auto isDepth = format == VK_FORMAT_D32_SFLOAT;

    auto info = VkImageCreateInfo {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {
        static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = static_cast<VkSampleCountFlagBits>(samples);
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = isDepth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                         : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                               | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device, &info, nullptr, &texture.image) != VK_SUCCESS)
        return {};

    auto allocation =
        context.allocateFor(texture.image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (allocation.memory == VK_NULL_HANDLE)
    {
        vkDestroyImage(device, texture.image, nullptr);
        return {};
    }

    texture.memory = allocation.memory;

    auto viewInfo = VkImageViewCreateInfo {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = texture.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask =
        isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;

    vkCreateImageView(device, &viewInfo, nullptr, &texture.view);

    return texture;
}

void destroyTarget(VulkanTexture& texture)
{
    if (texture.image != VK_NULL_HANDLE)
        getVulkanContext().deferDestroy(texture.image, texture.view, texture.memory);

    texture = {};
}
} // namespace

struct GPUView::Native
{
    int sampleCount = 4;
    bool depthEnabled = false;
    bool continuous = false;
    int framesInFlight = 3;
};

GPUView::GPUView()
    : impl()
{
}

GPUView::~GPUView() = default;

int GPUView::sampleCount() const
{
    return impl->sampleCount;
}

void GPUView::setSampleCount(int count)
{
    impl->sampleCount = std::max(1, count);
}

void GPUView::setDepth(bool enabled)
{
    impl->depthEnabled = enabled;
}

bool GPUView::hasDepth() const
{
    return impl->depthEnabled;
}

void GPUView::setContinuous(bool continuous)
{
    // Recorded but inert: continuous mode is driven by a display link against a
    // swapchain, and this backend has neither. Off-screen rendering is on
    // demand by definition.
    impl->continuous = continuous;
}

bool GPUView::isContinuous() const
{
    return impl->continuous;
}

void GPUView::setFramesInFlight(int count)
{
    impl->framesInFlight = std::max(1, count);
}

int GPUView::framesInFlight() const
{
    return impl->framesInFlight;
}

float GPUView::backingScale() const
{
    // No layer or swapchain to ask. Callers wanting a real snapshot scale pass
    // it to renderToImage explicitly, which is what the tests do.
    return 1.f;
}

void GPUView::resized() {}

void GPUView::backingScaleChanged() {}

void GPUView::paint(Graphics::Context&) {}

void GPUView::renderNow() {}

Graphics::Image GPUView::renderNativeContent(float scale)
{
    auto bounds = getLocalBounds();
    auto pixelWidth = static_cast<int>(std::lround(bounds.w * scale));
    auto pixelHeight = static_cast<int>(std::lround(bounds.h * scale));

    if (pixelWidth <= 0 || pixelHeight <= 0)
        return {};

    auto& context = getVulkanContext();

    if (!context.isValid())
        return {};

    auto samples = impl->sampleCount;

    auto colorTexture =
        makeTarget(pixelWidth, pixelHeight, VK_FORMAT_B8G8R8A8_UNORM, 1);

    if (colorTexture.image == VK_NULL_HANDLE)
        return {};

    auto msaaTexture =
        samples > 1
            ? makeTarget(pixelWidth, pixelHeight, VK_FORMAT_B8G8R8A8_UNORM, samples)
            : VulkanTexture {};

    auto depthTexture =
        impl->depthEnabled
            ? makeTarget(pixelWidth, pixelHeight, VK_FORMAT_D32_SFLOAT, samples)
            : VulkanTexture {};

    {
        auto target = OffscreenTarget {
            &colorTexture,
            msaaTexture.image != VK_NULL_HANDLE ? &msaaTexture : nullptr,
            depthTexture.image != VK_NULL_HANDLE ? &depthTexture : nullptr};

        auto frame = Frame(Device::shared(), target);
        render(frame);
    }

    auto rowBytes = static_cast<std::size_t>(pixelWidth) * 4;
    auto total = rowBytes * static_cast<std::size_t>(pixelHeight);

    auto bufferInfo = VkBufferCreateInfo {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = total;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    auto readback = VkBuffer {VK_NULL_HANDLE};
    auto* device = context.getDevice();

    if (vkCreateBuffer(device, &bufferInfo, nullptr, &readback) != VK_SUCCESS)
    {
        destroyTarget(colorTexture);
        destroyTarget(msaaTexture);
        destroyTarget(depthTexture);
        return {};
    }

    auto allocation = context.allocateFor(
        readback,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    auto image = Graphics::Image {};

    if (allocation.memory != VK_NULL_HANDLE)
    {
        auto* commands = context.acquire();

        if (commands != nullptr)
        {
            transitionImage(
                commands->list, colorTexture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

            auto copy = VkBufferImageCopy {};
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent = {static_cast<std::uint32_t>(pixelWidth),
                                static_cast<std::uint32_t>(pixelHeight),
                                1};

            vkCmdCopyImageToBuffer(commands->list,
                                   colorTexture.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   readback,
                                   1,
                                   &copy);

            context.waitFor(context.submit(commands));

            auto* mapped = static_cast<void*>(nullptr);

            if (vkMapMemory(device, allocation.memory, 0, total, 0, &mapped)
                == VK_SUCCESS)
            {
                auto* dst = image.prepareForOverwrite(pixelWidth, pixelHeight);

                if (dst != nullptr)
                {
                    // BGRA8 premultiplied -> straight-alpha RGBA, matching what
                    // the Metal and D3D12 read-back paths hand Image.
                    const auto* src = static_cast<const std::uint8_t*>(mapped);
                    auto count = static_cast<std::size_t>(pixelWidth) * pixelHeight;

                    for (auto i = std::size_t {}; i < count; ++i)
                    {
                        auto b = src[i * 4 + 0];
                        auto g = src[i * 4 + 1];
                        auto r = src[i * 4 + 2];
                        auto a = src[i * 4 + 3];

                        if (a == 0)
                        {
                            dst[i * 4 + 0] = 0;
                            dst[i * 4 + 1] = 0;
                            dst[i * 4 + 2] = 0;
                            dst[i * 4 + 3] = 0;
                            continue;
                        }

                        auto straight = [&](std::uint8_t channel) -> std::uint8_t
                        {
                            return static_cast<std::uint8_t>(
                                std::min(255, (channel * 255 + a / 2) / a));
                        };

                        dst[i * 4 + 0] = straight(r);
                        dst[i * 4 + 1] = straight(g);
                        dst[i * 4 + 2] = straight(b);
                        dst[i * 4 + 3] = a;
                    }
                }

                vkUnmapMemory(device, allocation.memory);
            }
        }
    }

    context.deferDestroy(readback, allocation.memory);
    destroyTarget(colorTexture);
    destroyTarget(msaaTexture);
    destroyTarget(depthTexture);

    return image;
}

bool GPUView::renderNativeContentToTarget(void*, float)
{
    // Zero-copy capture needs external-memory import of the platform's own
    // surface; the read-back path above is the only one here.
    return false;
}
} // namespace eacp::GPU
