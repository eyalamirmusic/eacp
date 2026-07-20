#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include "Texture.h"

#include "../Device/Device.h"

#include <eacp/Core/ObjC/CFRef.h>
#include <eacp/Core/ObjC/ObjC.h>

#include <cmath>

namespace eacp::GPU
{
namespace
{
MTLPixelFormat toMetalFormat(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::BGRA8Unorm:
            return MTLPixelFormatBGRA8Unorm;
        case TextureFormat::R8Unorm:
            return MTLPixelFormatR8Unorm;
        default:
            return MTLPixelFormatRGBA8Unorm;
    }
}

// Camera/video pixel buffers reach us as 32-bit BGRA (what the capture path
// requests); planar formats such as NV12 are a later addition.
bool toMetalFormat(CVPixelBufferRef pixelBuffer, MTLPixelFormat& out)
{
    switch (CVPixelBufferGetPixelFormatType(pixelBuffer))
    {
        case kCVPixelFormatType_32BGRA:
            out = MTLPixelFormatBGRA8Unorm;
            return true;
        case kCVPixelFormatType_32RGBA:
            out = MTLPixelFormatRGBA8Unorm;
            return true;
        default:
            return false;
    }
}
} // namespace

struct Texture::Native
{
    Native(Device& device, const TextureDescriptor& descriptor, const void* pixels)
        : width(descriptor.width)
        , height(descriptor.height)
        , pixelStride(bytesPerPixel(descriptor.format))
    {
        auto metalDevice = (__bridge id<MTLDevice>) device.nativeDevice();

        if (metalDevice == nil || width <= 0 || height <= 0)
            return;

        auto textureDescriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:toMetalFormat(descriptor.format)
                                         width:(NSUInteger) width
                                        height:(NSUInteger) height
                                     mipmapped:NO];
        textureDescriptor.usage = MTLTextureUsageShaderRead;

        texture = [metalDevice newTextureWithDescriptor:textureDescriptor];

        // The default storage mode keeps replaceRegion valid on every Mac
        // generation; it handles the CPU-to-GPU synchronisation itself.
        if (texture.get() != nil && pixels != nullptr)
            update(pixels, 0);
    }

    // Zero-copy wrap of a CVPixelBuffer: the texture cache maps the buffer's
    // IOSurface straight into an MTLTexture. cvTexture owns that mapping and
    // keeps it alive for the texture's lifetime.
    Native(Device& device, void* pixelBufferHandle)
    {
        auto metalDevice = (__bridge id<MTLDevice>) device.nativeDevice();
        auto cache = (CVMetalTextureCacheRef) device.nativeTextureCache();
        auto pixelBuffer = (CVPixelBufferRef) pixelBufferHandle;

        if (metalDevice == nil || cache == nullptr || pixelBuffer == nullptr)
            return;

        MTLPixelFormat metalFormat;

        if (!toMetalFormat(pixelBuffer, metalFormat))
            return;

        width = (int) CVPixelBufferGetWidth(pixelBuffer);
        height = (int) CVPixelBufferGetHeight(pixelBuffer);

        CVMetalTextureRef mapped = nullptr;
        auto status = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault,
                                                                cache,
                                                                pixelBuffer,
                                                                nullptr,
                                                                metalFormat,
                                                                (size_t) width,
                                                                (size_t) height,
                                                                0,
                                                                &mapped);

        if (status != kCVReturnSuccess || mapped == nullptr)
            return;

        cvTexture.reset(mapped);

        // The MTLTexture is owned by the CVMetalTexture mapping; retain it so
        // the Ptr's release on destruction stays balanced.
        texture.reset(CVMetalTextureGetTexture(mapped));
    }

    void update(const void* pixels, std::size_t bytesPerRow)
    {
        updateRegion(0, 0, width, height, pixels, bytesPerRow);
    }

    // Both update() overloads land here; the whole-texture one is just the full
    // rect, so there is a single replaceRegion call to reason about.
    void updateRegion(int x,
                      int y,
                      int regionWidth,
                      int regionHeight,
                      const void* pixels,
                      std::size_t bytesPerRow)
    {
        if (texture.get() == nil || pixels == nullptr || width <= 0 || height <= 0)
            return;

        if (regionWidth <= 0 || regionHeight <= 0)
            return;

        // Metal raises on a region that leaves the texture, so an out-of-bounds
        // request is dropped rather than clamped — see the header for why
        // clamping would be worse than doing nothing.
        if (x < 0 || y < 0 || x + regionWidth > width || y + regionHeight > height)
            return;

        auto stride = bytesPerRow != 0 ? bytesPerRow
                                       : (std::size_t) (regionWidth * pixelStride);

        [texture.get() replaceRegion:MTLRegionMake2D((NSUInteger) x,
                                                     (NSUInteger) y,
                                                     (NSUInteger) regionWidth,
                                                     (NSUInteger) regionHeight)
                         mipmapLevel:0
                           withBytes:pixels
                         bytesPerRow:(NSUInteger) stride];
    }

    int width = 0;
    int height = 0;

    // Bytes per pixel of the texture's format; the CV-wrapped path stays at 4
    // because those buffers are always 32-bit BGRA/RGBA.
    int pixelStride = 4;
    ObjC::Ptr<NSObject<MTLTexture>> texture;
    CFRef<CVMetalTextureRef> cvTexture;
};

Texture::Texture(Device& device,
                 const TextureDescriptor& descriptor,
                 const void* pixels)
    : impl(device, descriptor, pixels)
{
}

Texture::Texture(Device& device, void* nativePixelBuffer)
    : impl(device, nativePixelBuffer)
{
}

void Texture::update(const void* pixels, std::size_t bytesPerRow)
{
    impl->update(pixels, bytesPerRow);
}

void Texture::update(const Graphics::Rect& region,
                     const void* pixels,
                     std::size_t bytesPerRow)
{
    // Texels are whole; round rather than truncate so a rect built from
    // accumulated float arithmetic lands on the texel it is nearest to.
    impl->updateRegion((int) std::lround(region.x),
                       (int) std::lround(region.y),
                       (int) std::lround(region.w),
                       (int) std::lround(region.h),
                       pixels,
                       bytesPerRow);
}

int Texture::width() const
{
    return impl->width;
}

int Texture::height() const
{
    return impl->height;
}

bool Texture::isValid() const
{
    return impl->texture.get() != nil;
}

void* Texture::nativeTexture() const
{
    return (__bridge void*) impl->texture.get();
}

void* Texture::nativeReadView() const
{
    return nullptr;
}
} // namespace eacp::GPU
