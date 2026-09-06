#include "GPUView.h"

#include "../Device/Device.h"
#include "../Frame/Frame.h"
#include "../Texture/Texture.h"
#include "../Vulkan/VulkanTypes.h"

#include <eacp/Core/Threads/Timer.h>
#include <eacp/Graphics/Helpers/DisplayLink.h>
#include <eacp/Graphics/View/View-Linux.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>

// Linux/Vulkan backend. Two halves that share nothing but the view: an
// off-screen one that renders into a texture it owns and reads the pixels back
// - the path View::renderToImage takes and the one the GPU test suite rides on,
// which needs no compositor at all - and a swapchain that puts pixels on a
// Wayland surface.
//
// **What the GPU module knows about Wayland is two opaque pointers.** The
// window backend hands this view a Graphics::ViewSurface
// (Graphics/View/View-Linux.h): a wl_display and a wl_surface of its own, the
// pixel size the compositor expects, the scale, and five hooks. Everything else
// - the subsurface, the xdg_toplevel, the frame callbacks, who commits what -
// belongs on the other side of that header, and eacp-gpu neither links nor
// includes libwayland. What comes back the other way is a VkSurfaceKHR over
// those two pointers, a swapchain sized to pixelWidth x pixelHeight, and one
// present per frame.
//
// **Frames are paced by the compositor, not by a clock.** There is no
// DisplayLink here. Continuous mode renders a frame, asks for a
// wl_surface.frame callback, presents (which is the commit that carries the
// request), and renders the next frame when onFrameDone says the compositor has
// taken the last one. A window that is hidden or occluded gets no callbacks, so
// nothing is rendered for it - which is the right answer, and is reached
// without the main thread ever blocking inside vkAcquireNextImageKHR.
//
// **Nothing waits per frame.** The acquire and render-finished semaphores order
// the GPU against the presentation engine; what keeps the CPU from running away
// is the context timeline, waited on once per frame before a slot's acquire
// semaphore is handed out again. vkDeviceWaitIdle happens on a swapchain
// rebuild and on teardown and nowhere else.

namespace eacp::GPU
{
namespace
{
// How long vkAcquireNextImageKHR is given before the frame is dropped.
//
// Not UINT64_MAX, which is what most samples pass: this runs on the main
// thread, and a compositor that has stopped handing images back - one that
// stopped sending frame callbacks, a surface that is no longer mapped - would
// hang the whole app inside a driver call rather than merely stop drawing. A
// tenth of a second is far longer than the six-refresh worst case of a 60 Hz
// panel and short enough that the app stays alive if it is ever hit.
constexpr auto acquireTimeoutNanoseconds = std::uint64_t {100000000};

// The alpha mode asked for first, and what to fall back to. Opaque because a
// GPUView renders a rectangle of its own pixels; a compositor that offers no
// opaque mode gets whichever it does offer rather than a refused swapchain.
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
        // Asking for the record is what marks this view as one that presents,
        // and the hooks are set unconditionally: every one of them is a no-op
        // until the window backend has a surface to report, and the record
        // exists from the View's construction whether or not it ever gets one.
        record.onAvailable = [this] { surfaceAvailable(); };
        record.onLost = [this] { surfaceLost(); };
        record.onResized = [this] { surfaceResized(); };
        record.onRepaint = [this] { render(); };
        record.onFrameDone = [this] { frameCallbackArrived(); };
    }

    ~Native()
    {
        // A cap-timer tick may already be queued on the main thread, and the
        // Timer's destructor only stops the thread that posts them. The flag is
        // what makes the one in the queue fizzle rather than call into freed
        // storage - the same guard DisplayLink-Linux.cpp keeps, for the same
        // reason.
        *alive = false;

        // The record belongs to the View and outlives this - onLost fires from
        // ~View, which runs after ~GPUView has already destroyed the Pimpl - so
        // the hooks have to stop pointing here before the storage goes.
        record.onAvailable = [] {};
        record.onLost = [] {};
        record.onResized = [] {};
        record.onRepaint = [] {};
        record.onFrameDone = [] {};

        stopContinuous();
        destroySwapchain();
        destroySurface();
    }

    // ------------------------------------------------------- the ViewSurface

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

        // One frame straight away. On a view that animates this is what starts
        // the loop - continuous mode is paced by frame callbacks and no
        // callback arrives until something has been presented - and on one that
        // does not, it is the first picture, which nothing else would ask for
        // until the app repainted.
        if (continuous)
            continuousTick(true);
        else
            render();
    }

    // Fires *before* the wl_surface is torn down, so everything built over it
    // has to go here rather than at the next frame: a VkSwapchainKHR outliving
    // the wl_surface it was created from is a use-after-free inside the driver,
    // not a handle that reports an error.
    void surfaceLost()
    {
        destroySwapchain();
        destroySurface();
    }

    void surfaceResized()
    {
        swapchainStale = true;

        // The rebuild happens at the next frame rather than here, so a live
        // resize that reports twenty sizes builds one swapchain. This is what
        // asks for that frame: a compositor that resized a surface is not
        // obliged to repaint it, and a view that is not animating would
        // otherwise keep showing the old size until something else asked.
        view.repaint();
    }

    void frameCallbackArrived()
    {
        // The one thing a frame callback means outside continuous mode is that
        // the last frame was taken, which nothing here is waiting to hear.
        if (continuous)
            continuousTick(false);
    }

    float surfaceScale() const
    {
        if (record.surface != nullptr && record.scale > 0.f)
            return record.scale;

        return Graphics::linuxDefaultBackingScale;
    }

    // ------------------------------------------------------ continuous mode

    void startContinuous()
    {
        // Rebuilt rather than kept, so `time` starts at zero each time the view
        // is animated rather than counting from the first time it ever was.
        stampedTick = Threads::DisplayLink::timedTick(
            [this](Threads::FrameTime time)
            {
                view.update(time);
                render();
            });

        pacingStarted = false;
        startCapTimer();
        continuousTick(true);
    }

    void stopContinuous()
    {
        capTimer.reset();
        stampedTick = [] {};
        pacingStarted = false;
    }

    // The cap's own wake-up, and it exists because of one asymmetry with a
    // display link: a tick this view *skips* presents nothing, so the surface
    // is never committed, so no further frame callback arrives and the loop
    // stops at the first skipped tick. A DisplayLink has no such problem - the
    // display keeps refreshing whether or not anything was drawn.
    //
    // So while a cap is set, continuous mode has two sources of ticks - the
    // compositor's callback and this timer at the cap's own rate - and the
    // divider below drops whichever of the two arrives early. Uncapped, there
    // is no timer and the callback is the whole of the pacing.
    void startCapTimer()
    {
        capTimer.reset();

        if (continuous && maxFps > 0)
            capTimer.emplace(
                [this, guard = alive]
                {
                    if (*guard)
                        continuousTick(false);
                },
                maxFps);
    }

    // The setMaxFps divider, which is DisplayLink::rateLimited written out -
    // that one is private to DisplayLink and this file has no link to wrap.
    // Skips ticks until the cap's interval has accumulated, firing on whichever
    // tick lands closest to due, and carries the remainder so an uneven cap
    // holds as an average. The half-tick grace is not decoration: without it,
    // 60 on a 120 Hz compositor misses the interval by float dust and runs at
    // 40.
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

    // `force` is the frame that starts the loop, which has no interval to wait
    // out and would otherwise be paced against whatever the divider last saw.
    void continuousTick(bool force)
    {
        if (!continuous)
            return;

        // Nothing to present to - a hidden window, a device with no swapchain
        // support, a surface that has not arrived yet. The tick is dropped
        // whole rather than at render(), because update() advancing an
        // animation for a frame that will never be drawn is a step the view
        // silently loses.
        if (swapchain == VK_NULL_HANDLE)
            return;

        if (!force && !tickIsDue())
            return;

        stampedTick();
    }

    // ------------------------------------------------------- surface objects

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

        // The two opaque pointers, and the whole of what this file does with
        // them. vulkan_wayland.h declares both types itself, so nothing here
        // needs a Wayland header.
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

        // Asked per surface rather than assumed from the extension: a device
        // may present to one surface and not another, and the queue this
        // backend renders on is the only one there is - so a "no" here means
        // the view stays off-screen rather than that a different queue is
        // found.
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

    // Said once per process: a machine with no window-system integration is a
    // headless ICD or a loader without WSI, which is a normal thing to be and
    // not an error - the view renders off-screen exactly as it did before there
    // was a swapchain at all.
    static void reportNoPresentation()
    {
        static auto reported = false;

        if (reported)
            return;

        reported = true;
        LOG("Vulkan: no swapchain support on this device, so a GPUView renders "
            "off-screen only");
    }

    // ------------------------------------------------------------- swapchain

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

        // BGRA8 unorm, which is what the off-screen snapshot renders into and
        // what the other two backends give their swapchains, so a shader that
        // writes the right colours on one writes them on all three. Not an sRGB
        // *format*: that would have the hardware encode on write, and nothing
        // in eacp expects it to.
        //
        // It is also what RenderPipelineDescriptor::colorFormat defaults to, so
        // a surface that does not offer it - none seen yet - would leave every
        // pipeline compiled against a format the pass does not have, which
        // under dynamic rendering is a driver refusal at the draw. Taking the
        // first offered anyway is the honest half of that: the window still
        // presents, and an app that names its own colorFormat still works.
        for (const auto& format: formats)
            if (format.format == VK_FORMAT_B8G8R8A8_UNORM
                && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                return format;

        return formats[0];
    }

    // MAILBOX where it is offered, so a renderer faster than the display drops
    // frames instead of blocking on the acquire; FIFO otherwise, which the spec
    // guarantees every surface supports.
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

    // **Wayland answers 0xFFFFFFFF to "how big is this surface", and it means
    // "you decide".** There is no server-side size for a wl_surface - the
    // buffer the client attaches is the size - so the extent comes from the
    // record the window backend keeps current, clamped to what the driver will
    // make. Any other window system that did report a size would be taken at
    // its word.
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

        // A surface with no pixels yet - a window that has been created and not
        // laid out, one that is hidden - is not a failure, and the next
        // onResized brings the rebuild round again.
        if (extent.width == 0 || extent.height == 0)
            return false;

        const auto format = chooseFormat(physical);

        if (format.format == VK_FORMAT_UNDEFINED)
            return false;

        // One more than the compositor's minimum, which is the standard
        // trade: the minimum is what it takes to present at all, and one spare
        // is what lets the renderer draw the next frame while the compositor
        // still holds the last.
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

        // Colour attachment and nothing else: a swapchain image is drawn into
        // and presented, never sampled and never read back - the snapshot path
        // renders into a texture of its own instead - and every extra usage bit
        // is one a compositor is allowed to refuse.
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;

        // Taken as it is rather than asked to be IDENTITY: a rotated output
        // that would have to rotate the buffer itself is the compositor's
        // business, and naming its own transform is what tells it there is
        // nothing to undo.
        info.preTransform = capabilities.currentTransform;
        info.compositeAlpha = chooseCompositeAlpha(capabilities);
        info.presentMode = choosePresentMode(physical);
        info.clipped = VK_TRUE;

        // The driver may reuse the old swapchain's images and memory rather
        // than allocating a second set and freeing the first, which is what
        // makes a live resize cheap. The old handle is still ours to destroy
        // afterwards.
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
            // A VulkanTextureData like any render target's, with one flag
            // changed: `presentable` is what makes restingUse() answer
            // PRESENT_SRC_KHR, so the image leaves every pass ready for
            // vkQueuePresentKHR and RenderPass::end needs no swapchain case.
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

    // **One multisample buffer and one depth buffer for the whole swapchain**,
    // not a pair per image. Both are scratch space *within* a frame - the pass
    // renders into the multisample image and resolves out of it before it ends,
    // and nothing outside the frame reads either - so a copy per image would be
    // three times the memory for the same picture. The other two backends keep
    // one of each for the same reason.
    //
    // What is per image, and is dealt with by lendCompanions, is the layout
    // each of them is in.
    bool createCompanions()
    {
        auto& gpu = Device::shared();
        auto& context = getVulkanContext(gpu);

        companions = {};
        companions.format = swapchainFormat;
        companions.width = swapchainWidth;
        companions.height = swapchainHeight;

        // A count the device refuses is dropped to one rather than refusing the
        // whole view, which is what Texture does for a target the caller built
        // - here the caller is the view, and a window that draws nothing is a
        // worse answer than one that draws without multisampling.
        companions.sampleCount =
            sampleCount > 1 && gpu.supportsSampleCount(sampleCount) ? sampleCount
                                                                    : 1;

        if (companions.sampleCount > 1
            && !createVulkanMultisampleCompanion(context, companions))
            return false;

        // A failed depth buffer leaves a view that can still be drawn into
        // without a depth test, which is what hasDepth() answers and what every
        // pass already branches on.
        if (depthEnabled || stencilEnabled)
            createVulkanDepthCompanion(context, companions, stencilEnabled, false);

        return true;
    }

    // **One acquire semaphore per frame in flight, one render-finished
    // semaphore per swapchain image.** The pairing is not arbitrary: the
    // acquire belongs to the frame that waits on it, and the render-finished
    // belongs to the image the present waits for - and reaching the same image
    // twice while its first present had not been picked up is precisely the
    // case a per-frame array of the second gets wrong.
    bool createSemaphores()
    {
        const auto vulkanDevice = getVulkanShared().getDevice();

        // At most one fewer than there are images, or the CPU would be allowed
        // to have every image outstanding and the acquire would have nothing to
        // hand back.
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

    // Everything the swapchain owns except the VkSwapchainKHR itself, which the
    // next vkCreateSwapchainKHR wants as its oldSwapchain.
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

        // The images belong to the swapchain and go with it; the views over
        // them are this backend's.
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

    // The one place a full stall is paid, and it is paid on a rebuild and on
    // teardown rather than per frame. The timeline covers everything this
    // Device submitted; the device wait covers what it does not - a present
    // whose semaphore the presentation engine has not picked up yet, which is
    // exactly the semaphore about to be destroyed.
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

        // Nothing usable came back, so the old handle goes too and the next
        // frame starts from scratch. Left stale so it tries again rather than
        // giving up on a window that is merely not laid out yet.
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

    // ---------------------------------------------------------- the frame

    // The shared companions, and their layouts, lent to whichever image this
    // frame renders into - see createCompanions. The allocations are
    // deliberately not copied: `companions` owns those, and an image that
    // carried one could be told to release it.
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

    // And taken back with whatever layouts the frame left them in, so the next
    // frame - into a different image - barriers from where they actually are.
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

    // **Not re-entrant, and it has to say so.** Everything that reaches this -
    // a frame callback, a repaint, a resize - can also be reached from inside
    // view.render(), which is app code and is free to call repaint(). A second
    // frame inside the first would acquire a second image against a semaphore
    // the first is still waiting on, and a rebuild inside one would move the
    // very VulkanTextureData the open frame is recording against.
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

        // The CPU throttle, and the whole of it. This slot's acquire semaphore
        // was waited on by the frame that last used it, so it cannot be handed
        // to vkAcquireNextImageKHR again until that frame has run - and the
        // timeline value the frame submitted is what says it has. `slots` of
        // them is therefore exactly "this many frames may be outstanding".
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

        // Out of date acquires nothing and signals nothing, so the frame is
        // dropped and the swapchain rebuilt before the next one. Timeout and
        // not-ready are the same shape: no image, no signalled semaphore,
        // nothing to undo.
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR)
        {
            swapchainStale = true;
            return;
        }

        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
            return;

        // Suboptimal *did* hand over an image and *did* signal the semaphore,
        // so this frame is drawn and presented and the rebuild waits until
        // afterwards. Dropping it here would leave a signalled semaphore behind
        // for the next frame to trip over.
        if (acquired == VK_SUBOPTIMAL_KHR)
            swapchainStale = true;

        auto& target = images[static_cast<int>(imageIndex)];

        // The contents of an acquired image are undefined and the layout it was
        // last left in is not to be relied on, so the tracking is reset rather
        // than carried over from the last time this image came round. See
        // imageAcquired for why the stage in it is not NONE.
        target.use = imageAcquired;
        lendCompanions(target);

        auto drawable = VulkanDrawable {};
        drawable.swapchain = swapchain;
        drawable.imageIndex = imageIndex;
        drawable.target = &target;
        drawable.acquired = acquireSemaphores[currentSlot];
        drawable.renderFinished = renderFinished[static_cast<int>(imageIndex)];

        // Before the present, because the request rides on the surface's next
        // commit and the present is the commit. See
        // ViewSurface::requestFrameCallback.
        record.requestFrameCallback();

        {
            // Multisampling and depth are the drawable's own, on the
            // VulkanTextureData the frame is handed, so the other two arguments
            // stay null exactly as they do on the off-screen path.
            auto frame = Frame(gpu, &drawable, nullptr, nullptr);
            view.render(frame);
        }
        // The Frame destructor submitted with the two semaphores and presented.

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

        // Both mean the swapchain no longer matches the surface: out of date
        // because it cannot be presented at all, suboptimal because it can but
        // the compositor is having to work to do it.
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
            swapchainStale = true;

        // The initial value, meaning the frame never presented - the driver
        // would not open a command buffer, or refused the submission. The image
        // is neither here nor there; what matters is that its acquire semaphore
        // was signalled and nothing ever waited on it, and reusing a signalled
        // semaphore is undefined. Rebuilding is what makes that impossible,
        // since the semaphores go with the swapchain.
        if (result == VK_NOT_READY)
            swapchainStale = true;
    }

    // A lost device is the end of this view's presenting, and deliberately not
    // the start of a recovery: rebuilding the VkDevice would mean rebuilding
    // every Buffer, Texture and pipeline created on the old one, and the
    // machinery for that (Device::Native's replacement, the live-view registry
    // D3D12 keeps) does not exist on this backend. So onDeviceRestored does not
    // fire, and the README says so.
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

    // One per swapchain image, each describing that image as a render target;
    // and the multisample and depth buffers all of them share.
    Vector<VulkanTextureData> images;
    VulkanTextureData companions;

    Vector<VkSemaphore> acquireSemaphores;
    Vector<VkSemaphore> renderFinished;

    // The timeline value each in-flight slot's last frame submitted, waited on
    // before that slot's acquire semaphore is used again.
    Vector<std::uint64_t> slotValues;
    int currentSlot = 0;

    bool swapchainStale = false;
    bool companionsStale = false;
    bool deviceLost = false;
    bool rendering = false;

    // update() + render(), with the FrameTime stamped on by the same helper the
    // display link uses, so a continuous view here gets exactly the timing one
    // gets on the other two backends.
    Callback stampedTick = [] {};

    std::optional<Threads::Timer> capTimer;
    std::chrono::steady_clock::time_point lastTick;
    double accumulated = 0.0;
    bool pacingStarted = false;

    // Shared with every cap-timer tick already posted to the main thread, and
    // cleared by ~Native. See the note there.
    std::shared_ptr<std::atomic<bool>> alive =
        std::make_shared<std::atomic<bool>>(true);
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

    // The stencil plane lives in the depth attachment, so turning the
    // attachment off takes the plane with it rather than leaving hasStencil()
    // claiming a buffer that is gone.
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
    impl->startCapTimer();
}

int GPUView::maxFps() const
{
    return impl->maxFps;
}

void GPUView::setFramesInFlight(int count)
{
    impl->framesInFlight = count < 1 ? 1 : count;

    // The count is applied where the semaphores are made, so a change while a
    // swapchain is up is a rebuild. Rare - the documentation says to set this
    // before the first frame - and cheaper than a second code path.
    if (impl->swapchain != VK_NULL_HANDLE)
        impl->swapchainStale = true;
}

int GPUView::framesInFlight() const
{
    return impl->framesInFlight;
}

void GPUView::resized()
{
    Graphics::View::resized();

    // Nothing more to do here, and that is the design rather than an omission:
    // the swapchain follows the *surface's* pixel size, which is the view's
    // bounds times the compositor's scale and is the window backend's to work
    // out. It reports the result through ViewSurface::onResized, which arrives
    // behind this call.
}

void GPUView::backingScaleChanged()
{
    Graphics::View::backingScaleChanged();

    // The buffer size changes with the scale, but Wayland reports the two
    // separately - wp_fractional_scale carries no size with it - so the rebuild
    // is onResized's and this is only the news reaching app code, which is
    // where a glyph atlas rasterized at the old scale is rebuilt.
    onBackingScaleChanged(backingScale());
}

float GPUView::backingScale() const
{
    if (renderScale > 0.f)
        return renderScale;

    return impl->surfaceScale();
}

void GPUView::paint(Graphics::Context&)
{
    // Never called on Linux: there is no 2D Context and nothing composites a
    // view tree, so the on-demand render path is ViewSurface::onRepaint, which
    // View::repaint() reaches directly.
}

void GPUView::renderNow()
{
    impl->render();
}

// Off-screen GPU snapshot for View::renderToImage, mirroring GPUView-Apple.mm
// and GPUView-Windows.cpp: render() draws into a texture this owns through a
// Frame that waits instead of presenting, then the texture is read back and
// swizzled from premultiplied BGRA to the straight RGBA an Image holds.
//
// Independent of the swapchain in both directions: it works before there is one
// and while there is one, it neither acquires an image nor presents, and it is
// what runs under EACP_HEADLESS where there is no compositor at all.
//
// The target is a plain GPU::Texture rather than a hand-built image, which is
// what this backend has that the other two do not: a render target created with
// renderTarget grows its own multisample companion and its own depth buffer, so
// the multisampling and the depth the view asked for come along by naming them
// in the descriptor, and OffscreenTarget's msaaTexture and depthTexture stay
// null.
//
// Empty on every failure - no device, an empty view, a texture the device would
// not make. An invalid Image says "there are no pixels", where a blank one of
// the right size would be a picture of nothing that a caller could compare
// against and believe.
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

    // BGRA rather than RGBA, so the bytes that come back are laid out the way
    // the swapchain lays them out and the conversion below is the same one on
    // both paths.
    descriptor.format = TextureFormat::BGRA8Unorm;
    descriptor.renderTarget = true;

    // A count the device refuses would make the whole texture invalid and the
    // snapshot empty, where a view asking for more samples than the device has
    // should still get a picture. Texture refuses it for a target the caller
    // built; here the caller is this function.
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

        auto frame = Frame(device, target);

        // Rendered as a view of this scale: backingScale() answers it for the
        // duration, so a view sizing its geometry from the scale draws the
        // snapshot at the size that was asked for rather than at the screen's.
        renderScale = scale;
        render(frame);
        renderScale = 0.f;
    }
    // The Frame destructor submitted everything and waited, so the texture is
    // ready to be read.

    const auto pixelCount =
        static_cast<std::size_t>(pixelWidth) * static_cast<std::size_t>(pixelHeight);

    auto bgra = Vector<std::uint8_t>(static_cast<int>(pixelCount * 4));

    texture.read(bgra.data());

    auto image = Graphics::Image {};
    auto* dst = image.prepareForOverwrite(pixelWidth, pixelHeight);

    if (dst == nullptr)
        return {};

    // BGRA8 premultiplied - what a render target holds, and what the compositor
    // treats the swapchain as - into the straight RGBA an Image holds.
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
