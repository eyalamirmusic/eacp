#include "Texture.h"

#include "../Device/Device.h"
#include "../WebGPU/WebGPUTypes.h"
#include "MipChain.h"

#include <cmath>
#include <cstring>

namespace eacp::GPU
{
namespace
{
template <typename Handle, typename Release>
void releaseHandle(Handle& handle, Release release)
{
    if (handle != nullptr)
        release(handle);

    handle = nullptr;
}

WGPUTexture makeWebTexture(const WGPUTextureDescriptor& descriptor)
{
    auto& shared = getWebGPUShared();

    if (!shared.isValid())
        return nullptr;

    return wgpuDeviceCreateTexture(shared.getDevice(), &descriptor);
}

WGPUTextureView makeWebView(WGPUTexture texture,
                            WGPUTextureViewDimension dimension,
                            WGPUTextureAspect aspect,
                            int levels)
{
    if (texture == nullptr)
        return nullptr;

    auto descriptor = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    descriptor.dimension = dimension;
    descriptor.aspect = aspect;
    descriptor.mipLevelCount = static_cast<std::uint32_t>(levels);

    return wgpuTextureCreateView(texture, &descriptor);
}

WGPUExtent3D webExtent(int width, int height, int layers = 1)
{
    return {static_cast<std::uint32_t>(width),
            static_cast<std::uint32_t>(height),
            static_cast<std::uint32_t>(layers)};
}

// The copy extent of a compressed level is its physical size: whole blocks.
int blockAligned(TextureFormat format, int extent)
{
    return isCompressedFormat(format) ? (extent + 3) / 4 * 4 : extent;
}
} // namespace

WGPUTextureFormat toWebFormat(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::RGBA8Unorm:
            return WGPUTextureFormat_RGBA8Unorm;
        case TextureFormat::BGRA8Unorm:
            return WGPUTextureFormat_BGRA8Unorm;
        case TextureFormat::R8Unorm:
            return WGPUTextureFormat_R8Unorm;
        case TextureFormat::RG8Unorm:
            return WGPUTextureFormat_RG8Unorm;
        case TextureFormat::RGBA16Float:
            return WGPUTextureFormat_RGBA16Float;
        case TextureFormat::RGBA32Float:
            return WGPUTextureFormat_RGBA32Float;
        case TextureFormat::R32Float:
            return WGPUTextureFormat_R32Float;
        case TextureFormat::BC1RGBA:
            return WGPUTextureFormat_BC1RGBAUnorm;
        case TextureFormat::BC2RGBA:
            return WGPUTextureFormat_BC2RGBAUnorm;
        case TextureFormat::BC3RGBA:
            return WGPUTextureFormat_BC3RGBAUnorm;
        case TextureFormat::BC7RGBA:
            return WGPUTextureFormat_BC7RGBAUnorm;
    }

    return WGPUTextureFormat_RGBA8Unorm;
}

WGPUTextureFormat toWebFormat(PixelFormat format)
{
    switch (format)
    {
        case PixelFormat::BGRA8Unorm:
            return WGPUTextureFormat_BGRA8Unorm;
        case PixelFormat::RGBA8Unorm:
            return WGPUTextureFormat_RGBA8Unorm;
        case PixelFormat::RGBA16Float:
            return WGPUTextureFormat_RGBA16Float;
        case PixelFormat::RGBA32Float:
            return WGPUTextureFormat_RGBA32Float;
        case PixelFormat::R32Float:
            return WGPUTextureFormat_R32Float;
    }

    return WGPUTextureFormat_BGRA8Unorm;
}

// Depth32Float is what Metal attaches; with a stencil plane the combined format
// WebGPU guarantees is Depth24PlusStencil8, Depth32FloatStencil8 being optional.
WGPUTextureFormat webDepthFormat(bool stencil)
{
    return stencil ? WGPUTextureFormat_Depth24PlusStencil8
                   : WGPUTextureFormat_Depth32Float;
}

bool webFormatHasStencil(WGPUTextureFormat format)
{
    return format == WGPUTextureFormat_Depth24PlusStencil8
           || format == WGPUTextureFormat_Depth32FloatStencil8;
}

bool createWebMultisampleCompanion(WebTextureData& data)
{
    auto descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
    descriptor.label = toWebString("eacp multisample");
    descriptor.usage = WGPUTextureUsage_RenderAttachment;
    descriptor.size = webExtent(data.width, data.height);
    descriptor.format = data.format;
    descriptor.sampleCount = static_cast<std::uint32_t>(data.sampleCount);

    data.msaaTexture = makeWebTexture(descriptor);
    data.msaaView = makeWebView(
        data.msaaTexture, WGPUTextureViewDimension_2D, WGPUTextureAspect_All, 1);

    return data.msaaView != nullptr;
}

// WebGPU resolves no depth, so a multisampled target cannot have a sampleable
// one; Texture refuses that descriptor before this runs.
bool createWebDepthCompanion(WebTextureData& data, bool stencil, bool sampleable)
{
    data.depthHasStencil = stencil;
    data.depthFormat = webDepthFormat(stencil);

    auto descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
    descriptor.label = toWebString("eacp depth");
    descriptor.usage = WGPUTextureUsage_RenderAttachment;
    descriptor.size = webExtent(data.width, data.height);
    descriptor.format = data.depthFormat;
    descriptor.sampleCount = static_cast<std::uint32_t>(data.sampleCount);

    if (sampleable)
        descriptor.usage |= WGPUTextureUsage_TextureBinding;

    data.depthTexture = makeWebTexture(descriptor);
    data.depthView = makeWebView(
        data.depthTexture, WGPUTextureViewDimension_2D, WGPUTextureAspect_All, 1);

    if (data.depthView == nullptr)
        return false;

    if (sampleable)
        data.depthReadView = makeWebView(data.depthTexture,
                                         WGPUTextureViewDimension_2D,
                                         WGPUTextureAspect_DepthOnly,
                                         1);

    return !sampleable || data.depthReadView != nullptr;
}

void releaseWebCompanions(WebTextureData& data)
{
    releaseHandle(data.msaaView, wgpuTextureViewRelease);
    releaseHandle(data.msaaTexture, wgpuTextureRelease);
    releaseHandle(data.depthReadView, wgpuTextureViewRelease);
    releaseHandle(data.depthView, wgpuTextureViewRelease);
    releaseHandle(data.depthTexture, wgpuTextureRelease);

    data.depthFormat = WGPUTextureFormat_Undefined;
    data.depthHasStencil = false;
}

void releaseWebTexture(WebTextureData& data)
{
    releaseWebCompanions(data);

    releaseHandle(data.storageView, wgpuTextureViewRelease);
    releaseHandle(data.attachmentView, wgpuTextureViewRelease);
    releaseHandle(data.sampledView, wgpuTextureViewRelease);
    releaseHandle(data.texture, wgpuTextureRelease);
}

struct Texture::Native
{
    Native(Device& device, const TextureDescriptor& descriptor, const void* pixels)
        : format(descriptor.format)
    {
        data.width = descriptor.width;
        data.height = descriptor.height;
        data.cube = descriptor.cube;
        data.format = toWebFormat(descriptor.format);

        if (!device.isValid() || data.width <= 0 || data.height <= 0)
            return;

        if (!descriptorIsPossible(device, descriptor, pixels))
            return;

        if (!createTexture(descriptor))
        {
            release();
            return;
        }

        if (pixels != nullptr)
            upload(pixels, 0);

        if (data.sampleCount > 1 && !createWebMultisampleCompanion(data))
        {
            release();
            return;
        }

        if (descriptor.renderTarget
            && (descriptor.depth || descriptor.stencil || descriptor.sampleableDepth)
            && !createWebDepthCompanion(
                data, descriptor.stencil, descriptor.sampleableDepth))
            releaseWebCompanions(data);
    }

    Native(Device&, void*) {}

    ~Native() { release(); }

    void release()
    {
        releaseWebTexture(data);
        data = {};
    }

    bool descriptorIsPossible(Device& device,
                              const TextureDescriptor& descriptor,
                              const void* pixels)
    {
        if (data.cube
            && (data.width != data.height || descriptor.renderTarget
                || descriptor.computeWrite))
            return false;

        if (descriptor.renderTarget && descriptor.sampleCount > 1)
        {
            if (data.cube)
                return false;

            if (!device.supportsSampleCount(descriptor.sampleCount))
            {
                LOG("WebGPU: the device refuses ",
                    descriptor.sampleCount,
                    " samples (WebGPU has 1 and 4), so the target is invalid "
                    "rather than drawn at a count its pipelines do not carry");
                return false;
            }

            if (descriptor.sampleableDepth)
            {
                LOG("WebGPU: there is no depth resolve, so a multisampled "
                    "sampleable-depth target is invalid rather than sampled "
                    "from a resolve that never ran");
                return false;
            }

            data.sampleCount = descriptor.sampleCount;
        }

        if (isCompressedFormat(format))
        {
            if (descriptor.renderTarget || descriptor.computeWrite)
                return false;

            if (!device.supportsBlockCompression())
            {
                LOG("WebGPU: the device has no texture-compression-bc, so a "
                    "compressed texture is invalid here");
                return false;
            }

            if (data.width % 4 != 0 || data.height % 4 != 0)
            {
                LOG("WebGPU: a block-compressed texture must be a whole number "
                    "of 4x4 blocks across and down");
                return false;
            }
        }

        if (descriptor.computeWrite)
        {
            if (!supportsComputeWrite(descriptor.format))
                return false;

            computeWrite = true;
        }

        if (descriptor.mipLevels < 0)
            return false;

        if (descriptor.mipLevels > 0)
        {
            if (descriptor.mipmapped || descriptor.renderTarget
                || descriptor.computeWrite || data.cube || pixels == nullptr
                || descriptor.mipLevels > mipLevelCount(data.width, data.height))
                return false;

            data.mipLevels = descriptor.mipLevels;
            suppliedChain = true;
        }
        else if (descriptor.mipmapped && pixels != nullptr
                 && canBuildMipChain(format))
        {
            data.mipLevels = mipLevelCount(data.width, data.height);
        }

        return true;
    }

    bool createTexture(const TextureDescriptor& descriptor)
    {
        auto info = WGPU_TEXTURE_DESCRIPTOR_INIT;
        info.size = webExtent(data.width, data.height, data.cube ? 6 : 1);
        info.format = data.format;
        info.mipLevelCount = static_cast<std::uint32_t>(data.mipLevels);
        info.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc
                     | WGPUTextureUsage_CopyDst;

        if (descriptor.renderTarget)
            info.usage |= WGPUTextureUsage_RenderAttachment;

        if (computeWrite)
            info.usage |= WGPUTextureUsage_StorageBinding;

        data.texture = makeWebTexture(info);

        if (data.texture == nullptr)
            return false;

        data.sampledView = makeWebView(data.texture,
                                       data.cube ? WGPUTextureViewDimension_Cube
                                                 : WGPUTextureViewDimension_2D,
                                       WGPUTextureAspect_All,
                                       data.mipLevels);

        if (descriptor.renderTarget)
            data.attachmentView = makeWebView(
                data.texture, WGPUTextureViewDimension_2D, WGPUTextureAspect_All, 1);

        if (computeWrite)
            data.storageView = makeWebView(
                data.texture, WGPUTextureViewDimension_2D, WGPUTextureAspect_All, 1);

        return data.sampledView != nullptr
               && (!descriptor.renderTarget || data.attachmentView != nullptr)
               && (!computeWrite || data.storageView != nullptr);
    }

    // Straight from the caller's rows: writeTexture takes any stride, where a
    // buffer copy would want it on 256.
    void copyPixels(const void* pixels,
                    int sourcePitch,
                    int destX,
                    int destY,
                    int regionWidth,
                    int regionHeight,
                    int mipLevel,
                    int face)
    {
        auto destination = WGPUTexelCopyTextureInfo {};
        destination.texture = data.texture;
        destination.mipLevel = static_cast<std::uint32_t>(mipLevel);
        destination.origin = {static_cast<std::uint32_t>(destX),
                              static_cast<std::uint32_t>(destY),
                              static_cast<std::uint32_t>(face)};
        destination.aspect = WGPUTextureAspect_All;

        const auto rows = levelRows(format, regionHeight);

        auto layout = WGPUTexelCopyBufferLayout {};
        layout.bytesPerRow = static_cast<std::uint32_t>(sourcePitch);
        layout.rowsPerImage = static_cast<std::uint32_t>(rows);

        const auto bytes =
            static_cast<std::size_t>(sourcePitch)
                * static_cast<std::size_t>(rows - 1)
            + static_cast<std::size_t>(levelBytesPerRow(format, regionWidth));

        const auto extent = webExtent(blockAligned(format, regionWidth),
                                      blockAligned(format, regionHeight));

        wgpuQueueWriteTexture(getWebGPUShared().getQueue(),
                              &destination,
                              pixels,
                              bytes,
                              &layout,
                              &extent);
    }

    void recordFace(const void* pixels, int sourcePitch, int face)
    {
        if (data.mipLevels <= 1)
        {
            copyPixels(pixels, sourcePitch, 0, 0, data.width, data.height, 0, face);
            return;
        }

        if (suppliedChain)
        {
            recordChain(pixels, nullptr, face);
            return;
        }

        const auto chain =
            buildMipChain(pixels, data.width, data.height, format, sourcePitch);

        if (chain.isValid())
            recordChain(nullptr, &chain, face);
    }

    void recordChain(const void* packed, const MipChain* chain, int face)
    {
        const auto* bytes = static_cast<const std::byte*>(packed);

        for (auto level = 0; level < data.mipLevels; ++level)
        {
            const auto levelWidth = mipExtent(data.width, level);
            const auto levelHeight = mipExtent(data.height, level);

            copyPixels(chain != nullptr ? chain->level(level) : bytes,
                       levelBytesPerRow(format, levelWidth),
                       0,
                       0,
                       levelWidth,
                       levelHeight,
                       level,
                       face);

            if (bytes != nullptr)
                bytes += levelBytes(format, levelWidth, levelHeight);
        }
    }

    void upload(const void* pixels, int bytesPerRow)
    {
        const auto pitch =
            bytesPerRow != 0 ? bytesPerRow : levelBytesPerRow(format, data.width);

        const auto faces = data.cube ? 6 : 1;
        const auto faceBytes =
            static_cast<std::size_t>(pitch)
            * static_cast<std::size_t>(levelRows(format, data.height));

        for (auto face = 0; face < faces; ++face)
            recordFace(static_cast<const std::byte*>(pixels)
                           + static_cast<std::size_t>(face) * faceBytes,
                       pitch,
                       face);
    }

    void update(const void* pixels, int bytesPerRow)
    {
        if (!data.isValid() || pixels == nullptr)
            return;

        if (bytesPerRow != 0 && (suppliedChain || isCompressedFormat(format)))
            return;

        if (data.mipLevels > 1 || data.cube || isCompressedFormat(format))
        {
            upload(pixels, bytesPerRow);
            return;
        }

        updateRegion(0, 0, data.width, data.height, pixels, bytesPerRow);
    }

    void updateRegion(int x,
                      int y,
                      int regionWidth,
                      int regionHeight,
                      const void* pixels,
                      int bytesPerRow)
    {
        if (!data.isValid() || pixels == nullptr)
            return;

        if (regionWidth <= 0 || regionHeight <= 0 || data.cube
            || isCompressedFormat(format))
            return;

        if (x < 0 || y < 0 || x + regionWidth > data.width
            || y + regionHeight > data.height)
            return;

        const auto pitch =
            bytesPerRow != 0 ? bytesPerRow : levelBytesPerRow(format, regionWidth);

        copyPixels(pixels, pitch, x, y, regionWidth, regionHeight, 0, 0);
    }

    TextureFormat format = TextureFormat::RGBA8Unorm;

    bool suppliedChain = false;
    bool computeWrite = false;

    mutable WebTextureData data;
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

    impl->updateRegion(static_cast<int>(std::lround(region.x)),
                       static_cast<int>(std::lround(region.y)),
                       static_cast<int>(std::lround(region.w)),
                       static_cast<int>(std::lround(region.h)),
                       pixels,
                       bytesPerRow);
}

// A copy back is a mapAsync the main thread would have to wait on.
void Texture::read(void*, int) const
{
    reportWebUnsupported("Texture::read");
}

void Texture::read(const Graphics::Rect&, void*, int) const
{
    reportWebUnsupported("Texture::read");
}

int Texture::width() const
{
    return impl->data.width;
}

int Texture::height() const
{
    return impl->data.height;
}

bool Texture::isValid() const
{
    return impl->data.isValid();
}

int Texture::mipLevels() const
{
    return impl->data.mipLevels;
}

bool Texture::isRenderTarget() const
{
    return isValid() && impl->data.isRenderTarget();
}

bool Texture::isCube() const
{
    return isValid() && impl->data.cube;
}

bool Texture::isComputeWritable() const
{
    return isValid() && impl->data.isComputeWritable();
}

bool Texture::hasDepth() const
{
    return isValid() && impl->data.hasDepth();
}

bool Texture::hasStencil() const
{
    return isValid() && impl->data.hasStencil();
}

bool Texture::hasSampleableDepth() const
{
    return isValid() && impl->data.hasSampleableDepth();
}

int Texture::sampleCount() const
{
    return isValid() ? impl->data.sampleCount : 1;
}

void* Texture::nativeTexture() const
{
    return &impl->data;
}

void* Texture::nativeReadView() const
{
    return &impl->data;
}

void* Texture::nativeDepthTexture() const
{
    return nullptr;
}

void* Texture::nativeMultisampleTexture() const
{
    return nullptr;
}

void* Texture::nativeResolvedDepthTexture() const
{
    return nullptr;
}
} // namespace eacp::GPU
