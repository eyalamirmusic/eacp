#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include "Texture.h"

#include "../Device/Device.h"
#include "MipChain.h"

#include <eacp/Core/ObjC/CFRef.h>
#include <eacp/Core/ObjC/ObjC.h>

#include <cmath>
#include <cstring>

namespace eacp::GPU
{
namespace
{
// **The guard is for iOS, and is inert on macOS as eacp configures it.** The
// four constants and Device::supportsBlockCompression carry the same
// availability the query does - macOS 11 and iOS 16.4 - and the macOS
// deployment target is already 11.0, while the iOS one is 14.0. So on macOS
// this compiles to the branch being taken, and on iOS it is what keeps the
// constants from warning against a target that predates them.
//
// The fallback below is unreachable rather than a default answer: the only
// caller is toMetalFormat, which only reaches here for a block format, and the
// constructor refuses one before that unless Device::supportsBlockCompression
// said yes - which needs the same version this branch does. If it ever were
// reached, MTLPixelFormatInvalid makes newTextureWithDescriptor: raise rather
// than hand back a texture in some other format.
MTLPixelFormat toMetalBlockFormat(TextureFormat format)
{
    if (@available(macOS 11.0, iOS 16.4, *))
    {
        switch (format)
        {
            case TextureFormat::BC1RGBA:
                return MTLPixelFormatBC1_RGBA;
            case TextureFormat::BC2RGBA:
                return MTLPixelFormatBC2_RGBA;
            case TextureFormat::BC3RGBA:
                return MTLPixelFormatBC3_RGBA;
            case TextureFormat::BC7RGBA:
                return MTLPixelFormatBC7_RGBAUnorm;

            case TextureFormat::RGBA8Unorm:
            case TextureFormat::BGRA8Unorm:
            case TextureFormat::R8Unorm:
            case TextureFormat::RG8Unorm:
            case TextureFormat::RGBA16Float:
            case TextureFormat::RGBA32Float:
            case TextureFormat::R32Float:
                break;
        }
    }

    return MTLPixelFormatInvalid;
}

// Exhaustive, so a format added without a Metal counterpart is a -Wswitch
// warning rather than a texture quietly created as RGBA8 - which is what the
// default this replaced would have made of every one of the block formats.
MTLPixelFormat toMetalFormat(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::RGBA8Unorm:
            return MTLPixelFormatRGBA8Unorm;
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
        case TextureFormat::R32Float:
            return MTLPixelFormatR32Float;

        case TextureFormat::BC1RGBA:
        case TextureFormat::BC2RGBA:
        case TextureFormat::BC3RGBA:
        case TextureFormat::BC7RGBA:
            return toMetalBlockFormat(format);
    }

    return MTLPixelFormatInvalid;
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
    Native(Device& deviceToUse,
           const TextureDescriptor& descriptor,
           const void* pixels)
        : device(&deviceToUse)
        , width(descriptor.width)
        , height(descriptor.height)
        , format(descriptor.format)
        , renderTarget(descriptor.renderTarget)
        , computeWrite(descriptor.computeWrite)
        , cube(descriptor.cube)
    {
        auto metalDevice = (__bridge id<MTLDevice>) deviceToUse.nativeDevice();

        if (metalDevice == nil || width <= 0 || height <= 0)
            return;

        // Refused rather than clamped, and refused before anything is created,
        // so a target the device cannot multisample is an invalid texture rather
        // than one that silently draws at a different count than the pipelines
        // compiled for it. See TextureDescriptor::sampleCount; ask
        // Device::supportsSampleCount to choose a number instead of finding out
        // here. A count on a texture nothing renders into is ignored, as depth
        // is, and a multisampled cube is refused with the other two below.
        if (renderTarget && descriptor.sampleCount > 1)
        {
            if (!deviceToUse.supportsSampleCount(descriptor.sampleCount))
                return;

            samples = descriptor.sampleCount;
        }

        // Refused here rather than at the bind, so a format D3D12 cannot take a
        // typed store to fails the same way on both backends instead of working
        // on one of them. Nothing is created, so the texture is invalid rather
        // than a kernel output that quietly does nothing. See
        // supportsComputeWrite.
        if (computeWrite && !supportsComputeWrite(descriptor.format))
            return;

        // A cube's faces are square and there are six of them, so a rectangle or
        // a GPU-written cube has nothing this could create. Refused here rather
        // than clamped or half-built, and on both backends identically - see
        // TextureDescriptor::cube.
        if (cube && (width != height || renderTarget || computeWrite))
            return;

        // A cube cannot be a render target at all, so it cannot be a
        // multisampled one either; refused here rather than left to look like it
        // worked.
        if (cube && descriptor.sampleCount > 1)
            return;

        // A compressed texture is a block of bytes the sampler decodes and
        // nothing else: there is no per-texel address for a pass or a kernel to
        // write to. Refused before anything is created, and on a device that has
        // no BC formats at all the format itself is refused - invalid rather
        // than quietly something else, which is the answer a refused sampleCount
        // already gives. See the block formats in Texture.h.
        if (isCompressedFormat(format))
        {
            if (renderTarget || computeWrite)
                return;

            if (!deviceToUse.supportsBlockCompression())
                return;
        }

        if (descriptor.mipLevels < 0)
            return;

        // A caller-supplied chain is taken as it is, and everything that would
        // make it a chain nobody could have supplied is refused rather than
        // reconciled - see TextureDescriptor::mipLevels for each of these.
        if (descriptor.mipLevels > 0)
        {
            if (descriptor.mipmapped || renderTarget || computeWrite || cube)
                return;

            // Nothing was handed over, so there is no chain to take. Refused
            // rather than created empty, which would be N levels the sampler
            // reads and nothing ever wrote - the very thing mipmapped avoids by
            // asking for pixels before it builds anything.
            if (pixels == nullptr)
                return;

            if (descriptor.mipLevels > mipLevelCount(width, height))
                return;

            levels = descriptor.mipLevels;
            suppliedChain = true;
        }
        // A chain eacp builds is only worth asking for when there are pixels to
        // build it from and a format it can average: a render target or a kernel
        // output has no pixels at creation, and a compressed one has no average,
        // so either would get levels nothing ever writes and the sampler would
        // read them.
        else if (descriptor.mipmapped && pixels != nullptr
                 && canBuildMipChain(format))
        {
            levels = mipLevelCount(width, height);
        }

        // Two descriptors for one texture, and the only difference is the shape:
        // a cube is six square slices Metal indexes with a direction, and the
        // rest of this constructor - the usage, the level count, the upload -
        // does not care which of the two it got.
        auto textureDescriptor =
            cube ? [MTLTextureDescriptor
                       textureCubeDescriptorWithPixelFormat:toMetalFormat(
                                                                descriptor.format)
                                                       size:(NSUInteger) width
                                                  mipmapped:levels > 1 ? YES : NO]
                 : [MTLTextureDescriptor
                       texture2DDescriptorWithPixelFormat:toMetalFormat(
                                                              descriptor.format)
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

        // The texture the pass renders into when this target multisamples; this
        // one becomes what it resolves into. Created after it and refused with
        // it, so a target is either both textures or invalid.
        if (samples > 1 && texture.get() != nil)
        {
            makeMultisampleTexture(metalDevice, toMetalFormat(descriptor.format));

            if (multisampleTexture.get() == nil)
            {
                texture.release();
                return;
            }
        }

        if (renderTarget
            && (descriptor.depth || descriptor.stencil
                || descriptor.sampleableDepth)
            && texture.get() != nil)
            makeDepthTexture(metalDevice,
                             descriptor.stencil,
                             descriptor.sampleableDepth);

        // The default storage mode keeps replaceRegion valid on every Mac
        // generation; it handles the CPU-to-GPU synchronisation itself.
        if (texture.get() != nil && pixels != nullptr)
            update(pixels, 0);
    }

    // The multisampled colour attachment that stands in front of the target when
    // it multisamples. Render-target usage only and private storage: nothing
    // samples it, reads it back or uploads to it - the resolve into the target
    // is the only way anything sees what it holds.
    void makeMultisampleTexture(id<MTLDevice> metalDevice, MTLPixelFormat format)
    {
        auto descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                               width:(NSUInteger) width
                                                              height:(NSUInteger) height
                                                           mipmapped:NO];

        descriptor.textureType = MTLTextureType2DMultisample;
        descriptor.sampleCount = (NSUInteger) samples;
        descriptor.usage = MTLTextureUsageRenderTarget;
        descriptor.storageMode = MTLStorageModePrivate;

        multisampleTexture = [metalDevice newTextureWithDescriptor:descriptor];
    }

    // The depth buffer a pass into this texture attaches, at the target's own
    // sample count - both APIs require every attachment of a pass to agree on
    // one - and private, the pass clearing it and storing nothing, so it is
    // never read outside the GPU.
    //
    // Unless it is asked to be. MTLTextureUsageShaderRead is the whole of what
    // sampleableDepth costs on a single-sampled target - the storage mode is
    // already Private and has to stay so, a depth format having no CPU layout to
    // share - and without it Metal refuses the sample whatever texture the
    // encoder is handed. It is still a flag rather than the default because the
    // same request costs D3D12 a typeless resource, a descriptor and a barrier
    // per pass.
    //
    // A *multisampled* target pays one texture more for it, because the read
    // usage is not the problem there: a shader eacp generated declares a depth2d
    // and Metal will not bind a depth2d_ms to one. So the buffer is resolved into
    // a single-sampled twin at the end of every pass and that is what the bind
    // hands over. See TextureDescriptor::sampleCount.
    void makeDepthTexture(id<MTLDevice> metalDevice,
                          bool withStencil,
                          bool sampleable)
    {
        const auto format = withStencil ? MTLPixelFormatDepth32Float_Stencil8
                                        : MTLPixelFormatDepth32Float;

        auto depthDescriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:format
                                         width:(NSUInteger) width
                                        height:(NSUInteger) height
                                     mipmapped:NO];

        depthDescriptor.usage = MTLTextureUsageRenderTarget;

        if (sampleable && samples == 1)
            depthDescriptor.usage |= MTLTextureUsageShaderRead;

        if (samples > 1)
        {
            depthDescriptor.textureType = MTLTextureType2DMultisample;
            depthDescriptor.sampleCount = (NSUInteger) samples;
        }

        depthDescriptor.storageMode = MTLStorageModePrivate;

        depthTexture = [metalDevice newTextureWithDescriptor:depthDescriptor];

        if (depthTexture.get() == nil)
            return;

        if (sampleable && samples > 1)
        {
            depthDescriptor.textureType = MTLTextureType2D;
            depthDescriptor.sampleCount = 1;
            depthDescriptor.usage =
                MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;

            resolvedDepthTexture =
                [metalDevice newTextureWithDescriptor:depthDescriptor];

            sampleableDepth = resolvedDepthTexture.get() != nil;
            return;
        }

        sampleableDepth = sampleable;
    }

    // Zero-copy wrap of a CVPixelBuffer: the texture cache maps the buffer's
    // IOSurface straight into an MTLTexture. cvTexture owns that mapping and
    // keeps it alive for the texture's lifetime.
    Native(Device& deviceToUse, void* pixelBufferHandle)
        : device(&deviceToUse)
    {
        auto metalDevice = (__bridge id<MTLDevice>) deviceToUse.nativeDevice();
        auto cache = (CVMetalTextureCacheRef) deviceToUse.nativeTextureCache();
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

    // Every slice of the texture, from one block of bytes: one face or six, one
    // level or a whole chain, whichever this texture was created as.
    //
    // bytesPerRow is the stride *within* a face, which is what makes a cube
    // assembled out of six separately loaded images uploadable without
    // repacking. It has to be 0 where the layout is tightly packed by
    // definition - a compressed texture, or a chain the caller supplied - and a
    // nonzero one there is dropped rather than used as a pitch it cannot be.
    // See Texture::update.
    void update(const void* pixels, std::size_t bytesPerRow)
    {
        if (texture.get() == nil || pixels == nullptr || width <= 0 || height <= 0)
            return;

        if (bytesPerRow != 0 && (suppliedChain || isCompressedFormat(format)))
            return;

        const auto stride =
            bytesPerRow != 0 ? bytesPerRow : levelBytesPerRow(format, width);

        // A face is an ordinary texture as far as everything below here is
        // concerned: its own slice, and its own chain built from its own pixels
        // rather than from its neighbours'. See TextureDescriptor::cube for the
        // order the six are in and why it is the same order on both backends.
        const auto faceBytes = stride * (std::size_t) levelRows(format, height);
        const auto faces = cube ? 6 : 1;

        for (auto face = 0; face < faces; ++face)
        {
            const auto* facePixels =
                (const unsigned char*) pixels + (std::size_t) face * faceBytes;

            if (suppliedChain)
                uploadSuppliedChain(face, facePixels);
            else if (levels > 1)
                uploadChain(face, facePixels, stride);
            else
                replaceSlice(face, 0, width, height, facePixels, stride);
        }
    }

    // Every level of one slice, from the chain built on the CPU. Not
    // generateMipmapsForTexture, which would work here and has no counterpart on
    // D3D12 - see MipChain.h for why one filter shared by both backends is worth
    // more than a free one on this side only.
    void uploadChain(int slice, const void* pixels, std::size_t bytesPerRow)
    {
        const auto chain = buildMipChain(pixels, width, height, format, bytesPerRow);

        if (!chain.isValid())
            return;

        for (auto level = 0; level < chain.levelCount() && level < levels; ++level)
        {
            const auto levelWidth = mipExtent(width, level);
            const auto levelHeight = mipExtent(height, level);

            replaceSlice(slice,
                         level,
                         levelWidth,
                         levelHeight,
                         chain.level(level),
                         levelBytesPerRow(format, levelWidth));
        }
    }

    // Every level of one slice, from the chain the caller built - the same loop
    // as the one above with the source walked rather than produced, since the
    // layout is the one MipChain packs. No filter runs anywhere here, which is
    // the entire point of TextureDescriptor::mipLevels.
    void uploadSuppliedChain(int slice, const void* pixels)
    {
        const auto* bytes = (const unsigned char*) pixels;

        for (auto level = 0; level < levels; ++level)
        {
            const auto levelWidth = mipExtent(width, level);
            const auto levelHeight = mipExtent(height, level);

            replaceSlice(slice,
                         level,
                         levelWidth,
                         levelHeight,
                         bytes,
                         levelBytesPerRow(format, levelWidth));

            bytes += levelBytes(format, levelWidth, levelHeight);
        }
    }

    // One rectangle of one slice of one level. bytesPerImage is 0 because this
    // is a 2D region: Metal reads that argument only where a single
    // replaceRegion covers more than one image, which is a 3D texture or a whole
    // array, and never for a cube face uploaded a slice at a time.
    //
    // Every caller passes the *whole* level, which is what makes this correct
    // for a compressed format: Metal wants a region either aligned to the 4x4
    // block grid or reaching the edge of the level, and the level's own bounds
    // do both. bytesPerRow is then bytes per block row, which is what
    // levelBytesPerRow gives.
    void replaceSlice(int slice,
                      int level,
                      int regionWidth,
                      int regionHeight,
                      const void* pixels,
                      std::size_t bytesPerRow)
    {
        [texture.get() replaceRegion:MTLRegionMake2D(0,
                                                    0,
                                                    (NSUInteger) regionWidth,
                                                    (NSUInteger) regionHeight)
                         mipmapLevel:(NSUInteger) level
                               slice:(NSUInteger) slice
                           withBytes:pixels
                         bytesPerRow:(NSUInteger) bytesPerRow
                       bytesPerImage:0];
    }

    // Where the region overload of update() lands, and the only replaceRegion
    // here that is not a whole level.
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

        // A cube has six rectangles this could mean, and nothing here says
        // which - see the header. Dropped rather than sent to +X.
        if (cube)
            return;

        // A compressed rectangle would have to land on the 4x4 block grid, so
        // the rect asked for and the rect written would differ by up to three
        // texels a side. Dropped rather than rounded, for the same reason an
        // out-of-bounds one is - see the header.
        if (isCompressedFormat(format))
            return;

        // Metal raises on a region that leaves the texture, so an out-of-bounds
        // request is dropped rather than clamped — see the header for why
        // clamping would be worse than doing nothing.
        if (x < 0 || y < 0 || x + regionWidth > width || y + regionHeight > height)
            return;

        auto stride = bytesPerRow != 0 ? bytesPerRow
                                       : levelBytesPerRow(format, regionWidth);

        [texture.get() replaceRegion:MTLRegionMake2D((NSUInteger) x,
                                                     (NSUInteger) y,
                                                     (NSUInteger) regionWidth,
                                                     (NSUInteger) regionHeight)
                         mipmapLevel:0
                           withBytes:pixels
                         bytesPerRow:(NSUInteger) stride];
    }

    // Both read() overloads land here, as the update() pair land in
    // updateRegion.
    //
    // Through a shared buffer rather than getBytes, because a render target is
    // in private storage and getBytes on one raises: the blit is the only way
    // off the device that works for every texture this class creates, and it is
    // what GPUView's own snapshot has always done.
    void readRegion(int x,
                    int y,
                    int regionWidth,
                    int regionHeight,
                    void* dst,
                    std::size_t bytesPerRow) const
    {
        auto source = (id<MTLTexture>) texture.get();

        if (source == nil || dst == nullptr || device == nullptr)
            return;

        if (regionWidth <= 0 || regionHeight <= 0)
            return;

        // Six faces and no argument to name one, exactly as the region upload
        // has none - see the header.
        if (cube)
            return;

        // Blocks rather than pixels, and no decoder at either end - see the
        // header for why a compressed read-back has nothing to hand back.
        if (isCompressedFormat(format))
            return;

        // Dropped rather than clamped, exactly as the upload side drops it.
        if (x < 0 || y < 0 || x + regionWidth > width || y + regionHeight > height)
            return;

        auto metalDevice = (__bridge id<MTLDevice>) device->nativeDevice();
        auto queue = (__bridge id<MTLCommandQueue>) device->nativeQueue();

        if (metalDevice == nil || queue == nil)
            return;

        @autoreleasepool
        {
            const auto rowBytes = (NSUInteger) levelBytesPerRow(format, regionWidth);
            const auto imageBytes = rowBytes * (NSUInteger) regionHeight;

            auto staging =
                [metalDevice newBufferWithLength:imageBytes
                                         options:MTLResourceStorageModeShared];

            if (staging == nil)
                return;

            auto commandBuffer = [queue commandBuffer];
            auto blit = [commandBuffer blitCommandEncoder];

            [blit copyFromTexture:source
                             sourceSlice:0
                             sourceLevel:0
                            sourceOrigin:MTLOriginMake((NSUInteger) x,
                                                       (NSUInteger) y,
                                                       0)
                              sourceSize:MTLSizeMake((NSUInteger) regionWidth,
                                                     (NSUInteger) regionHeight,
                                                     1)
                                toBuffer:staging
                       destinationOffset:0
                  destinationBytesPerRow:rowBytes
                destinationBytesPerImage:imageBytes];
            [blit endEncoding];

            // The queue is FIFO, so waiting for this copy waits for everything
            // committed before it - which is what makes the rule in the header
            // "committed" rather than "finished".
            [commandBuffer commit];
            [commandBuffer waitUntilCompleted];

            const auto stride =
                bytesPerRow != 0 ? bytesPerRow : (std::size_t) rowBytes;

            auto* out = (unsigned char*) dst;
            auto* in = (const unsigned char*) [staging contents];

            for (auto row = 0; row < regionHeight; ++row)
                std::memcpy(out + (std::size_t) row * stride,
                            in + (std::size_t) row * rowBytes,
                            (std::size_t) rowBytes);
        }
    }

    // The Device these belong to, for the queue a read-back's blit is committed
    // on. Held rather than passed in because a Texture is read through its own
    // handle, with no Device in sight.
    Device* device = nullptr;

    int width = 0;
    int height = 0;

    // Every size in this file comes out of the format through levelBytesPerRow
    // and levelBytes rather than out of a stored stride, because a compressed
    // format has no per-texel size to store. The CV-wrapped path leaves this at
    // RGBA8Unorm, which is the four bytes those buffers always carry.
    TextureFormat format = TextureFormat::RGBA8Unorm;

    // 1 unless a chain was asked for and there were pixels to build one from,
    // or the caller supplied one of its own.
    int levels = 1;

    // Whether those levels arrived with the pixels rather than being built from
    // level 0, which changes where each level's bytes come from and nothing
    // else. See TextureDescriptor::mipLevels.
    bool suppliedChain = false;

    bool renderTarget = false;
    bool computeWrite = false;

    // How many samples a pass into this target takes. 1 on everything that is
    // not a multisampled render target, and the count the multisample colour
    // texture and the depth buffer were both created at otherwise.
    int samples = 1;

    // Six square slices rather than one rectangle, which changes the descriptor
    // it was created from, the upload loop, and what the two region-shaped
    // entry points do - and nothing else.
    bool cube = false;

    // Whether that depth buffer was created able to be sampled as well as
    // attached, which is the one thing a bind through it can check.
    bool sampleableDepth = false;

    ObjC::Ptr<NSObject<MTLTexture>> texture;
    ObjC::Ptr<NSObject<MTLTexture>> multisampleTexture;
    ObjC::Ptr<NSObject<MTLTexture>> depthTexture;
    ObjC::Ptr<NSObject<MTLTexture>> resolvedDepthTexture;
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

void Texture::read(void* dst, std::size_t bytesPerRow) const
{
    impl->readRegion(0, 0, impl->width, impl->height, dst, bytesPerRow);
}

void Texture::read(const Graphics::Rect& region,
                   void* dst,
                   std::size_t bytesPerRow) const
{
    // Rounded rather than truncated, as update()'s region is: a rect built from
    // accumulated float arithmetic lands on the texel it is nearest to.
    impl->readRegion((int) std::lround(region.x),
                     (int) std::lround(region.y),
                     (int) std::lround(region.w),
                     (int) std::lround(region.h),
                     dst,
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

bool Texture::isCube() const
{
    return impl->cube && impl->texture.get() != nil;
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

bool Texture::hasStencil() const
{
    auto depth = (id<MTLTexture>) impl->depthTexture.get();

    return depth != nil
           && depth.pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
}

bool Texture::hasSampleableDepth() const
{
    return impl->sampleableDepth && impl->depthTexture.get() != nil;
}

int Texture::sampleCount() const
{
    return impl->samples;
}

void* Texture::nativeTexture() const
{
    return (__bridge void*) impl->texture.get();
}

void* Texture::nativeDepthTexture() const
{
    return (__bridge void*) impl->depthTexture.get();
}

void* Texture::nativeMultisampleTexture() const
{
    return (__bridge void*) impl->multisampleTexture.get();
}

void* Texture::nativeResolvedDepthTexture() const
{
    return (__bridge void*) impl->resolvedDepthTexture.get();
}

void* Texture::nativeReadView() const
{
    return nullptr;
}
} // namespace eacp::GPU
