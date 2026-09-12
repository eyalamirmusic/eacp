#include "GPUView.h"

#include "../Device/Device.h"
#include "../Frame/Frame.h"
#include "../Texture/Texture.h"
#include "../Vulkan/VulkanTypes.h"

#include <eacp/Graphics/Helpers/DisplayLink.h>
#include <eacp/Graphics/View/View-Linux.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>

namespace eacp::GPU
{
namespace
{
// Bounded so a compositor that stops handing images back cannot hang the main
// thread inside the driver.
constexpr auto acquireTimeoutNanoseconds = std::uint64_t {100000000};

VkCompositeAlphaFlagBitsKHR
    chooseCompositeAlpha(const VkSurfaceCapabilitiesKHR& capabilities)
{
    const auto offered = capabilities.supportedCompositeAlpha;

    if ((offered & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0)
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

    for (auto bit = std::uint32_t {1}; bit != 0; bit <<= 1)
        if ((offered & bit) != 0)
            return static_cast<VkCompositeAlphaFlagBitsKHR>(bit);

    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}
} // namespace

struct GPUView::Native
{
    explicit Native(GPUView& viewToUse)
        : view(viewToUse)
        , record(Graphics::requestViewSurface(viewToUse))
    {
        // Asking for the record is what marks this view as one that presents.
        record.onAvailable = [this] { surfaceAvailable(); };
        record.onLost = [this] { surfaceLost(); };
        record.onResized = [this] { surfaceResized(); };
        record.onRepaint = [this] { render(); };
        record.onFrameDone = [this] { frameCallbackArrived(); };
    }

    ~Native()
    {
        // The record outlives this: onLost fires from ~View, after the Pimpl
        // has gone, so the hooks must stop pointing here first.
        record.onAvailable = [] {};
        record.onLost = [] {};
        record.onResized = [] {};
        record.onRepaint = [] {};
        record.onFrameDone = [] {};

        stopContinuous();
        destroySwapchain();
        destroySurface();
    }

    void surfaceAvailable()
    {
        if (deviceLost || !createSurface())
            return;

        swapchainStale = false;
        companionsStale = false;

        if (!createSwapchain())
        {
            destroySwapchain();
            destroySurface();
            return;
        }

        // No frame callback arrives until something has been presented.
        if (continuous)
            continuousTick(true);
        else
            render();
    }

    // Fires before the wl_surface is torn down: a swapchain that outlives it is
    // a use-after-free inside the driver.
    void surfaceLost()
    {
        destroySwapchain();
        destroySurface();
    }

    void surfaceResized()
    {
        swapchainStale = true;

        // A compositor that resized a surface is not obliged to repaint it.
        view.repaint();
    }

    void frameCallbackArrived()
    {
        if (continuous)
            continuousTick(false);
    }

    float surfaceScale() const
    {
        if (record.surface != nullptr && record.scale > 0.f)
            return record.scale;

        return Graphics::linuxDefaultBackingScale;
    }

    // A scale change tells itself apart from a resize by the surface's own
    // scale. Nothing is recorded before a surface carries a real one, so the
    // first that arrives is the initial scale rather than a change.
    void notifyScaleChange()
    {
        if (record.surface == nullptr)
            return;

        const auto scale = surfaceScale();
        const auto changed = backingScale > 0.f && scale != backingScale;
        backingScale = scale;

        if (changed)
            view.onBackingScaleChanged(scale);
    }

    void startContinuous()
    {
        stampedTick = Threads::DisplayLink::timedTick(
            [this](Threads::FrameTime time)
            {
                view.update(time);
                render();
            });

        pacingStarted = false;
        continuousTick(true);
    }

    void stopContinuous()
    {
        stampedTick = [] {};
        pacingStarted = false;
    }

    // The half-tick grace keeps 60 on a 120 Hz compositor from missing the
    // interval by float dust and running at 40.
    bool tickIsDue()
    {
        using Clock = std::chrono::steady_clock;

        if (maxFps <= 0)
        {
            pacingStarted = false;
            return true;
        }

        const auto interval = 1.0 / static_cast<double>(maxFps);
        const auto now = Clock::now();

        if (!pacingStarted)
        {
            pacingStarted = true;
            lastTick = now;
            accumulated = interval;
        }

        const auto period = std::chrono::duration<double>(now - lastTick).count();
        lastTick = now;
        accumulated += period;

        if (accumulated + period * 0.5 < interval)
            return false;

        accumulated = std::min(accumulated - interval, interval * 0.5);
        return true;
    }

    void continuousTick(bool force)
    {
        if (!continuous)
            return;

        // Dropped here, not at render(): a frame never drawn must not have
        // advanced update().
        if (swapchain == VK_NULL_HANDLE)
            return;

        // A tick that presents nothing earns no frame callback of its own, so
        // the request is made here; requestFrameCallback commits for it.
        if (!force && !tickIsDue())
        {
            record.requestFrameCallback();
            return;
        }

        stampedTick();
    }

    bool createSurface()
    {
        auto& shared = getVulkanShared();

        if (!shared.supportsPresentation())
        {
            reportNoPresentation();
            return false;
        }

        if (vkSurface != VK_NULL_HANDLE)
            return true;

        if (record.display == nullptr || record.surface == nullptr)
            return false;

        VkWaylandSurfaceCreateInfoKHR info = {};
        info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
        info.display = record.display;
        info.surface = record.surface;

        if (vkCreateWaylandSurfaceKHR(
                shared.getInstance(), &info, nullptr, &vkSurface)
            != VK_SUCCESS)
        {
            vkSurface = VK_NULL_HANDLE;
            return false;
        }

        auto presentable = VkBool32 {VK_FALSE};
        vkGetPhysicalDeviceSurfaceSupportKHR(shared.getPhysicalDevice(),
                                             shared.getQueueFamily(),
                                             vkSurface,
                                             &presentable);

        if (presentable == VK_TRUE)
            return true;

        LOG("Vulkan: the queue this device renders on cannot present to this "
            "surface, so the view has no swapchain");

        destroySurface();
        return false;
    }

    void destroySurface()
    {
        if (vkSurface == VK_NULL_HANDLE)
            return;

        vkDestroySurfaceKHR(getVulkanShared().getInstance(), vkSurface, nullptr);
        vkSurface = VK_NULL_HANDLE;
    }

    static void reportNoPresentation()
    {
        static auto reported = false;

        if (reported)
            return;

        reported = true;
        LOG("Vulkan: no swapchain support on this device, so a GPUView renders "
            "off-screen only");
    }

    VkSurfaceFormatKHR chooseFormat(VkPhysicalDevice physical) const
    {
        auto count = std::uint32_t {0};
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical, vkSurface, &count, nullptr);

        auto formats = Vector<VkSurfaceFormatKHR> {};
        formats.resize(static_cast<int>(count));
        vkGetPhysicalDeviceSurfaceFormatsKHR(
            physical, vkSurface, &count, formats.data());

        if (formats.empty())
            return {};

        // What RenderPipelineDescriptor::colorFormat defaults to, and not an
        // sRGB format: nothing in eacp expects the hardware to encode on write.
        for (const auto& format: formats)
            if (format.format == VK_FORMAT_B8G8R8A8_UNORM
                && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                return format;

        return formats[0];
    }

    VkPresentModeKHR choosePresentMode(VkPhysicalDevice physical) const
    {
        auto count = std::uint32_t {0};
        vkGetPhysicalDeviceSurfacePresentModesKHR(
            physical, vkSurface, &count, nullptr);

        auto modes = Vector<VkPresentModeKHR> {};
        modes.resize(static_cast<int>(count));
        vkGetPhysicalDeviceSurfacePresentModesKHR(
            physical, vkSurface, &count, modes.data());

        for (auto mode: modes)
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
                return VK_PRESENT_MODE_MAILBOX_KHR;

        return VK_PRESENT_MODE_FIFO_KHR;
    }

    // Wayland answers 0xFFFFFFFF, meaning "you decide": a wl_surface has no
    // server-side size, so the extent comes from the record.
    VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) const
    {
        if (capabilities.currentExtent.width != 0xFFFFFFFFu)
            return capabilities.currentExtent;

        if (record.pixelWidth <= 0 || record.pixelHeight <= 0)
            return {};

        auto extent = VkExtent2D {static_cast<std::uint32_t>(record.pixelWidth),
                                  static_cast<std::uint32_t>(record.pixelHeight)};

        extent.width = std::clamp(extent.width,
                                  capabilities.minImageExtent.width,
                                  capabilities.maxImageExtent.width);
        extent.height = std::clamp(extent.height,
                                   capabilities.minImageExtent.height,
                                   capabilities.maxImageExtent.height);

        return extent;
    }

    bool createSwapchain()
    {
        auto& shared = getVulkanShared();
        const auto physical = shared.getPhysicalDevice();
        const auto vulkanDevice = shared.getDevice();

        if (vkSurface == VK_NULL_HANDLE || vulkanDevice == VK_NULL_HANDLE)
            return false;

        VkSurfaceCapabilitiesKHR capabilities = {};

        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                physical, vkSurface, &capabilities)
            != VK_SUCCESS)
            return false;

        const auto extent = chooseExtent(capabilities);

        if (extent.width == 0 || extent.height == 0)
            return false;

        const auto format = chooseFormat(physical);

        if (format.format == VK_FORMAT_UNDEFINED)
            return false;

        auto imageCount = capabilities.minImageCount + 1;

        if (capabilities.maxImageCount > 0
            && imageCount > capabilities.maxImageCount)
            imageCount = capabilities.maxImageCount;

        VkSwapchainCreateInfoKHR info = {};
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.surface = vkSurface;
        info.minImageCount = imageCount;
        info.imageFormat = format.format;
        info.imageColorSpace = format.colorSpace;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;

        // Every extra usage bit is one a compositor is allowed to refuse.
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;

        // Naming the compositor's own transform says there is nothing to undo.
        info.preTransform = capabilities.currentTransform;
        info.compositeAlpha = chooseCompositeAlpha(capabilities);
        info.presentMode = choosePresentMode(physical);
        info.clipped = VK_TRUE;

        // The old handle is still ours to destroy afterwards.
        info.oldSwapchain = swapchain;

        auto created = VkSwapchainKHR {VK_NULL_HANDLE};

        if (vkCreateSwapchainKHR(vulkanDevice, &info, nullptr, &created)
            != VK_SUCCESS)
            return false;

        if (swapchain != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(vulkanDevice, swapchain, nullptr);

        swapchain = created;
        swapchainFormat = format.format;
        swapchainWidth = static_cast<int>(extent.width);
        swapchainHeight = static_cast<int>(extent.height);

        return createImages() && createCompanions() && createSemaphores();
    }

    bool createImages()
    {
        const auto vulkanDevice = getVulkanShared().getDevice();

        auto count = std::uint32_t {0};
        vkGetSwapchainImagesKHR(vulkanDevice, swapchain, &count, nullptr);

        auto handles = Vector<VkImage> {};
        handles.resize(static_cast<int>(count));
        vkGetSwapchainImagesKHR(vulkanDevice, swapchain, &count, handles.data());

        if (handles.empty())
            return false;

        for (auto handle: handles)
        {
            // `presentable` makes restingUse() answer PRESENT_SRC_KHR.
            auto data = VulkanTextureData {};
            data.presentable = true;
            data.image = handle;
            data.format = swapchainFormat;
            data.width = swapchainWidth;
            data.height = swapchainHeight;
            data.attachmentView = makeVulkanImageView(vulkanDevice,
                                                      handle,
                                                      swapchainFormat,
                                                      VK_IMAGE_VIEW_TYPE_2D,
                                                      VK_IMAGE_ASPECT_COLOR_BIT,
                                                      1,
                                                      1);

            if (data.attachmentView == VK_NULL_HANDLE)
                return false;

            images.add(data);
        }

        return true;
    }

    // Shared by every image; only the layouts are per image, which
    // lendCompanions and reclaimCompanions carry.
    bool createCompanions()
    {
        auto& gpu = Device::shared();
        auto& context = getVulkanContext(gpu);

        companions = {};
        companions.format = swapchainFormat;
        companions.width = swapchainWidth;
        companions.height = swapchainHeight;

        companions.sampleCount =
            sampleCount > 1 && gpu.supportsSampleCount(sampleCount) ? sampleCount
                                                                    : 1;

        if (companions.sampleCount > 1
            && !createVulkanMultisampleCompanion(context, companions))
            return false;

        if (depthEnabled || stencilEnabled)
            createVulkanDepthCompanion(context, companions, stencilEnabled, false);

        return true;
    }

    // One acquire semaphore per frame in flight, one render-finished per image:
    // a per-frame array of the second breaks when an image comes round twice.
    bool createSemaphores()
    {
        const auto vulkanDevice = getVulkanShared().getDevice();

        // At most one fewer than there are images, or the acquire has nothing
        // left to hand back.
        const auto slots =
            std::clamp(framesInFlight, 1, std::max(images.size() - 1, 1));

        VkSemaphoreCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        const auto makeSemaphores = [&](Vector<VkSemaphore>& into, int count)
        {
            for (auto index = 0; index < count; ++index)
            {
                auto semaphore = VkSemaphore {VK_NULL_HANDLE};

                if (vkCreateSemaphore(vulkanDevice, &info, nullptr, &semaphore)
                    != VK_SUCCESS)
                    return false;

                into.add(semaphore);
            }

            return true;
        };

        if (!makeSemaphores(acquireSemaphores, slots)
            || !makeSemaphores(renderFinished, images.size()))
            return false;

        slotValues.resize(slots, 0);
        currentSlot = 0;

        return true;
    }

    void destroySwapchainResources()
    {
        const auto vulkanDevice = getVulkanShared().getDevice();

        if (vulkanDevice == VK_NULL_HANDLE)
            return;

        waitForOutstandingWork();

        for (auto semaphore: acquireSemaphores)
            vkDestroySemaphore(vulkanDevice, semaphore, nullptr);

        for (auto semaphore: renderFinished)
            vkDestroySemaphore(vulkanDevice, semaphore, nullptr);

        acquireSemaphores.clear();
        renderFinished.clear();
        slotValues.clear();
        currentSlot = 0;

        for (auto& image: images)
            if (image.attachmentView != VK_NULL_HANDLE)
                vkDestroyImageView(vulkanDevice, image.attachmentView, nullptr);

        images.clear();

        releaseVulkanCompanions(getVulkanContext(Device::shared()), companions);
        companions = {};
    }

    void destroySwapchain()
    {
        destroySwapchainResources();

        if (swapchain == VK_NULL_HANDLE)
            return;

        vkDestroySwapchainKHR(getVulkanShared().getDevice(), swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }

    // The timeline covers what this Device submitted; the device wait covers a
    // present semaphore the engine has not picked up - the one about to go.
    void waitForOutstandingWork()
    {
        auto& shared = getVulkanShared();
        const auto vulkanDevice = shared.getDevice();

        if (vulkanDevice == VK_NULL_HANDLE)
            return;

        getVulkanContext(Device::shared()).waitIdle();

        auto lock = std::lock_guard<std::mutex> {shared.getQueueMutex()};
        vkDeviceWaitIdle(vulkanDevice);
    }

    bool rebuildSwapchain()
    {
        swapchainStale = false;
        companionsStale = false;

        destroySwapchainResources();

        if (createSwapchain())
            return true;

        destroySwapchain();
        swapchainStale = true;

        return false;
    }

    void rebuildCompanions()
    {
        companionsStale = false;

        if (swapchain == VK_NULL_HANDLE)
            return;

        waitForOutstandingWork();
        releaseVulkanCompanions(getVulkanContext(Device::shared()), companions);

        if (!createCompanions())
        {
            destroySwapchain();
            swapchainStale = true;
        }
    }

    // No allocations: `companions` owns those, and an image that carried one
    // could be told to release it.
    void lendCompanions(VulkanTextureData& target)
    {
        target.sampleCount = companions.sampleCount;

        target.msaaImage = companions.msaaImage;
        target.msaaView = companions.msaaView;
        target.msaaUse = companions.msaaUse;

        target.depthImage = companions.depthImage;
        target.depthAttachmentView = companions.depthAttachmentView;
        target.depthUse = companions.depthUse;
        target.depthHasStencil = companions.depthHasStencil;
        target.depthFormat = companions.depthFormat;
    }

    void reclaimCompanions(const VulkanTextureData& target)
    {
        companions.msaaUse = target.msaaUse;
        companions.depthUse = target.depthUse;
    }

    bool readyToRender()
    {
        if (deviceLost || record.surface == nullptr)
            return false;

        if (!Device::shared().isValid())
            return false;

        if (swapchainStale && !rebuildSwapchain())
            return false;

        if (companionsStale)
            rebuildCompanions();

        return swapchain != VK_NULL_HANDLE && !images.empty();
    }

    // Not re-entrant: view.render() may call repaint(), and a second frame would
    // acquire against a semaphore the first is still waiting on.
    void render()
    {
        if (rendering || !readyToRender())
            return;

        rendering = true;
        renderOneFrame();
        rendering = false;
    }

    void renderOneFrame()
    {
        auto& gpu = Device::shared();
        auto& context = getVulkanContext(gpu);
        const auto vulkanDevice = getVulkanShared().getDevice();

        // The CPU throttle: this slot's acquire semaphore cannot be handed out
        // again until the frame that waited on it has run.
        context.waitFor(slotValues[currentSlot]);

        auto imageIndex = std::uint32_t {0};
        const auto acquired = vkAcquireNextImageKHR(vulkanDevice,
                                                    swapchain,
                                                    acquireTimeoutNanoseconds,
                                                    acquireSemaphores[currentSlot],
                                                    VK_NULL_HANDLE,
                                                    &imageIndex);

        if (acquired == VK_ERROR_DEVICE_LOST)
        {
            noteDeviceLost();
            return;
        }

        // Out of date signalled nothing, so there is nothing to undo.
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR)
        {
            swapchainStale = true;
            return;
        }

        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
            return;

        // Suboptimal did hand over an image and did signal, so dropping the
        // frame here would leave a signalled semaphore behind.
        if (acquired == VK_SUBOPTIMAL_KHR)
            swapchainStale = true;

        auto& target = images[static_cast<int>(imageIndex)];

        // An acquired image's contents and layout are undefined.
        target.use = imageAcquired;
        lendCompanions(target);

        auto drawable = VulkanDrawable {};
        drawable.swapchain = swapchain;
        drawable.imageIndex = imageIndex;
        drawable.target = &target;
        drawable.acquired = acquireSemaphores[currentSlot];
        drawable.renderFinished = renderFinished[static_cast<int>(imageIndex)];

        // The first one rides on this present; after that it commits itself.
        record.requestFrameCallback();

        {
            auto frame = Frame(gpu, &drawable, nullptr, nullptr, surfaceScale());
            view.render(frame);
        }

        reclaimCompanions(target);

        slotValues[currentSlot] = context.lastSubmitted();
        currentSlot = (currentSlot + 1) % acquireSemaphores.size();

        handlePresentResult(drawable.presentResult);
    }

    void handlePresentResult(VkResult result)
    {
        if (result == VK_ERROR_DEVICE_LOST)
        {
            noteDeviceLost();
            return;
        }

        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
            swapchainStale = true;

        // The frame never presented, so its acquire semaphore was signalled and
        // never waited on; only the rebuild retires it.
        if (result == VK_NOT_READY)
            swapchainStale = true;
    }

    void noteDeviceLost()
    {
        if (deviceLost)
            return;

        deviceLost = true;

        LOG("GPUView: the Vulkan device was lost; this view has stopped "
            "presenting. Rebuilding the device is not implemented on this "
            "backend, so onDeviceRestored will not fire.");

        stopContinuous();
        destroySwapchain();
        destroySurface();
    }

    GPUView& view;
    Graphics::ViewSurface& record;

    int sampleCount = 1;
    int maxFps = 0;
    int framesInFlight = 2;
    bool depthEnabled = false;
    bool stencilEnabled = false;
    bool continuous = false;

    VkSurfaceKHR vkSurface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
    int swapchainWidth = 0;
    int swapchainHeight = 0;

    Vector<VulkanTextureData> images;
    VulkanTextureData companions;

    Vector<VkSemaphore> acquireSemaphores;
    Vector<VkSemaphore> renderFinished;

    Vector<std::uint64_t> slotValues;
    int currentSlot = 0;

    bool swapchainStale = false;
    bool companionsStale = false;
    bool deviceLost = false;
    bool rendering = false;

    // Zero until the surface reports one, which is how the initial scale is told
    // apart from a change.
    float backingScale = 0.f;

    Callback stampedTick = [] {};

    std::chrono::steady_clock::time_point lastTick;
    double accumulated = 0.0;
    bool pacingStarted = false;
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
    impl->sampleCount = count;
    impl->companionsStale = true;
}

void GPUView::setDepth(bool enabled)
{
    impl->depthEnabled = enabled;

    // The stencil plane lives in the depth attachment, so it goes with it.
    if (!enabled)
        impl->stencilEnabled = false;

    impl->companionsStale = true;
}

bool GPUView::hasDepth() const
{
    return impl->depthEnabled;
}

void GPUView::setStencil(bool enabled)
{
    impl->stencilEnabled = enabled;

    if (enabled)
        impl->depthEnabled = true;

    impl->companionsStale = true;
}

bool GPUView::hasStencil() const
{
    return impl->stencilEnabled;
}

void GPUView::setContinuous(bool continuous)
{
    if (impl->continuous == continuous)
        return;

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

void GPUView::setMaxFps(int fps)
{
    impl->maxFps = fps;

    // The pacing restarts at the new rate rather than carrying the old one's
    // accumulated remainder into it.
    impl->pacingStarted = false;
}

int GPUView::maxFps() const
{
    return impl->maxFps;
}

void GPUView::setFramesInFlight(int count)
{
    impl->framesInFlight = count < 1 ? 1 : count;

    if (impl->swapchain != VK_NULL_HANDLE)
        impl->swapchainStale = true;
}

int GPUView::framesInFlight() const
{
    return impl->framesInFlight;
}

// No drawable to resize here: the swapchain is rebuilt from the surface's own
// configure, so all this owes either event is the scale notification.
void GPUView::resizeStarted()
{
    impl->notifyScaleChange();
}

// Nothing to order after the subclass: the swapchain is rebuilt from that
// configure and the frame drawn from a frame callback, both after every
// override has run.
void GPUView::resizeFinished() {}

float GPUView::backingScale() const
{
    if (renderScale > 0.f)
        return renderScale;

    return impl->surfaceScale();
}

void GPUView::paint(Graphics::Context&)
{
    // Never called on Linux: there is no 2D Context.
}

void GPUView::renderNow()
{
    impl->render();
}

// Touches no swapchain, so this is also what runs headless.
Graphics::Image GPUView::renderNativeContent(float scale)
{
    const auto bounds = getLocalBounds();
    const auto pixelWidth = static_cast<int>(std::lround(bounds.w * scale));
    const auto pixelHeight = static_cast<int>(std::lround(bounds.h * scale));

    if (pixelWidth <= 0 || pixelHeight <= 0)
        return {};

    auto& device = Device::shared();

    if (!device.isValid())
        return {};

    auto descriptor = TextureDescriptor {};
    descriptor.width = pixelWidth;
    descriptor.height = pixelHeight;

    descriptor.format = TextureFormat::BGRA8Unorm;
    descriptor.renderTarget = true;

    descriptor.sampleCount =
        device.supportsSampleCount(impl->sampleCount) ? impl->sampleCount : 1;

    descriptor.depth = impl->depthEnabled;
    descriptor.stencil = impl->stencilEnabled;

    auto texture = device.makeTexture(descriptor);

    if (!texture.isValid())
        return {};

    {
        auto target = OffscreenTarget {};
        target.colorTexture = texture.nativeTexture();

        auto frame = Frame(device, target, scale);

        renderScale = scale;
        render(frame);
        renderScale = 0.f;
    }
    // The Frame destructor submitted and waited, so the texture can be read.

    const auto pixelCount =
        static_cast<std::size_t>(pixelWidth) * static_cast<std::size_t>(pixelHeight);

    auto bgra = Vector<std::uint8_t>(static_cast<int>(pixelCount * 4));

    texture.read(bgra.data());

    auto image = Graphics::Image {};
    auto* dst = image.prepareForOverwrite(pixelWidth, pixelHeight);

    if (dst == nullptr)
        return {};

    for (auto pixel = std::size_t {0}; pixel < pixelCount; ++pixel)
    {
        const auto* src = bgra.data() + pixel * 4;
        auto* out = dst + pixel * 4;

        const auto b = src[0];
        const auto g = src[1];
        const auto r = src[2];
        const auto a = src[3];

        if (a == 0)
        {
            out[0] = out[1] = out[2] = out[3] = 0;
            continue;
        }

        const auto straight = [a](std::uint8_t channel)
        {
            return static_cast<std::uint8_t>(
                std::min(255, (channel * 255 + a / 2) / a));
        };

        out[0] = straight(r);
        out[1] = straight(g);
        out[2] = straight(b);
        out[3] = a;
    }

    return image;
}

bool GPUView::renderNativeContentToTarget(void*, float)
{
    return false;
}
} // namespace eacp::GPU
