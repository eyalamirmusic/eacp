// vulkan.h only declares VkMetalSurfaceCreateInfoEXT and its entry point when
// the platform is opted into, and GPUViewSurface-Vulkan.h pulls vulkan.h in, so
// this has to come first.
#define VK_USE_PLATFORM_METAL_EXT

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "GPUViewSurface-Vulkan.h"

#include "GPUView.h"
#include "GPUViewBacking-Apple.h"

#include <eacp/Graphics/Layers/ImmediateLayerClass.h>
#include <eacp/Graphics/Primitives/GraphicUtils.h>

// The Apple half of Vulkan presentation. MoltenVK draws into a CAMetalLayer, so
// the layer hosting here is the same as the Metal backend's in GPUView-Apple.mm
// -- an immediate (non-animating) CAMetalLayer added as a sublayer of the View's
// own backing layer, so the GPU content sits inside the normal View hierarchy
// and inherits its window, events and sizing. The only difference is that the
// layer is handed to vkCreateMetalSurfaceEXT instead of being drawn by Metal
// directly.

namespace eacp::GPU::detail
{
namespace
{
Class getImmediateMetalLayerClass()
{
    static auto cls =
        Graphics::makeImmediateLayerClass<CAMetalLayer>("EacpImmediateVulkanLayer");
    return cls;
}
} // namespace

SurfaceHost createSurfaceHost(GPUView& view, VkInstance instance)
{
    auto createMetalSurface = reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateMetalSurfaceEXT"));

    if (createMetalSurface == nullptr)
        return {};

    auto* layer = (CAMetalLayer*) [[getImmediateMetalLayerClass() alloc] init];

    // BGRA8 to match the swapchain format the render pipelines are built for,
    // and framebufferOnly because nothing samples the presented image.
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;

    auto* base = (__bridge CALayer*) view.getNativeLayer();

    if (base == nil)
    {
        [layer release];
        return {};
    }

    [base addSublayer:layer];

    auto info = VkMetalSurfaceCreateInfoEXT {.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
    info.pLayer = layer;

    auto host = SurfaceHost {};

    if (createMetalSurface(instance, &info, nullptr, &host.surface) != VK_SUCCESS)
    {
        [layer removeFromSuperlayer];
        [layer release];
        return {};
    }

    host.handle = layer;

    return host;
}

void destroySurfaceHost(SurfaceHost& host, VkInstance instance)
{
    if (host.surface != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(instance, host.surface, nullptr);

    if (host.handle != nullptr)
    {
        auto* layer = (__bridge CAMetalLayer*) host.handle;
        [layer removeFromSuperlayer];
        [layer release];
    }

    host = {};
}

void resizeSurfaceHost(const SurfaceHost& host, GPUView& view, float scale)
{
    if (host.handle == nullptr)
        return;

    auto* layer = (__bridge CAMetalLayer*) host.handle;
    auto bounds = Graphics::toCGRect(view.getLocalBounds());

    layer.frame = bounds;
    layer.contentsScale = scale;
    layer.drawableSize =
        CGSizeMake(bounds.size.width * scale, bounds.size.height * scale);
}

float surfaceBackingScale(GPUView& view)
{
    return (float) platformBackingScale(view);
}
} // namespace eacp::GPU::detail
