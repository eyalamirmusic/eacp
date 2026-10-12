#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include "Texture.h"

#include "../Device/Device.h"
#include "MipChain.h"
#include "TextureShape.h"

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
// deployment target is already 11.0, while the iOS one is 15.0. So on macOS
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
            case TextureFormat::BC1RGBASrgb:
                return MTLPixelFormatBC1_RGBA_sRGB;
            case TextureFormat::BC2RGBASrgb:
                return MTLPixelFormatBC2_RGBA_sRGB;
            case TextureFormat::BC3RGBASrgb:
                return MTLPixelFormatBC3_RGBA_sRGB;
            case TextureFormat::BC7RGBASrgb:
                return MTLPixelFormatBC7_RGBAUnorm_sRGB;
            case TextureFormat::BC4RUnorm:
                return MTLPixelFormatBC4_RUnorm;
            case TextureFormat::BC4RSnorm:
                return MTLPixelFormatBC4_RSnorm;
            case TextureFormat::BC5RGUnorm:
                return MTLPixelFormatBC5_RGUnorm;
            case TextureFormat::BC5RGSnorm:
                return MTLPixelFormatBC5_RGSnorm;
            case TextureFormat::BC6HRGBUfloat:
                return MTLPixelFormatBC6H_RGBUfloat;

            case TextureFormat::RGBA8Unorm:
            case TextureFormat::BGRA8Unorm:
            case TextureFormat::R8Unorm:
            case TextureFormat::RG8Unorm:
            case TextureFormat::RGBA16Float:
            case TextureFormat::RGBA32Float:
            case TextureFormat::R32Float:
            case TextureFormat::R16Float:
            case TextureFormat::RG16Float:
            case TextureFormat::RG32Float:
            case TextureFormat::R16Unorm:
            case TextureFormat::R32Uint:
            case TextureFormat::RGBA8UnormSrgb:
            case TextureFormat::BGRA8UnormSrgb:
            case TextureFormat::RG16Unorm:
            case TextureFormat::RGBA16Unorm:
            case TextureFormat::RGB10A2Unorm:
            case TextureFormat::RG11B10Float:
            case TextureFormat::RGB9E5Float:
            case TextureFormat::R8Uint:
            case TextureFormat::RG8Uint:
            case TextureFormat::RGBA8Uint:
            case TextureFormat::R16Uint:
            case TextureFormat::RG16Uint:
            case TextureFormat::RGBA16Uint:
            case TextureFormat::RG32Uint:
            case TextureFormat::RGBA32Uint:
            case TextureFormat::R8Sint:
            case TextureFormat::RG8Sint:
            case TextureFormat::RGBA8Sint:
            case TextureFormat::R16Sint:
            case TextureFormat::RG16Sint:
            case TextureFormat::RGBA16Sint:
            case TextureFormat::R32Sint:
            case TextureFormat::RG32Sint:
            case TextureFormat::RGBA32Sint:
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
        case TextureFormat::R16Float:
            return MTLPixelFormatR16Float;
        case TextureFormat::RG16Float:
            return MTLPixelFormatRG16Float;
        case TextureFormat::RG32Float:
            return MTLPixelFormatRG32Float;
        case TextureFormat::R16Unorm:
            return MTLPixelFormatR16Unorm;
        case TextureFormat::R32Uint:
            return MTLPixelFormatR32Uint;
        case TextureFormat::RGBA8UnormSrgb:
            return MTLPixelFormatRGBA8Unorm_sRGB;
        case TextureFormat::BGRA8UnormSrgb:
            return MTLPixelFormatBGRA8Unorm_sRGB;
        case TextureFormat::RG16Unorm:
            return MTLPixelFormatRG16Unorm;
        case TextureFormat::RGBA16Unorm:
            return MTLPixelFormatRGBA16Unorm;
        case TextureFormat::RGB10A2Unorm:
            return MTLPixelFormatRGB10A2Unorm;
        case TextureFormat::RG11B10Float:
            return MTLPixelFormatRG11B10Float;
        case TextureFormat::RGB9E5Float:
            return MTLPixelFormatRGB9E5Float;
        case TextureFormat::R8Uint:
            return MTLPixelFormatR8Uint;
        case TextureFormat::RG8Uint:
            return MTLPixelFormatRG8Uint;
        case TextureFormat::RGBA8Uint:
            return MTLPixelFormatRGBA8Uint;
        case TextureFormat::R16Uint:
            return MTLPixelFormatR16Uint;
        case TextureFormat::RG16Uint:
            return MTLPixelFormatRG16Uint;
        case TextureFormat::RGBA16Uint:
            return MTLPixelFormatRGBA16Uint;
        case TextureFormat::RG32Uint:
            return MTLPixelFormatRG32Uint;
        case TextureFormat::RGBA32Uint:
            return MTLPixelFormatRGBA32Uint;
        case TextureFormat::R8Sint:
            return MTLPixelFormatR8Sint;
        case TextureFormat::RG8Sint:
            return MTLPixelFormatRG8Sint;
        case TextureFormat::RGBA8Sint:
            return MTLPixelFormatRGBA8Sint;
        case TextureFormat::R16Sint:
            return MTLPixelFormatR16Sint;
        case TextureFormat::RG16Sint:
            return MTLPixelFormatRG16Sint;
        case TextureFormat::RGBA16Sint:
            return MTLPixelFormatRGBA16Sint;
        case TextureFormat::R32Sint:
            return MTLPixelFormatR32Sint;
        case TextureFormat::RG32Sint:
            return MTLPixelFormatRG32Sint;
        case TextureFormat::RGBA32Sint:
            return MTLPixelFormatRGBA32Sint;

        case TextureFormat::BC1RGBA:
        case TextureFormat::BC2RGBA:
        case TextureFormat::BC3RGBA:
        case TextureFormat::BC7RGBA:
        case TextureFormat::BC1RGBASrgb:
        case TextureFormat::BC2RGBASrgb:
        case TextureFormat::BC3RGBASrgb:
        case TextureFormat::BC7RGBASrgb:
        case TextureFormat::BC4RUnorm:
        case TextureFormat::BC4RSnorm:
        case TextureFormat::BC5RGUnorm:
        case TextureFormat::BC5RGSnorm:
        case TextureFormat::BC6HRGBUfloat:
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

MTLTextureType toMetalTextureType(TextureType kind)
{
    switch (kind)
    {
        case TextureType::TwoD:
            return MTLTextureType2D;
        case TextureType::Cube:
            return MTLTextureTypeCube;
        case TextureType::TwoDArray:
            return MTLTextureType2DArray;
        case TextureType::CubeArray:
            return MTLTextureTypeCubeArray;
        case TextureType::ThreeD:
            return MTLTextureType3D;
    }

    return MTLTextureType2D;
}

MTLTextureDescriptor* describeShape(const TextureShape& shape, MTLPixelFormat format)
{
    auto descriptor = [MTLTextureDescriptor new];
    descriptor.textureType = toMetalTextureType(shape.kind);
    descriptor.pixelFormat = format;
    descriptor.width = (NSUInteger) shape.width;
    descriptor.height = (NSUInteger) shape.height;
    descriptor.depth = (NSUInteger) shape.depth;
    descriptor.arrayLength = (NSUInteger) shape.layers;
    descriptor.mipmapLevelCount = (NSUInteger) shape.levels;
    return descriptor;
}

struct SubresourceViewKey
{
    int level = 0;
    int levelCount = 0;
    int slice = 0;
    int sliceCount = 0;

    bool operator==(const SubresourceViewKey&) const = default;
};

bool canGenerateMipsOnDevice(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::RGBA8Unorm:
        case TextureFormat::BGRA8Unorm:
        case TextureFormat::R8Unorm:
        case TextureFormat::RG8Unorm:
        case TextureFormat::RGBA16Float:
            return true;
        default:
            return false;
    }
}
} // namespace

// What RenderPipeline-Apple.mm maps a PixelFormat through, by way of
// textureFormatFor: one table of Metal formats rather than two.
MTLPixelFormat metalTextureFormat(TextureFormat format)
{
    return toMetalFormat(format);
}

struct Texture::Native
{
    Native(Device& deviceToUse,
           const TextureDescriptor& descriptor,
           const void* pixels,
           InitialData initialData)
        : device(&deviceToUse)
        , width(descriptor.width)
        , height(descriptor.height)
        , format(descriptor.format)
        , renderTarget(descriptor.renderTarget)
        , computeWrite(descriptor.computeWrite)
    {
        auto metalDevice = (__bridge id<MTLDevice>) deviceToUse.nativeDevice();

        if (metalDevice == nil)
            return;

        const auto resolved = resolveTextureShape(descriptor, initialData);

        if (!resolved)
            return;

        shape = *resolved;
        levels = shape.levels;

        // Refused rather than clamped: see TextureDescriptor::sampleCount. A
        // count on a texture nothing renders into is ignored, as depth is.
        if (renderTarget && descriptor.sampleCount > 1)
        {
            if (!deviceToUse.supportsSampleCount(descriptor.sampleCount))
                return;

            samples = descriptor.sampleCount;
        }

        // Refused rather than created unwritable. See Device::supportsComputeWrite.
        if (computeWrite && !deviceToUse.supportsComputeWrite(descriptor.format))
            return;

        // See the block formats and RGB9E5Float in Texture.h: sampled only.
        if (!isRenderableFormat(format) && (renderTarget || computeWrite))
            return;

        if (isCompressedFormat(format) && !deviceToUse.supportsBlockCompression())
            return;

        if (shape.kind == TextureType::CubeArray && !deviceToUse.supportsCubeArrays())
            return;

        // A chain built from the pixels can come from Metal's blit encoder
        // instead of the CPU, for the formats it filters. See
        // TextureDescriptor::mipmapsOnDevice.
        deviceChain = shape.buildsChain() && descriptor.mipmapsOnDevice
                      && canGenerateMipsOnDevice(format);

        auto textureDescriptor =
            describeShape(shape, toMetalFormat(descriptor.format));
        textureDescriptor.usage = MTLTextureUsageShaderRead;

        // A render target is written by the GPU and read by it, so it goes in
        // private storage and keeps the default (managed/shared) mode otherwise
        // - which is what makes replaceRegion valid on every Mac generation.
        if (renderTarget)
        {
            textureDescriptor.usage |= MTLTextureUsageRenderTarget;
            textureDescriptor.storageMode = MTLStorageModePrivate;
        }

        // The storage mode is left alone, so a texture a kernel accumulates into
        // can still be seeded from the CPU with update().
        if (computeWrite)
            textureDescriptor.usage |= MTLTextureUsageShaderWrite;

        texture = [metalDevice newTextureWithDescriptor:textureDescriptor];

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

        // The colour target's own shape, levels and slices, so a pass into any
        // level or layer of it finds a depth plane of the same size there.
        auto depthDescriptor = describeShape(shape, format);

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
            depthDescriptor.mipmapLevelCount = 1;
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
        shape.width = width;
        shape.height = height;

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

    void update(const void* pixels, int bytesPerRow)
    {
        if (texture.get() == nil)
            return;

        const auto plan =
            planWholeUpload(shapeToUpload(), format, pixels, bytesPerRow);

        if (!plan.valid)
            return;

        upload(plan.uploads);

        if (deviceChain)
            generateLevelsOnDevice();
    }

    TextureShape shapeToUpload() const
    {
        if (!deviceChain)
            return shape;

        auto levelZero = shape;
        levelZero.levels = 1;
        return levelZero;
    }

    void upload(const Vector<SubresourceUpload>& uploads)
    {
        if (texture.get() == nil)
            return;

        if (renderTarget)
        {
            uploadThroughBlit(uploads);
            return;
        }

        for (const auto& each: uploads)
            replaceRegion(each);
    }

    // bytesPerImage is read only where one call covers several images, which
    // is a volume; every other kind takes 0.
    void replaceRegion(const SubresourceUpload& upload)
    {
        const auto bytesPerImage = shape.isVolume() ? upload.bytesPerImage : 0;

        [texture.get() replaceRegion:MTLRegionMake3D((NSUInteger) upload.x,
                                                     (NSUInteger) upload.y,
                                                     (NSUInteger) upload.z,
                                                     (NSUInteger) upload.width,
                                                     (NSUInteger) upload.height,
                                                     (NSUInteger) upload.depth)
                         mipmapLevel:(NSUInteger) upload.level
                               slice:(NSUInteger) upload.slice
                           withBytes:upload.pixels
                         bytesPerRow:(NSUInteger) upload.bytesPerRow
                       bytesPerImage:(NSUInteger) bytesPerImage];
    }

    // A render target is in private storage, where replaceRegion raises, so
    // its pixels go through a shared buffer and a blit on the queue: ordered
    // after everything committed before it, and before every frame after.
    void uploadThroughBlit(const Vector<SubresourceUpload>& uploads)
    {
        auto metalDevice = (__bridge id<MTLDevice>) device->nativeDevice();
        auto queue = (__bridge id<MTLCommandQueue>) device->nativeQueue();

        if (metalDevice == nil || queue == nil)
            return;

        @autoreleasepool
        {
            auto commandBuffer = [queue commandBuffer];
            auto blit = [commandBuffer blitCommandEncoder];

            for (const auto& each: uploads)
                blitUpload(metalDevice, blit, each);

            [blit endEncoding];
            [commandBuffer commit];
        }
    }

    void generateLevelsOnDevice()
    {
        auto queue = (__bridge id<MTLCommandQueue>) device->nativeQueue();

        if (queue == nil)
            return;

        @autoreleasepool
        {
            auto commandBuffer = [queue commandBuffer];
            auto blit = [commandBuffer blitCommandEncoder];

            [blit generateMipmapsForTexture:(id<MTLTexture>) texture.get()];
            [blit endEncoding];
            [commandBuffer commit];

            device->trackSubmittedWork((__bridge void*) commandBuffer);
        }
    }

    void blitUpload(id<MTLDevice> metalDevice,
                    id<MTLBlitCommandEncoder> blit,
                    const SubresourceUpload& upload)
    {
        const auto rowBytes = levelBytesPerRow(format, upload.width);
        const auto rows = levelRows(format, upload.height);
        const auto imageBytes = (NSUInteger) rowBytes * (NSUInteger) rows;

        auto staging = [metalDevice
            newBufferWithLength:imageBytes * (NSUInteger) upload.depth
                        options:MTLResourceStorageModeShared];

        if (staging == nil)
            return;

        auto* out = (unsigned char*) [staging contents];
        const auto* in = (const unsigned char*) upload.pixels;

        for (auto image = 0; image < upload.depth; ++image)
            for (auto row = 0; row < rows; ++row)
                std::memcpy(out + (NSUInteger) image * imageBytes
                                + (NSUInteger) row * (NSUInteger) rowBytes,
                            in + (std::size_t) image * (std::size_t) upload.bytesPerImage
                                + (std::size_t) row * (std::size_t) upload.bytesPerRow,
                            (std::size_t) rowBytes);

        [blit copyFromBuffer:staging
                   sourceOffset:0
              sourceBytesPerRow:(NSUInteger) rowBytes
            sourceBytesPerImage:imageBytes
                     sourceSize:MTLSizeMake((NSUInteger) upload.width,
                                            (NSUInteger) upload.height,
                                            (NSUInteger) upload.depth)
                      toTexture:texture.get()
               destinationSlice:(NSUInteger) upload.slice
               destinationLevel:(NSUInteger) upload.level
              destinationOrigin:MTLOriginMake((NSUInteger) upload.x,
                                              (NSUInteger) upload.y,
                                              (NSUInteger) upload.z)];
    }

    // The two old region forms: level 0 of a 2D texture, uncompressed. On any
    // other kind there is more than one rectangle they could mean, and a
    // compressed one would have to land on the block grid; both are dropped
    // rather than guessed at - TextureRegion is how to name either.
    bool takesLevelZeroRegions() const
    {
        return shape.kind == TextureType::TwoD && !isCompressedFormat(format);
    }

    static TextureRegion levelZeroRegion(int x,
                                         int y,
                                         int regionWidth,
                                         int regionHeight,
                                         int bytesPerRow)
    {
        auto region = TextureRegion {};
        region.origin = {x, y, 0};
        region.size = {regionWidth, regionHeight, 1};
        region.bytesPerRow = bytesPerRow;
        return region;
    }

    void updateRegion(int x,
                      int y,
                      int regionWidth,
                      int regionHeight,
                      const void* pixels,
                      int bytesPerRow)
    {
        if (regionWidth <= 0 || regionHeight <= 0 || !takesLevelZeroRegions())
            return;

        update(levelZeroRegion(x, y, regionWidth, regionHeight, bytesPerRow),
               pixels);
    }

    void update(const TextureRegion& region, const void* pixels)
    {
        if (texture.get() == nil)
            return;

        if (auto upload = planRegionUpload(shape, format, region, pixels))
            this->upload(Vector<SubresourceUpload> {*upload});
    }

    void readRegion(int x,
                    int y,
                    int regionWidth,
                    int regionHeight,
                    void* dst,
                    int bytesPerRow) const
    {
        if (regionWidth <= 0 || regionHeight <= 0 || !takesLevelZeroRegions())
            return;

        read(levelZeroRegion(x, y, regionWidth, regionHeight, bytesPerRow), dst);
    }

    // Through a shared buffer rather than getBytes, because a render target is
    // in private storage and getBytes on one raises: the blit is the only way
    // off the device that works for every texture this class creates.
    void read(const TextureRegion& region, void* dst) const
    {
        auto source = (id<MTLTexture>) texture.get();

        if (source == nil || device == nullptr)
            return;

        const auto plan = planRegionRead(shape, format, region, dst);

        if (!plan)
            return;

        auto metalDevice = (__bridge id<MTLDevice>) device->nativeDevice();
        auto queue = (__bridge id<MTLCommandQueue>) device->nativeQueue();

        if (metalDevice == nil || queue == nil)
            return;

        @autoreleasepool
        {
            const auto rowBytes =
                (NSUInteger) levelBytesPerRow(format, plan->width);
            const auto imageBytes = rowBytes * (NSUInteger) plan->height;

            auto staging =
                [metalDevice newBufferWithLength:imageBytes * (NSUInteger) plan->depth
                                         options:MTLResourceStorageModeShared];

            if (staging == nil)
                return;

            auto commandBuffer = [queue commandBuffer];
            auto blit = [commandBuffer blitCommandEncoder];

            [blit copyFromTexture:source
                             sourceSlice:(NSUInteger) plan->slice
                             sourceLevel:(NSUInteger) plan->level
                            sourceOrigin:MTLOriginMake((NSUInteger) plan->x,
                                                       (NSUInteger) plan->y,
                                                       (NSUInteger) plan->z)
                              sourceSize:MTLSizeMake((NSUInteger) plan->width,
                                                     (NSUInteger) plan->height,
                                                     (NSUInteger) plan->depth)
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

            auto* out = (unsigned char*) dst;
            const auto* in = (const unsigned char*) [staging contents];

            for (auto image = 0; image < plan->depth; ++image)
                for (auto row = 0; row < plan->height; ++row)
                    std::memcpy(out + (std::size_t) image * (std::size_t) plan->bytesPerImage
                                    + (std::size_t) row * (std::size_t) plan->bytesPerRow,
                                in + (NSUInteger) image * imageBytes
                                    + (NSUInteger) row * rowBytes,
                                (std::size_t) rowBytes);
        }
    }

    id<MTLTexture> subresourceView(int level,
                                   int levelCount,
                                   int slice,
                                   int sliceCount) const
    {
        auto whole = (id<MTLTexture>) texture.get();

        if (whole == nil || level < 0 || levelCount < 1 || slice < 0 || sliceCount < 1
            || level + levelCount > shape.levels)
            return nil;

        const auto slices = shape.isVolume() ? 1 : shape.slices();

        if (shape.isVolume())
        {
            slice = 0;
            sliceCount = 1;
        }

        if (slice + sliceCount > slices)
            return nil;

        if (level == 0 && levelCount == shape.levels && slice == 0
            && sliceCount == slices)
            return whole;

        const auto key = SubresourceViewKey {level, levelCount, slice, sliceCount};

        for (const auto& [cached, view]: views)
            if (cached == key)
                return (id<MTLTexture>) view.get();

        const auto type = shape.isVolume()  ? MTLTextureType3D
                          : sliceCount == 1 ? MTLTextureType2D
                                            : MTLTextureType2DArray;

        auto view = [whole
            newTextureViewWithPixelFormat:whole.pixelFormat
                              textureType:type
                                   levels:NSMakeRange((NSUInteger) level,
                                                      (NSUInteger) levelCount)
                                   slices:NSMakeRange((NSUInteger) slice,
                                                      (NSUInteger) sliceCount)];

        if (view == nil)
            return nil;

        auto owned = ObjC::Ptr<NSObject<MTLTexture>> {};
        owned = (NSObject<MTLTexture>*) view;
        views.add({key, owned});
        return view;
    }

    id<MTLTexture> levelView(int level) const
    {
        auto whole = (id<MTLTexture>) texture.get();

        if (whole == nil || level < 0 || level >= shape.levels)
            return nil;

        if (shape.levels == 1)
            return whole;

        // Keyed apart from subresourceView's ranges by a negative slice count.
        const auto key = SubresourceViewKey {level, 1, 0, -1};

        for (const auto& [cached, view]: views)
            if (cached == key)
                return (id<MTLTexture>) view.get();

        const auto slices = shape.isVolume() ? 1 : shape.slices();

        auto view = [whole
            newTextureViewWithPixelFormat:whole.pixelFormat
                              textureType:whole.textureType
                                   levels:NSMakeRange((NSUInteger) level, 1)
                                   slices:NSMakeRange(0, (NSUInteger) slices)];

        if (view == nil)
            return nil;

        auto owned = ObjC::Ptr<NSObject<MTLTexture>> {};
        owned = (NSObject<MTLTexture>*) view;
        views.add({key, owned});
        return view;
    }

    // The sampleable plane - the resolve on a multisampled target - as a 2D
    // texture of one slice, so a shader declaring depth2d reads a layer or a
    // face of an array or a cube target's depth.
    id<MTLTexture> depthSliceView(int slice) const
    {
        auto plane = resolvedDepthTexture.get() != nil
                         ? (id<MTLTexture>) resolvedDepthTexture.get()
                         : (id<MTLTexture>) depthTexture.get();

        if (!sampleableDepth || plane == nil || slice < 0
            || slice >= (int) plane.arrayLength * sliceFactor(plane))
            return nil;

        if (plane.textureType == MTLTextureType2D)
            return plane;

        for (const auto& [cached, view]: depthSliceViews)
            if (cached == slice)
                return (id<MTLTexture>) view.get();

        auto view = [plane newTextureViewWithPixelFormat:plane.pixelFormat
                                             textureType:MTLTextureType2D
                                                  levels:NSMakeRange(0, 1)
                                                  slices:NSMakeRange((NSUInteger) slice, 1)];

        if (view == nil)
            return nil;

        auto owned = ObjC::Ptr<NSObject<MTLTexture>> {};
        owned = (NSObject<MTLTexture>*) view;
        depthSliceViews.add({slice, owned});
        return view;
    }

    static int sliceFactor(id<MTLTexture> texture)
    {
        return texture.textureType == MTLTextureTypeCube
                       || texture.textureType == MTLTextureTypeCubeArray
                   ? 6
                   : 1;
    }

    // A cube or a cube array retyped as the 2D array of slices it is stored as;
    // an array is that already, and its level view is the answer.
    id<MTLTexture> arrayView(int level) const
    {
        if (shape.kind == TextureType::TwoDArray)
            return levelView(level);

        auto whole = (id<MTLTexture>) texture.get();

        if (whole == nil || level < 0 || level >= shape.levels
            || (shape.kind != TextureType::Cube
                && shape.kind != TextureType::CubeArray))
            return nil;

        // Keyed apart from the level views by a slice count of -2.
        const auto key = SubresourceViewKey {level, 1, 0, -2};

        for (const auto& [cached, view]: views)
            if (cached == key)
                return (id<MTLTexture>) view.get();

        auto view = [whole
            newTextureViewWithPixelFormat:whole.pixelFormat
                              textureType:MTLTextureType2DArray
                                   levels:NSMakeRange((NSUInteger) level, 1)
                                   slices:NSMakeRange(0, (NSUInteger) shape.slices())];

        if (view == nil)
            return nil;

        auto owned = ObjC::Ptr<NSObject<MTLTexture>> {};
        owned = (NSObject<MTLTexture>*) view;
        views.add({key, owned});
        return view;
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

    TextureShape shape;
    int levels = 1;

    // Level 0 is uploaded and the blit encoder builds the rest. See
    // TextureDescriptor::mipmapsOnDevice.
    bool deviceChain = false;

    bool renderTarget = false;
    bool computeWrite = false;

    // How many samples a pass into this target takes. 1 on everything that is
    // not a multisampled render target, and the count the multisample colour
    // texture and the depth buffer were both created at otherwise.
    int samples = 1;

    // Whether that depth buffer was created able to be sampled as well as
    // attached, which is the one thing a bind through it can check.
    bool sampleableDepth = false;

    ObjC::Ptr<NSObject<MTLTexture>> texture;
    ObjC::Ptr<NSObject<MTLTexture>> multisampleTexture;
    ObjC::Ptr<NSObject<MTLTexture>> depthTexture;
    ObjC::Ptr<NSObject<MTLTexture>> resolvedDepthTexture;
    CFRef<CVMetalTextureRef> cvTexture;

    mutable Vector<std::pair<SubresourceViewKey, ObjC::Ptr<NSObject<MTLTexture>>>>
        views;

    // Level 0 of each depth slice as a 2D texture, by slice, made on first use.
    mutable Vector<std::pair<int, ObjC::Ptr<NSObject<MTLTexture>>>> depthSliceViews;
};

Texture::Texture(Device& device,
                 const TextureDescriptor& descriptor,
                 const void* pixels)
    : impl(device,
           descriptor,
           pixels,
           pixels != nullptr ? InitialData::Pixels : InitialData::None)
{
}

Texture::Texture(Device& device,
                 const TextureDescriptor& descriptor,
                 std::span<const TextureSubresource> subresources)
    : impl(device, descriptor, nullptr, InitialData::Subresources)
{
    if (!isValid())
        return;

    const auto plan = planSubresourceUploads(impl->shape, impl->format, subresources);

    if (!plan.valid)
    {
        impl->texture.release();
        return;
    }

    impl->upload(plan.uploads);
}

Texture::Texture(Device& device, void* nativePixelBuffer)
    : impl(device, nativePixelBuffer)
{
}

void Texture::update(const void* pixels, int bytesPerRow)
{
    if (bytesPerRow < 0)
        return;

    impl->update(pixels, bytesPerRow);
}

void Texture::update(const Graphics::Rect& region,
                     const void* pixels,
                     int bytesPerRow)
{
    if (bytesPerRow < 0)
        return;

    // Texels are whole; round rather than truncate so a rect built from
    // accumulated float arithmetic lands on the texel it is nearest to.
    impl->updateRegion((int) std::lround(region.x),
                       (int) std::lround(region.y),
                       (int) std::lround(region.w),
                       (int) std::lround(region.h),
                       pixels,
                       bytesPerRow);
}

void Texture::updateSubresource(const TextureRegion& region, const void* pixels)
{
    impl->update(region, pixels);
}

void Texture::read(void* dst, int bytesPerRow) const
{
    if (bytesPerRow < 0)
        return;

    impl->readRegion(0, 0, impl->width, impl->height, dst, bytesPerRow);
}

void Texture::read(const Graphics::Rect& region, void* dst, int bytesPerRow) const
{
    if (bytesPerRow < 0)
        return;

    impl->readRegion((int) std::lround(region.x),
                     (int) std::lround(region.y),
                     (int) std::lround(region.w),
                     (int) std::lround(region.h),
                     dst,
                     bytesPerRow);
}

void Texture::readSubresource(const TextureRegion& region, void* dst) const
{
    impl->read(region, dst);
}

TextureType Texture::kind() const
{
    return impl->shape.kind;
}

int Texture::arrayLength() const
{
    return impl->shape.layers;
}

int Texture::volumeDepth() const
{
    return impl->shape.depth;
}

int Texture::slices() const
{
    return impl->shape.slices();
}

void* Texture::nativeSubresourceView(int level,
                                     int levelCount,
                                     int slice,
                                     int sliceCount) const
{
    return (__bridge void*) impl->subresourceView(level, levelCount, slice, sliceCount);
}

void* Texture::nativeLevelView(int level) const
{
    return (__bridge void*) impl->levelView(level);
}

void* Texture::nativeArrayView(int level) const
{
    return (__bridge void*) impl->arrayView(level);
}

void* Texture::nativeDepthSliceView(int slice) const
{
    return (__bridge void*) impl->depthSliceView(slice);
}

int Texture::width() const
{
    return impl->width;
}

int Texture::height() const
{
    return impl->height;
}

TextureFormat Texture::format() const
{
    return impl->format;
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
    return impl->shape.kind == TextureType::Cube && impl->texture.get() != nil;
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
