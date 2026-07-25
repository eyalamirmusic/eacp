// vulkan.h only declares VkImportMetalIOSurfaceInfoEXT when the platform is
// opted into, and TextureImport-Vulkan.h pulls vulkan.h in, so this comes first.
#define VK_USE_PLATFORM_METAL_EXT

#import <CoreVideo/CoreVideo.h>

#include "TextureImport-Vulkan.h"

#include "../Vulkan/VulkanContext.h"
#include "../Vulkan/VulkanTypes.h"

// The Apple half of zero-copy pixel-buffer import. A CVPixelBuffer created
// IOSurface-backed - which is what every AVFoundation capture frame is - carries
// an IOSurface, and VK_EXT_metal_objects lets that surface be the backing store
// of a VkImage. MoltenVK then builds the MTLTexture from the same IOSurface the
// Metal backend's CVMetalTextureCache would have, so the two backends reach the
// same place: the frame is sampled where it was captured.

namespace eacp::GPU::detail
{
namespace
{
// Only the packed 32-bit formats, matching the Metal backend's wrap path: a
// capture frame is BGRA there too, and a biplanar YUV buffer would need a
// per-plane view Texture has no shape for.
bool toVulkanFormat(OSType pixelFormat, VkFormat& format)
{
    if (pixelFormat == kCVPixelFormatType_32BGRA)
    {
        format = VK_FORMAT_B8G8R8A8_UNORM;
        return true;
    }

    if (pixelFormat == kCVPixelFormatType_32RGBA)
    {
        format = VK_FORMAT_R8G8B8A8_UNORM;
        return true;
    }

    return false;
}
} // namespace

VulkanTexture importPixelBuffer(void* pixelBufferHandle, VkImageUsageFlags usage)
{
    auto& context = getVulkanContext();
    auto pixelBuffer = (CVPixelBufferRef) pixelBufferHandle;

    if (!context.isValid() || !context.canImportSurfaces() || pixelBuffer == nullptr)
        return {};

    // A buffer allocated without kCVPixelBufferIOSurfacePropertiesKey has no
    // surface to share, so there is nothing to import - it is plain CPU memory.
    auto surface = CVPixelBufferGetIOSurface(pixelBuffer);

    if (surface == nullptr)
        return {};

    auto texture = VulkanTexture {};

    if (!toVulkanFormat(CVPixelBufferGetPixelFormatType(pixelBuffer),
                        texture.format))
        return {};

    texture.width = (int) CVPixelBufferGetWidth(pixelBuffer);
    texture.height = (int) CVPixelBufferGetHeight(pixelBuffer);

    auto import = VkImportMetalIOSurfaceInfoEXT {
        .sType = VK_STRUCTURE_TYPE_IMPORT_METAL_IO_SURFACE_INFO_EXT};
    import.ioSurface = surface;

    // The driver retains the surface for the image's lifetime, so the pixels
    // outlive the CVPixelBuffer handle the caller passed in.
    auto info = VkImageCreateInfo {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.pNext = &import;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = texture.format;
    info.extent = {(std::uint32_t) texture.width, (std::uint32_t) texture.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(context.getDevice(), &info, nullptr, &texture.image)
        != VK_SUCCESS)
        return {};

    // Reported as already in the layout the usage implies, rather than
    // transitioned into it. Metal has no image layouts at all, so on this driver
    // a barrier would be bookkeeping; worse, a transition out of UNDEFINED is
    // licensed to discard contents, and for a sampled import the contents are
    // the entire point. A driver that does track layouts wants an acquire from
    // VK_QUEUE_FAMILY_FOREIGN_EXT here instead, which is a dma-buf concern and
    // belongs with the code that adds one.
    texture.layout = (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0
                         ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                         : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    return texture;
}
} // namespace eacp::GPU::detail
