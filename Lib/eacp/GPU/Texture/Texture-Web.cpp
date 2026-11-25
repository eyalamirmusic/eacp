#include "Texture.h"

#include "TextureShape.h"

#include "../Device/Device.h"
#include "../WebGPU/WebGPUContext.h"

#include <eacp/Core/Utils/Logging.h>

#include <cmath>
#include <cstring>

namespace eacp::GPU
{
namespace
{
constexpr std::uint32_t readbackRowAlignment = 256;
constexpr std::uint32_t multisampleCount = 4;

bool isVolume(const WebGPUTextureData& data)
{
    return data.kind == TextureType::ThreeD;
}

bool isDepthWithStencil(wgpu::TextureFormat format)
{
    return format == wgpu::TextureFormat::Depth24PlusStencil8
           || format == wgpu::TextureFormat::Depth32FloatStencil8;
}

// A storage binding takes no cube view, so a cube is written as the 2D array
// of its faces, as it is on the other backends.
wgpu::TextureViewDimension storageDimension(TextureType kind)
{
    switch (kind)
    {
        case TextureType::TwoD:
            return wgpu::TextureViewDimension::e2D;
        case TextureType::ThreeD:
            return wgpu::TextureViewDimension::e3D;
        default:
            return wgpu::TextureViewDimension::e2DArray;
    }
}

int dataSizeOf(TextureFormat format, const SubresourceUpload& upload)
{
    const auto rows = levelRows(format, upload.height);
    const auto lastRow = levelBytesPerRow(format, upload.width);

    return upload.bytesPerImage * (upload.depth - 1)
           + upload.bytesPerRow * (rows - 1) + lastRow;
}
} // namespace

wgpu::TextureView
    webgpuSampledView(const WebGPUTextureData& data, int level, bool asArray)
{
    if (data.texture == nullptr)
        return {};

    if (level < 0 && !asArray)
        return data.sampledView;

    auto descriptor = wgpu::TextureViewDescriptor {};
    descriptor.dimension =
        asArray ? wgpu::TextureViewDimension::e2DArray : sampledDimension(data.kind);
    descriptor.baseMipLevel = static_cast<std::uint32_t>(level < 0 ? 0 : level);
    descriptor.mipLevelCount =
        level < 0 ? static_cast<std::uint32_t>(data.mipLevels) : 1u;
    return data.texture.CreateView(&descriptor);
}

wgpu::TextureView webgpuStorageView(const WebGPUTextureData& data, int level)
{
    if (data.texture == nullptr)
        return {};

    auto descriptor = wgpu::TextureViewDescriptor {};
    descriptor.dimension = storageDimension(data.kind);
    descriptor.baseMipLevel = static_cast<std::uint32_t>(level);
    descriptor.mipLevelCount = 1;
    return data.texture.CreateView(&descriptor);
}

wgpu::TextureView webgpuDepthView(const WebGPUTextureData& data, int slice)
{
    if (data.depth == nullptr)
        return {};

    auto descriptor = wgpu::TextureViewDescriptor {};
    descriptor.aspect = wgpu::TextureAspect::DepthOnly;
    descriptor.mipLevelCount = 1;

    if (slice >= 0)
    {
        descriptor.dimension = wgpu::TextureViewDimension::e2D;
        descriptor.baseArrayLayer = static_cast<std::uint32_t>(slice);
        descriptor.arrayLayerCount = 1;
    }
    else
    {
        descriptor.dimension = isVolume(data) ? wgpu::TextureViewDimension::e2DArray
                                              : sampledDimension(data.kind);
    }

    return data.depth.CreateView(&descriptor);
}

wgpu::TextureView webgpuAttachmentView(const wgpu::Texture& texture,
                                       int level,
                                       int slice,
                                       int layers)
{
    if (texture == nullptr)
        return {};

    auto descriptor = wgpu::TextureViewDescriptor {};
    descriptor.dimension = layers > 1 ? wgpu::TextureViewDimension::e2DArray
                                      : wgpu::TextureViewDimension::e2D;
    descriptor.baseMipLevel = static_cast<std::uint32_t>(level);
    descriptor.mipLevelCount = 1;
    descriptor.baseArrayLayer = static_cast<std::uint32_t>(slice);
    descriptor.arrayLayerCount = static_cast<std::uint32_t>(layers);

    if (isDepthWithStencil(texture.GetFormat()))
        descriptor.aspect = wgpu::TextureAspect::All;

    return texture.CreateView(&descriptor);
}

struct Texture::Native
{
    Native(Device& device,
           const TextureDescriptor& descriptor,
           const void* pixels,
           InitialData initialData)
        : context(getWebGPUContext(device))
        , format(descriptor.format)
    {
        data.width = descriptor.width;
        data.height = descriptor.height;
        data.format = toWGPUFormat(descriptor.format);
        data.textureFormat = descriptor.format;

        if (!context.isValid())
            return;

        const auto resolved = resolveTextureShape(descriptor, initialData);

        if (!resolved)
            return;

        shape = *resolved;
        data.kind = shape.kind;
        data.mipLevels = shape.levels;
        data.depthOrLayers = shape.isVolume() ? shape.depth : shape.slices();
        data.renderTarget = descriptor.renderTarget;

        if (!descriptorIsPossible(device, descriptor) || !createTexture())
        {
            release();
            return;
        }

        if (pixels != nullptr && !upload(pixels, 0))
        {
            release();
            return;
        }

        if (data.sampleCount > 1 && !createMultisample())
        {
            release();
            return;
        }

        if (descriptor.renderTarget
            && (descriptor.depth || descriptor.stencil
                || descriptor.sampleableDepth))
            createDepth(descriptor.stencil, descriptor.sampleableDepth);
    }

    Native(Device& device, void*)
        : context(getWebGPUContext(device))
    {
    }

    void release()
    {
        data.texture = nullptr;
        data.sampledView = nullptr;
        data.multisample = nullptr;
        data.depth = nullptr;
    }

    bool descriptorIsPossible(Device& device, const TextureDescriptor& descriptor)
    {
        if (data.format == wgpu::TextureFormat::Undefined)
            return false;

        if (!isRenderableFormat(format)
            && (descriptor.renderTarget || descriptor.computeWrite))
            return false;

        if (isCompressedFormat(format) && !device.supportsBlockCompression())
        {
            LOG("WebGPU: the device has no block-compressed formats, so a "
                "compressed texture is invalid here");
            return false;
        }

        if (descriptor.renderTarget && descriptor.sampleCount > 1)
        {
            if (!device.supportsSampleCount(descriptor.sampleCount))
            {
                LOG("WebGPU: ",
                    descriptor.sampleCount,
                    " samples are refused; WebGPU draws with 1 or 4");
                return false;
            }

            if (descriptor.sampleableDepth)
            {
                LOG("WebGPU: WebGPU resolves no depth, so a multisampled "
                    "sampleable-depth target is invalid here");
                return false;
            }

            data.sampleCount = descriptor.sampleCount;
        }

        if (descriptor.computeWrite)
        {
            if (!device.supportsComputeWrite(descriptor.format))
            {
                LOG("WebGPU: this format is no storage texture here, so a "
                    "computeWrite texture is invalid rather than unwritable");
                return false;
            }

            data.computeWrite = true;
        }

        return true;
    }

    bool createTexture()
    {
        auto descriptor = wgpu::TextureDescriptor {};
        descriptor.dimension = shape.isVolume() ? wgpu::TextureDimension::e3D
                                                : wgpu::TextureDimension::e2D;
        descriptor.size = {static_cast<std::uint32_t>(data.width),
                           static_cast<std::uint32_t>(data.height),
                           static_cast<std::uint32_t>(data.depthOrLayers)};
        descriptor.format = data.format;
        descriptor.mipLevelCount = static_cast<std::uint32_t>(data.mipLevels);
        descriptor.usage = wgpu::TextureUsage::TextureBinding
                           | wgpu::TextureUsage::CopySrc
                           | wgpu::TextureUsage::CopyDst;

        if (data.renderTarget)
            descriptor.usage |= wgpu::TextureUsage::RenderAttachment;

        if (data.computeWrite)
            descriptor.usage |= wgpu::TextureUsage::StorageBinding;

        auto message = std::string {};
        const auto created = context.succeeds(
            [&] { data.texture = context.getDevice().CreateTexture(&descriptor); },
            &message);

        if (!created || data.texture == nullptr)
        {
            LOG("WebGPU: the device would not create a ",
                data.width,
                "x",
                data.height,
                " texture in this format: ",
                message);
            return false;
        }

        auto view = wgpu::TextureViewDescriptor {};
        view.dimension = sampledDimension(shape.kind);
        data.sampledView = data.texture.CreateView(&view);
        return true;
    }

    bool createMultisample()
    {
        auto descriptor = wgpu::TextureDescriptor {};
        descriptor.size = {static_cast<std::uint32_t>(data.width),
                           static_cast<std::uint32_t>(data.height),
                           1};
        descriptor.format = data.format;
        descriptor.sampleCount = multisampleCount;
        descriptor.usage = wgpu::TextureUsage::RenderAttachment;

        data.multisample = context.getDevice().CreateTexture(&descriptor);
        return data.multisample != nullptr;
    }

    void createDepth(bool stencil, bool sampleable)
    {
        auto descriptor = wgpu::TextureDescriptor {};
        const auto multisampled = data.sampleCount > 1;

        descriptor.size = {
            static_cast<std::uint32_t>(data.width),
            static_cast<std::uint32_t>(data.height),
            multisampled ? 1u : static_cast<std::uint32_t>(data.depthOrLayers)};
        descriptor.mipLevelCount =
            multisampled ? 1u : static_cast<std::uint32_t>(data.mipLevels);
        descriptor.format = stencil ? wgpu::TextureFormat::Depth24PlusStencil8
                                    : wgpu::TextureFormat::Depth32Float;
        descriptor.sampleCount = multisampled ? multisampleCount : 1u;
        descriptor.usage = wgpu::TextureUsage::RenderAttachment;

        if (!multisampled)
            descriptor.usage |= wgpu::TextureUsage::TextureBinding;

        data.depth = context.getDevice().CreateTexture(&descriptor);
        data.depthFormat = descriptor.format;
        data.sampleableDepth = sampleable && !multisampled;
    }

    bool submitUploads(const Vector<SubresourceUpload>& uploads)
    {
        for (const auto& upload: uploads)
            write(upload);

        return true;
    }

    void write(const SubresourceUpload& upload)
    {
        auto destination = wgpu::TexelCopyTextureInfo {};
        destination.texture = data.texture;
        destination.mipLevel = static_cast<std::uint32_t>(upload.level);
        destination.origin = {
            static_cast<std::uint32_t>(upload.x),
            static_cast<std::uint32_t>(upload.y),
            static_cast<std::uint32_t>(shape.isVolume() ? upload.z : upload.slice)};

        auto layout = wgpu::TexelCopyBufferLayout {};
        layout.bytesPerRow = static_cast<std::uint32_t>(upload.bytesPerRow);
        layout.rowsPerImage =
            static_cast<std::uint32_t>(upload.bytesPerImage / upload.bytesPerRow);

        // A block-compressed copy covers whole blocks, so a level smaller than
        // one is written as the block its texels sit in.
        const auto block = isCompressedFormat(format) ? 4 : 1;
        const auto size = wgpu::Extent3D {
            static_cast<std::uint32_t>(alignTo(upload.width, block)),
            static_cast<std::uint32_t>(alignTo(upload.height, block)),
            static_cast<std::uint32_t>(upload.depth)};

        context.getQueue().WriteTexture(
            &destination,
            upload.pixels,
            static_cast<std::size_t>(dataSizeOf(format, upload)),
            &layout,
            &size);
    }

    bool upload(const void* pixels, int bytesPerRow)
    {
        const auto plan = planWholeUpload(shape, format, pixels, bytesPerRow);
        return plan.valid && submitUploads(plan.uploads);
    }

    void update(const void* pixels, int bytesPerRow)
    {
        if (data.isValid())
            upload(pixels, bytesPerRow);
    }

    bool takesLevelZeroRegions() const
    {
        return shape.kind == TextureType::TwoD && !isCompressedFormat(format);
    }

    static TextureRegion levelZeroRegion(
        int x, int y, int regionWidth, int regionHeight, int bytesPerRow)
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
        if (!data.isValid())
            return;

        if (auto upload = planRegionUpload(shape, format, region, pixels))
            write(*upload);
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

    // A copy into a staging buffer whose rows sit on the 256-byte grid WebGPU
    // asks of them, waited for and unpacked into the caller's rows.
    void read(const TextureRegion& region, void* dst) const
    {
        if (!data.isValid() || !webgpuCanWait())
            return;

        const auto plan = planRegionRead(shape, format, region, dst);

        if (!plan)
            return;

        const auto rowBytes =
            static_cast<std::uint32_t>(levelBytesPerRow(format, plan->width));
        const auto stagingRow =
            static_cast<std::uint32_t>(alignTo(rowBytes, readbackRowAlignment));
        const auto rows =
            static_cast<std::uint32_t>(levelRows(format, plan->height));
        const auto images = static_cast<std::uint32_t>(plan->depth);
        const auto bytes = static_cast<std::uint64_t>(stagingRow) * rows * images;

        auto stagingDescriptor = wgpu::BufferDescriptor {};
        stagingDescriptor.size = bytes;
        stagingDescriptor.usage =
            wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
        auto staging = context.getDevice().CreateBuffer(&stagingDescriptor);

        auto source = wgpu::TexelCopyTextureInfo {};
        source.texture = data.texture;
        source.mipLevel = static_cast<std::uint32_t>(plan->level);
        source.origin = {
            static_cast<std::uint32_t>(plan->x),
            static_cast<std::uint32_t>(plan->y),
            static_cast<std::uint32_t>(shape.isVolume() ? plan->z : plan->slice)};

        auto destination = wgpu::TexelCopyBufferInfo {};
        destination.buffer = staging;
        destination.layout.bytesPerRow = stagingRow;
        destination.layout.rowsPerImage = rows;

        const auto size = wgpu::Extent3D {static_cast<std::uint32_t>(plan->width),
                                          static_cast<std::uint32_t>(plan->height),
                                          images};

        auto encoder = context.getDevice().CreateCommandEncoder();
        encoder.CopyTextureToBuffer(&source, &destination, &size);
        auto commands = encoder.Finish();
        context.getQueue().Submit(1, &commands);

        auto done = false;
        auto mapped = false;

        staging.MapAsync(wgpu::MapMode::Read,
                         0,
                         toHostSize(bytes),
                         wgpu::CallbackMode::AllowSpontaneous,
                         [&](wgpu::MapAsyncStatus status, wgpu::StringView)
                         {
                             mapped = status == wgpu::MapAsyncStatus::Success;
                             webgpuWake(done);
                         });

        webgpuWaitUntil(done);

        if (!mapped)
            return;

        const auto* from = static_cast<const std::uint8_t*>(
            staging.GetConstMappedRange(0, toHostSize(bytes)));
        auto* out = static_cast<std::uint8_t*>(dst);

        if (from != nullptr)
            for (auto image = std::uint32_t {0}; image < images; ++image)
                for (auto row = std::uint32_t {0}; row < rows; ++row)
                    std::memcpy(
                        out + image * static_cast<std::size_t>(plan->bytesPerImage)
                            + row * static_cast<std::size_t>(plan->bytesPerRow),
                        from
                            + (static_cast<std::size_t>(image) * rows + row)
                                  * stagingRow,
                        rowBytes);

        staging.Unmap();
    }

    WebGPUContext& context;
    TextureFormat format = TextureFormat::RGBA8Unorm;
    TextureShape shape;
    mutable WebGPUTextureData data;
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

    const auto plan =
        planSubresourceUploads(impl->shape, impl->format, subresources);

    if (!plan.valid)
    {
        impl->release();
        return;
    }

    impl->submitUploads(plan.uploads);
}

// No zero-copy pixel buffers on the web.
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

void Texture::updateSubresource(const TextureRegion& region, const void* pixels)
{
    impl->update(region, pixels);
}

void Texture::read(void* dst, int bytesPerRow) const
{
    if (bytesPerRow < 0)
        return;

    impl->readRegion(0, 0, impl->data.width, impl->data.height, dst, bytesPerRow);
}

void Texture::read(const Graphics::Rect& region, void* dst, int bytesPerRow) const
{
    if (bytesPerRow < 0)
        return;

    impl->readRegion(static_cast<int>(std::lround(region.x)),
                     static_cast<int>(std::lround(region.y)),
                     static_cast<int>(std::lround(region.w)),
                     static_cast<int>(std::lround(region.h)),
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

void* Texture::nativeSubresourceView(int, int, int, int) const
{
    return nullptr;
}

void* Texture::nativeLevelView(int) const
{
    return nullptr;
}

void* Texture::nativeArrayView(int) const
{
    return nullptr;
}

void* Texture::nativeDepthSliceView(int) const
{
    return nullptr;
}

int Texture::width() const
{
    return impl->data.width;
}

int Texture::height() const
{
    return impl->data.height;
}

TextureFormat Texture::format() const
{
    return impl->format;
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
    return isValid() && impl->data.renderTarget;
}

bool Texture::isCube() const
{
    return isValid() && impl->shape.kind == TextureType::Cube;
}

bool Texture::isComputeWritable() const
{
    return isValid() && impl->data.computeWrite;
}

bool Texture::hasDepth() const
{
    return isValid() && impl->data.depth != nullptr;
}

bool Texture::hasStencil() const
{
    return hasDepth() && isDepthWithStencil(impl->data.depthFormat);
}

bool Texture::hasSampleableDepth() const
{
    return hasDepth() && impl->data.sampleableDepth;
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
    return impl->data.depth.Get();
}

void* Texture::nativeMultisampleTexture() const
{
    return impl->data.multisample.Get();
}

void* Texture::nativeResolvedDepthTexture() const
{
    return nullptr;
}
} // namespace eacp::GPU
