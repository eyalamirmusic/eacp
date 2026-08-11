#include <eacp/Core/Utils/WinInclude.h>

#include "Texture.h"

#include "../Device/Device.h"
#include "../Windows/D3D12Types.h"
#include "MipChain.h"

#include <cmath>

namespace eacp::GPU
{
namespace
{
DXGI_FORMAT toDXGIFormat(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::BGRA8Unorm:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case TextureFormat::R8Unorm:
            return DXGI_FORMAT_R8_UNORM;
        case TextureFormat::RG8Unorm:
            return DXGI_FORMAT_R8G8_UNORM;
        case TextureFormat::RGBA16Float:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case TextureFormat::RGBA32Float:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        default:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

// Asked rather than assumed: supportsComputeWrite() is what the spec
// guarantees, and a driver dropping stores silently is hard to find.
bool supportsTypedUAVStore(ID3D12Device* device, DXGI_FORMAT format)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {};
    support.Format = format;

    if (FAILED(device->CheckFeatureSupport(
            D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))))
        return false;

    return (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}
} // namespace

struct Texture::Native
{
    Native(Device& device, const TextureDescriptor& descriptor, const void* pixels)
        : width(descriptor.width)
        , height(descriptor.height)
        , pixelStride(bytesPerPixel(descriptor.format))
        , format(descriptor.format)
    {
        auto& context = getD3D12Context();

        if (!context.isValid() || !device.isValid() || width <= 0 || height <= 0)
            return;

        // Without pixels a chain would get levels nothing ever writes.
        if (descriptor.mipmapped && pixels != nullptr)
            levels = mipLevelCount(width, height);

        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = static_cast<UINT64>(width);
        desc.Height = static_cast<UINT>(height);
        desc.DepthOrArraySize = 1;
        desc.MipLevels = static_cast<UINT16>(levels);
        desc.Format = toDXGIFormat(descriptor.format);
        desc.SampleDesc.Count = 1;

        if (descriptor.renderTarget)
            desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        // Refused before the resource exists, so a target the device cannot
        // store to is an invalid texture rather than a dispatch writing nothing.
        if (descriptor.computeWrite)
        {
            if (!supportsComputeWrite(descriptor.format)
                || !supportsTypedUAVStore(context.getDevice(), desc.Format))
                return;

            desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            computeWrite = true;
        }

        auto initialState = pixels != nullptr
                                ? D3D12_RESOURCE_STATE_COPY_DEST
                                : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        data.state = initialState;

        if (FAILED(context.getDevice()->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                initialState,
                nullptr,
                __uuidof(ID3D12Resource),
                data.resource.put_void())))
            return;

        if (pixels != nullptr)
            upload(context, pixels);

        if (data.resource != nullptr)
            createDescriptors(context, descriptor);
    }

    // No zero-copy wrap on D3D12 yet: the null resource yields an invalid
    // texture, which the higher layer falls back from to update().
    Native(Device&, void*) {}

    ~Native()
    {
        auto& context = getD3D12Context();
        context.freeTextureDescriptor(data.srv);
        context.freeTextureDescriptor(data.uav);
        context.deferRelease(std::move(data.rtvHeap));
        context.deferRelease(std::move(data.dsvHeap));
        context.deferRelease(std::move(data.depthResource));
        context.deferRelease(std::move(data.resource));
    }

    // The resource must already be in COPY_DEST. Returns false on a staging
    // failure, having recorded nothing.
    bool copyPixels(D3D12Context& context,
                    CommandContext* commands,
                    const void* pixels,
                    std::size_t sourcePitch,
                    int destX,
                    int destY,
                    int regionWidth,
                    int regionHeight,
                    int mipLevel = 0,
                    bool transitionAfterwards = true)
    {
        auto desc = data.resource->GetDesc();

        // Footprints describe the *staging* side: the region's size, and always
        // one flat level, whatever chain the destination resource has.
        auto regionDesc = desc;
        regionDesc.Width = static_cast<UINT64>(regionWidth);
        regionDesc.Height = static_cast<UINT>(regionHeight);
        regionDesc.MipLevels = 1;

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT rows = 0;
        UINT64 rowBytes = 0;
        UINT64 totalBytes = 0;
        context.getDevice()->GetCopyableFootprints(
            &regionDesc, 0, 1, 0, &footprint, &rows, &rowBytes, &totalBytes);

        auto* staging = context.acquireStagingBuffer(*commands, totalBytes);

        if (staging == nullptr)
            return false;

        void* mapped = nullptr;
        const D3D12_RANGE noRead = {0, 0};

        if (FAILED(staging->Map(0, &noRead, &mapped)))
            return false;

        auto copyBytes = static_cast<std::size_t>(rowBytes);

        for (auto row = UINT {0}; row < rows; ++row)
            std::memcpy(static_cast<unsigned char*>(mapped)
                            + row * footprint.Footprint.RowPitch,
                        static_cast<const unsigned char*>(pixels)
                            + row * sourcePitch,
                        copyBytes);

        staging->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = data.resource.get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = static_cast<UINT>(mipLevel);

        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = staging;
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint;

        commands->list->CopyTextureRegion(&destination,
                                          static_cast<UINT>(destX),
                                          static_cast<UINT>(destY),
                                          0,
                                          &source,
                                          nullptr);

        // Held back mid-chain: every level shares the resource, so per-level
        // transitions would leave and re-enter COPY_DEST for nothing.
        if (transitionAfterwards)
            transitionTextureForUse(commands->list.get(),
                                    data,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        return true;
    }

    void upload(D3D12Context& context, const void* pixels)
    {
        auto* commands = context.acquire();

        if (commands == nullptr)
        {
            data.resource = nullptr;
            return;
        }

        // Created in COPY_DEST, so the copies need no leading barrier.
        if (!recordLevels(context,
                          commands,
                          pixels,
                          static_cast<std::size_t>(width * pixelStride)))
        {
            context.discard(commands);
            data.resource = nullptr;
            return;
        }

        context.submit(commands);
    }

    // Every level is copied before the one transition back to
    // PIXEL_SHADER_RESOURCE.
    bool recordLevels(D3D12Context& context,
                      CommandContext* commands,
                      const void* pixels,
                      std::size_t sourcePitch)
    {
        if (levels <= 1)
            return copyPixels(
                context, commands, pixels, sourcePitch, 0, 0, width, height);

        const auto chain = buildMipChain(pixels, width, height, format, sourcePitch);

        if (!chain.isValid())
            return false;

        for (auto level = 0; level < levels; ++level)
        {
            const auto levelWidth = mipExtent(width, level);
            const auto levelHeight = mipExtent(height, level);

            if (!copyPixels(context,
                            commands,
                            chain.level(level),
                            static_cast<std::size_t>(levelWidth * pixelStride),
                            0,
                            0,
                            levelWidth,
                            levelHeight,
                            level,
                            level == levels - 1))
                return false;
        }

        return true;
    }

    void update(const void* pixels, std::size_t bytesPerRow)
    {
        if (levels > 1)
        {
            updateChain(pixels, bytesPerRow);
            return;
        }

        updateRegion(0, 0, width, height, pixels, bytesPerRow);
    }

    void updateChain(const void* pixels, std::size_t bytesPerRow)
    {
        if (data.resource == nullptr || pixels == nullptr)
            return;

        auto& context = getD3D12Context();
        auto* commands = context.acquire();

        if (commands == nullptr)
            return;

        transitionTextureForUse(
            commands->list.get(), data, D3D12_RESOURCE_STATE_COPY_DEST);

        const auto pitch = bytesPerRow > 0
                               ? bytesPerRow
                               : static_cast<std::size_t>(width * pixelStride);

        if (!recordLevels(context, commands, pixels, pitch))
        {
            context.discard(commands);
            return;
        }

        context.submit(commands);
    }

    void updateRegion(int x,
                      int y,
                      int regionWidth,
                      int regionHeight,
                      const void* pixels,
                      std::size_t bytesPerRow)
    {
        if (data.resource == nullptr || pixels == nullptr || width <= 0
            || height <= 0)
            return;

        if (regionWidth <= 0 || regionHeight <= 0)
            return;

        // Dropped rather than clamped, which would upload skewed pixels.
        if (x < 0 || y < 0 || x + regionWidth > width || y + regionHeight > height)
            return;

        auto& context = getD3D12Context();

        if (!context.isValid())
            return;

        auto* commands = context.acquire();

        if (commands == nullptr)
            return;

        auto sourcePitch = bytesPerRow != 0
                               ? bytesPerRow
                               : static_cast<std::size_t>(regionWidth * pixelStride);

        // Through the tracked state: the resource rests in either
        // PIXEL_SHADER_RESOURCE or RENDER_TARGET between frames.
        transitionTextureForUse(
            commands->list.get(), data, D3D12_RESOURCE_STATE_COPY_DEST);

        if (!copyPixels(context,
                        commands,
                        pixels,
                        sourcePitch,
                        x,
                        y,
                        regionWidth,
                        regionHeight))
            transitionTextureForUse(commands->list.get(),
                                    data,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        context.submit(commands);
    }

    // A heap of its own: RTV descriptors are not shader-visible, so there is no
    // shared heap to take one from as the SRV does.
    void createRenderTargetView(D3D12Context& context,
                                const TextureDescriptor& descriptor)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = 1;

        if (FAILED(context.getDevice()->CreateDescriptorHeap(
                &heapDesc, __uuidof(ID3D12DescriptorHeap), data.rtvHeap.put_void())))
            return;

        D3D12_RENDER_TARGET_VIEW_DESC viewDesc = {};
        viewDesc.Format = toDXGIFormat(descriptor.format);
        viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

        auto handle = data.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        context.getDevice()->CreateRenderTargetView(
            data.resource.get(), &viewDesc, handle);

        data.rtv = handle;
    }

    // Created in DEPTH_WRITE and left there, nothing else using the resource.
    // The optimised clear value must match the 1.0 the pass clears to, or
    // ClearDepthStencilView is a validation error.
    void createDepthBuffer(D3D12Context& context)
    {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = static_cast<UINT64>(width);
        desc.Height = static_cast<UINT>(height);
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_D32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil.Depth = 1.f;

        if (FAILED(context.getDevice()->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                &clearValue,
                __uuidof(ID3D12Resource),
                data.depthResource.put_void())))
            return;

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heapDesc.NumDescriptors = 1;

        if (FAILED(context.getDevice()->CreateDescriptorHeap(
                &heapDesc, __uuidof(ID3D12DescriptorHeap), data.dsvHeap.put_void())))
        {
            data.depthResource = nullptr;
            return;
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC viewDesc = {};
        viewDesc.Format = DXGI_FORMAT_D32_FLOAT;
        viewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

        auto handle = data.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        context.getDevice()->CreateDepthStencilView(
            data.depthResource.get(), &viewDesc, handle);

        data.dsv = handle;
    }

    // Out of the same CBV_SRV_UAV heap as the SRV, so this costs one more
    // descriptor and no new heap.
    void createUnorderedAccessView(D3D12Context& context,
                                   const TextureDescriptor& descriptor)
    {
        data.uav = context.allocateTextureDescriptor();

        if (data.uav.cpu.ptr == 0)
        {
            computeWrite = false;
            return;
        }

        D3D12_UNORDERED_ACCESS_VIEW_DESC viewDesc = {};
        viewDesc.Format = toDXGIFormat(descriptor.format);
        viewDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        context.getDevice()->CreateUnorderedAccessView(
            data.resource.get(), nullptr, &viewDesc, data.uav.cpu);
    }

    void createDescriptors(D3D12Context& context,
                           const TextureDescriptor& descriptor)
    {
        if (descriptor.renderTarget)
            createRenderTargetView(context, descriptor);

        if (descriptor.renderTarget && descriptor.depth && data.rtv.ptr != 0)
            createDepthBuffer(context);

        if (computeWrite)
            createUnorderedAccessView(context, descriptor);

        data.srv = context.allocateTextureDescriptor();

        if (data.srv.cpu.ptr == 0)
            return;

        context.getDevice()->CreateShaderResourceView(
            data.resource.get(), nullptr, data.srv.cpu);

        // No sampler descriptor: samplers are static in the root signature,
        // picked by the shader's declared TextureSampling. See
        // D3D12Context::createRootSignatures.
    }

    int width = 0;
    int height = 0;

    // The zero-copy wrap path stays at 4: those buffers are 32-bit BGRA/RGBA.
    int pixelStride = 4;
    TextureFormat format = TextureFormat::RGBA8Unorm;

    int levels = 1;

    bool computeWrite = false;
    D3D12TextureData data;
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
    // Rounded, not truncated, so a rect from accumulated float arithmetic lands
    // on the nearest texel.
    impl->updateRegion(static_cast<int>(std::lround(region.x)),
                       static_cast<int>(std::lround(region.y)),
                       static_cast<int>(std::lround(region.w)),
                       static_cast<int>(std::lround(region.h)),
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
    return impl->data.resource != nullptr && impl->data.srv.cpu.ptr != 0;
}

int Texture::mipLevels() const
{
    return impl->levels;
}

bool Texture::isRenderTarget() const
{
    return isValid() && impl->data.isRenderTarget();
}

bool Texture::isComputeWritable() const
{
    return isValid() && impl->data.isComputeWritable();
}

bool Texture::hasDepth() const
{
    return isValid() && impl->data.hasDepth();
}

void* Texture::nativeTexture() const
{
    return const_cast<D3D12TextureData*>(&impl->data);
}

void* Texture::nativeReadView() const
{
    return const_cast<D3D12TextureData*>(&impl->data);
}

// The depth resource lives inside the D3D12TextureData nativeTexture returns.
void* Texture::nativeDepthTexture() const
{
    return nullptr;
}
} // namespace eacp::GPU
