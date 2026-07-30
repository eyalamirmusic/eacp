#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include "Texture.h"

#include "../Device/Device.h"
#include "MipChain.h"

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
        case TextureFormat::RG8Unorm:
            return MTLPixelFormatRG8Unorm;
        case TextureFormat::RGBA16Float:
            return MTLPixelFormatRGBA16Float;
        case TextureFormat::RGBA32Float:
            return MTLPixelFormatRGBA32Float;
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
        , format(descriptor.format)
        , renderTarget(descriptor.renderTarget)
        , computeWrite(descriptor.computeWrite)
    {
        auto metalDevice = (__bridge id<MTLDevice>) device.nativeDevice();

        if (metalDevice == nil || width <= 0 || height <= 0)
            return;

        // Refused here rather than at the bind, so a format D3D12 cannot take a
        // typed store to fails the same way on both backends instead of working
        // on one of them. Nothing is created, so the texture is invalid rather
        // than a kernel output that quietly does nothing. See
        // supportsComputeWrite.
        if (computeWrite && !supportsComputeWrite(descriptor.format))
            return;

        // A chain is only worth asking for when there are pixels to build it
        // from: a render target or a kernel output has none at creation, so it
        // would get levels nothing ever writes and the sampler would read them.
        if (descriptor.mipmapped && pixels != nullptr)
            levels = mipLevelCount(width, height);

        auto textureDescriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:toMetalFormat(descriptor.format)
                                         width:(NSUInteger) width
                                        height:(NSUInteger) height
                                     mipmapped:levels > 1 ? YES : NO];
        textureDescriptor.usage = MTLTextureUsageShaderRead;

        // Set explicitly as well as through the `mipmapped` flag, so the count
        // the texture is created with and the count the upload loop fills cannot
        // come from two different pieces of arithmetic.
        textureDescriptor.mipmapLevelCount = (NSUInteger) levels;

        // A render target is written by the GPU and read by it, so it goes in
        // private storage and keeps the default (managed/shared) mode otherwise
        // - which is what makes replaceRegion valid on every Mac generation.
        if (renderTarget)
        {
            textureDescriptor.usage |= MTLTextureUsageRenderTarget;
            textureDescriptor.storageMode = MTLStorageModePrivate;
        }

        // A kernel's output only needs the write usage; the storage mode is
        // deliberately left alone, so a texture a kernel accumulates into can
        // still be seeded from the CPU with update().
        if (computeWrite)
            textureDescriptor.usage |= MTLTextureUsageShaderWrite;

        texture = [metalDevice newTextureWithDescriptor:textureDescriptor];

        if (renderTarget && descriptor.depth && texture.get() != nil)
            makeDepthTexture(metalDevice);

        // The default storage mode keeps replaceRegion valid on every Mac
        // generation; it handles the CPU-to-GPU synchronisation itself.
        if (texture.get() != nil && pixels != nullptr)
            update(pixels, 0);
    }

    // The depth buffer a pass into this texture attaches. Single-sampled,
    // because a texture target never multisamples, and private - the pass
    // clears it and stores nothing, so it is never read outside the GPU.
    void makeDepthTexture(id<MTLDevice> metalDevice)
    {
        auto depthDescriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                         width:(NSUInteger) width
                                        height:(NSUInteger) height
                                     mipmapped:NO];

        depthDescriptor.usage = MTLTextureUsageRenderTarget;
        depthDescriptor.storageMode = MTLStorageModePrivate;

        depthTexture = [metalDevice newTextureWithDescriptor:depthDescriptor];
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
        if (levels > 1)
        {
            uploadChain(pixels, bytesPerRow);
            return;
        }

        updateRegion(0, 0, width, height, pixels, bytesPerRow);
    }

    // Every level, from the chain built on the CPU. Not
    // generateMipmapsForTexture, which would work here and has no counterpart on
    // D3D12 - see MipChain.h for why one filter shared by both backends is worth
    // more than a free one on this side only.
    void uploadChain(const void* pixels, std::size_t bytesPerRow)
    {
        if (texture.get() == nil || pixels == nullptr)
            return;

        const auto chain = buildMipChain(pixels, width, height, format, bytesPerRow);

        if (!chain.isValid())
            return;

        for (auto level = 0; level < chain.levelCount() && level < levels; ++level)
        {
            const auto levelWidth = mipExtent(width, level);
            const auto levelHeight = mipExtent(height, level);

            [texture.get()
                 replaceRegion:MTLRegionMake2D(0,
                                               0,
                                               (NSUInteger) levelWidth,
                                               (NSUInteger) levelHeight)
                   mipmapLevel:(NSUInteger) level
                     withBytes:chain.level(level)
                   bytesPerRow:(NSUInteger) (levelWidth * pixelStride)];
        }
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
    TextureFormat format = TextureFormat::RGBA8Unorm;

    // 1 unless a chain was asked for and there were pixels to build one from.
    int levels = 1;

    bool renderTarget = false;
    bool computeWrite = false;
    ObjC::Ptr<NSObject<MTLTexture>> texture;
    ObjC::Ptr<NSObject<MTLTexture>> depthTexture;
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

bool Texture::isRenderTarget() const
{
    return impl->renderTarget && impl->texture.get() != nil;
}

int Texture::mipLevels() const
{
    return impl->levels;
}

bool Texture::isComputeWritable() const
{
    return impl->computeWrite && impl->texture.get() != nil;
}

bool Texture::hasDepth() const
{
    return impl->depthTexture.get() != nil;
}

void* Texture::nativeTexture() const
{
    return (__bridge void*) impl->texture.get();
}

void* Texture::nativeDepthTexture() const
{
    return (__bridge void*) impl->depthTexture.get();
}

void* Texture::nativeReadView() const
{
    return nullptr;
}
} // namespace eacp::GPU
