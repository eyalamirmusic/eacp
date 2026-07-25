#include "GPUView.h"

#include "../Device/Device.h"
#include "../Frame/Frame.h"
#include "../Vulkan/VulkanContext.h"
#include "../Vulkan/VulkanTypes.h"
#include "GPUViewSurface-Vulkan.h"

#include <eacp/Graphics/Helpers/DisplayLink.h>
#include <eacp/Graphics/Image/Image.h>

#include <algorithm>
#include <cmath>
#include <cstring>

// Vulkan backend. The view owns a swapchain over a platform surface (see
// GPUViewSurface-Vulkan.h for where the platform enters) and presents from it,
// plus the off-screen read-back path View::renderToImage uses. Everything here
// is portable; nothing in this file knows which window system it is on.

namespace eacp::GPU
{
// Defined in Texture-Vulkan.cpp.
void transitionImage(VkCommandBuffer list,
                     VulkanTexture& texture,
                     VkImageLayout target);

namespace
{
constexpr auto swapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
constexpr auto depthFormat = VK_FORMAT_D32_SFLOAT;

// A render-target image, unlike the sampled ones Texture creates: it carries
// attachment usage and stays in whatever layout the frame leaves it.
VulkanTexture makeTarget(int width, int height, VkFormat format, int samples)
{
    auto& context = getVulkanContext();
    auto* device = context.getDevice();

    auto texture = VulkanTexture {};
    texture.format = format;
    texture.width = width;
    texture.height = height;

    auto isDepth = format == depthFormat;

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
    explicit Native(GPUView& viewToUse)
        : view(viewToUse)
    {
        auto& context = getVulkanContext();

        if (!context.isValid() || !context.canPresent())
            return;

        host = detail::createSurfaceHost(view, context.getInstance());
    }

    ~Native()
    {
        auto& context = getVulkanContext();

        if (context.isValid())
        {
            context.waitIdle();
            releaseSwapchain();
            releaseSync();
        }

        detail::destroySurfaceHost(host, context.getInstance());
    }

    bool canPresent() const { return host.surface != VK_NULL_HANDLE; }

    void releaseSync()
    {
        auto* device = getVulkanContext().getDevice();

        for (auto i = 0; i < acquireSemaphores.size(); ++i)
            vkDestroySemaphore(device, acquireSemaphores[i], nullptr);

        for (auto i = 0; i < renderSemaphores.size(); ++i)
            vkDestroySemaphore(device, renderSemaphores[i], nullptr);

        acquireSemaphores.clear();
        renderSemaphores.clear();
        frameValues.clear();
    }

    void buildSync()
    {
        if (acquireSemaphores.size() == framesInFlight)
            return;

        releaseSync();

        auto& context = getVulkanContext();

        for (auto i = 0; i < framesInFlight; ++i)
        {
            acquireSemaphores.add(context.makeSemaphore());
            renderSemaphores.add(context.makeSemaphore());
            frameValues.add(0);
        }
    }

    // The swapchain owns its images, so only the views this created are
    // destroyed here -- freeing a swapchain image would be a double free.
    void releaseSwapchain()
    {
        auto& context = getVulkanContext();
        auto* device = context.getDevice();

        for (auto i = 0; i < backBuffers.size(); ++i)
            if (backBuffers[i].view != VK_NULL_HANDLE)
                vkDestroyImageView(device, backBuffers[i].view, nullptr);

        backBuffers.clear();

        if (swapchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(device, swapchain, nullptr);
            swapchain = VK_NULL_HANDLE;
        }

        destroyTarget(msaaTarget);
        destroyTarget(depthTarget);
    }

    void updateSize()
    {
        auto scale = detail::surfaceBackingScale(view);
        auto bounds = view.getLocalBounds();

        auto newWidth = static_cast<int>(std::lround(bounds.w * scale));
        auto newHeight = static_cast<int>(std::lround(bounds.h * scale));

        detail::resizeSurfaceHost(host, view, scale);

        auto scaleChanged = backingScale > 0.f && scale != backingScale;
        auto previousScale = backingScale;
        backingScale = scale;

        if (newWidth != pixelWidth || newHeight != pixelHeight
            || swapchain == VK_NULL_HANDLE)
        {
            pixelWidth = newWidth;
            pixelHeight = newHeight;
            rebuildSwapchain();
        }

        // Notified after the swapchain is consistent at the new scale, so a
        // handler rebuilding pixel-sized resources sees what it will draw into.
        if (scaleChanged && previousScale != scale)
            view.onBackingScaleChanged(scale);
    }

    void rebuildSwapchain()
    {
        auto& context = getVulkanContext();

        if (!canPresent() || pixelWidth <= 0 || pixelHeight <= 0)
            return;

        context.waitIdle();
        releaseSwapchain();
        buildSync();

        auto capabilities = VkSurfaceCapabilitiesKHR {};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            context.getPhysicalDevice(), host.surface, &capabilities);

        auto imageCount = std::max(static_cast<std::uint32_t>(framesInFlight),
                                   capabilities.minImageCount);

        if (capabilities.maxImageCount > 0)
            imageCount = std::min(imageCount, capabilities.maxImageCount);

        auto extent = VkExtent2D {static_cast<std::uint32_t>(pixelWidth),
                                  static_cast<std::uint32_t>(pixelHeight)};

        if (capabilities.currentExtent.width != UINT32_MAX)
            extent = capabilities.currentExtent;

        if (extent.width == 0 || extent.height == 0)
            return;

        auto info =
            VkSwapchainCreateInfoKHR {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = host.surface;
        info.minImageCount = imageCount;
        info.imageFormat = swapchainFormat;
        info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = capabilities.currentTransform;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

        // FIFO is the only mode every implementation must offer, and it is the
        // vsync-paced one a display-link-driven view wants anyway.
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;

        auto* device = context.getDevice();

        if (vkCreateSwapchainKHR(device, &info, nullptr, &swapchain) != VK_SUCCESS)
        {
            swapchain = VK_NULL_HANDLE;
            return;
        }

        pixelWidth = static_cast<int>(extent.width);
        pixelHeight = static_cast<int>(extent.height);

        auto count = std::uint32_t {};
        vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);

        auto images = Vector<VkImage> {};
        images.resize(static_cast<int>(count));
        vkGetSwapchainImagesKHR(device, swapchain, &count, &images[0]);

        for (auto i = 0; i < images.size(); ++i)
        {
            auto backBuffer = VulkanTexture {};
            backBuffer.image = images[i];
            backBuffer.format = swapchainFormat;
            backBuffer.width = pixelWidth;
            backBuffer.height = pixelHeight;
            backBuffer.layout = VK_IMAGE_LAYOUT_UNDEFINED;

            auto viewInfo =
                VkImageViewCreateInfo {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = backBuffer.image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = swapchainFormat;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;

            vkCreateImageView(device, &viewInfo, nullptr, &backBuffer.view);
            backBuffers.add(backBuffer);
        }

        if (sampleCount > 1)
            msaaTarget =
                makeTarget(pixelWidth, pixelHeight, swapchainFormat, sampleCount);

        if (depthEnabled)
            depthTarget =
                makeTarget(pixelWidth, pixelHeight, depthFormat, sampleCount);
    }

    void present()
    {
        auto& context = getVulkanContext();

        if (!canPresent() || !context.isValid())
            return;

        if (swapchain == VK_NULL_HANDLE)
        {
            updateSize();

            if (swapchain == VK_NULL_HANDLE)
                return;
        }

        // A semaphore slot cannot be reused while the frame that last signalled
        // it is still on the GPU.
        context.waitFor(frameValues[frameSlot]);

        auto imageIndex = std::uint32_t {};
        auto acquired = vkAcquireNextImageKHR(context.getDevice(),
                                              swapchain,
                                              UINT64_MAX,
                                              acquireSemaphores[frameSlot],
                                              VK_NULL_HANDLE,
                                              &imageIndex);

        if (acquired == VK_ERROR_OUT_OF_DATE_KHR)
        {
            rebuildSwapchain();
            return;
        }

        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
            return;

        auto drawable = VulkanDrawable {};
        drawable.image = &backBuffers[static_cast<int>(imageIndex)];
        drawable.swapchain = swapchain;
        drawable.imageIndex = imageIndex;
        drawable.acquired = acquireSemaphores[frameSlot];
        drawable.rendered = renderSemaphores[frameSlot];

        {
            auto frame =
                Frame(Device::shared(),
                      &drawable,
                      msaaTarget.image != VK_NULL_HANDLE ? &msaaTarget : nullptr,
                      depthTarget.image != VK_NULL_HANDLE ? &depthTarget : nullptr);
            view.render(frame);
        }

        frameValues[frameSlot] = context.lastSubmitted();
        frameSlot = (frameSlot + 1) % framesInFlight;
    }

    void startContinuous()
    {
        if (displayLink == nullptr)
            displayLink = makeOwned<Threads::DisplayLink>(
                [this](Threads::FrameTime time)
                {
                    view.update(time);
                    view.renderNow();
                });
    }

    void stopContinuous() { displayLink = nullptr; }

    GPUView& view;
    detail::SurfaceHost host;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    Vector<VulkanTexture> backBuffers;
    VulkanTexture msaaTarget;
    VulkanTexture depthTarget;

    Vector<VkSemaphore> acquireSemaphores;
    Vector<VkSemaphore> renderSemaphores;
    Vector<std::uint64_t> frameValues;
    int frameSlot = 0;

    int sampleCount = 4;
    int framesInFlight = 3;
    bool continuous = false;
    bool depthEnabled = false;
    float backingScale = 0.f;
    int pixelWidth = 0;
    int pixelHeight = 0;

    OwningPointer<Threads::DisplayLink> displayLink;
};

GPUView::GPUView()
    : impl(*this)
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
    impl->updateSize();
}

bool GPUView::hasDepth() const
{
    return impl->depthEnabled;
}

void GPUView::setContinuous(bool continuous)
{
    impl->continuous = continuous;

    if (continuous)
        impl->startContinuous();
    else
        impl->stopContinuous();
}

bool GPUView::isContinuous() const
{
    return impl->continuous;
}

void GPUView::setFramesInFlight(int count)
{
    // Two is the floor: one would leave the GPU idle waiting on the display.
    impl->framesInFlight = std::clamp(count, 2, 3);
}

int GPUView::framesInFlight() const
{
    return impl->framesInFlight;
}

float GPUView::backingScale() const
{
    if (impl->backingScale > 0.f)
        return impl->backingScale;

    return detail::surfaceBackingScale(const_cast<GPUView&>(*this));
}

void GPUView::resized()
{
    Graphics::View::resized();
    impl->updateSize();

    // Draw at the new size within the layout pass rather than waiting for the
    // display link, so a live resize does not lag behind the window.
    renderNow();
}

void GPUView::backingScaleChanged()
{
    Graphics::View::backingScaleChanged();
    impl->updateSize();
    renderNow();
}

void GPUView::paint(Graphics::Context& context)
{
    // A snapshot captures GPU content via renderNativeContent; presenting a live
    // frame here would be a side effect of taking a picture.
    if (context.isSnapshot())
        return;

    renderNow();
}

void GPUView::renderNow()
{
    impl->present();
}

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

    auto colorTexture = makeTarget(pixelWidth, pixelHeight, swapchainFormat, 1);

    if (colorTexture.image == VK_NULL_HANDLE)
        return {};

    auto msaaTexture =
        samples > 1 ? makeTarget(pixelWidth, pixelHeight, swapchainFormat, samples)
                    : VulkanTexture {};

    auto depthTexture =
        impl->depthEnabled
            ? makeTarget(pixelWidth, pixelHeight, depthFormat, samples)
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
