#include <eacp/Core/Utils/WinInclude.h>

#include "Texture.h"

#include "../Device/Device.h"
#include "../Windows/D3D12Types.h"

#include <cmath>

// Windows/D3D12 backend. A texture is a default-heap resource plus an SRV
// descriptor living in the context's shader-visible heap for the texture's
// whole lifetime, so binding is a single root-table update. Pixels
// upload through a transient row-pitch-aligned staging buffer; the resource
// then stays in PIXEL_SHADER_RESOURCE state forever (it is only ever sampled).

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

// The filter/address translations that used to live here are gone with the
// sampler descriptors they filled in; the equivalent mapping is now in
// D3D12Context::createRootSignatures, which builds the static samplers.

// Whether this device can take a typed UAV store to the format. The formats
// supportsComputeWrite() allows are the ones the specification guarantees, but
// a guarantee is not a driver, so it is asked rather than assumed - a kernel
// whose stores are silently dropped is a great deal harder to find than a
// texture that refused to be created.
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
    {
        auto& context = getD3D12Context();

        if (!context.isValid() || !device.isValid() || width <= 0 || height <= 0)
            return;

        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = static_cast<UINT64>(width);
        desc.Height = static_cast<UINT>(height);
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = toDXGIFormat(descriptor.format);
        desc.SampleDesc.Count = 1;

        if (descriptor.renderTarget)
            desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        // Refused before the resource exists, so a kernel output the device
        // cannot actually store to is an invalid texture rather than a dispatch
        // that writes nothing.
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

    // Zero-copy wrapping of a shared D3D11/DXGI surface into the D3D12 backend
    // is a planned optimisation; until then the camera/video path uploads via
    // update(). A null resource yields an invalid texture, which the higher
    // layer detects and falls back from.
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

    // Maps a staging buffer, copies each source row's pixels (advancing the
    // source by sourcePitch to skip any padding) into the
    // 256-byte-aligned staging rows GetCopyableFootprints reports, then records
    // the copy and the transition back to PIXEL_SHADER_RESOURCE. The resource
    // must already be in COPY_DEST. Returns false on a staging failure, having
    // recorded nothing.
    bool copyPixels(D3D12Context& context,
                    CommandContext* commands,
                    const void* pixels,
                    std::size_t sourcePitch,
                    int destX,
                    int destY,
                    int regionWidth,
                    int regionHeight)
    {
        auto desc = data.resource->GetDesc();

        // Footprints are asked for at the *region's* size, not the texture's, so
        // the staging buffer and its row pitch describe only what is being
        // uploaded. The copy is then placed at destX/destY in the destination.
        auto regionDesc = desc;
        regionDesc.Width = static_cast<UINT64>(regionWidth);
        regionDesc.Height = static_cast<UINT>(regionHeight);

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
        transitionTextureForUse(
            commands->list.get(), data, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // Not parked in transients: the staging pool owns the buffer and takes
        // it back once this recording's fence passes.
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

        // The resource was created in COPY_DEST, so the copy records with no
        // leading barrier.
        if (!copyPixels(context,
                        commands,
                        pixels,
                        static_cast<std::size_t>(width * pixelStride),
                        0,
                        0,
                        width,
                        height))
        {
            context.discard(commands);
            data.resource = nullptr;
            return;
        }

        context.submit(commands);
    }

    void update(const void* pixels, std::size_t bytesPerRow)
    {
        updateRegion(0, 0, width, height, pixels, bytesPerRow);
    }

    // Both update() overloads land here; the whole-texture one is just the full
    // rect, so there is a single upload path to reason about.
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

        // Out-of-bounds is dropped rather than clamped — see the header for why
        // clamping would silently upload skewed pixels.
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

        // The resource rests in PIXEL_SHADER_RESOURCE between frames - or in
        // RENDER_TARGET, if it is one a pass last drew into - so the move to
        // COPY_DEST goes through the tracked state rather than assuming which.
        // Staging failing puts it back, so the next bind still sees a sampleable
        // resource.
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

    // A render target's own one-descriptor RTV heap. RTV descriptors are not
    // shader-visible, so there is no shared heap to take one from the way the
    // SRV does - and a heap per target is what keeps a texture's descriptor
    // alive for exactly as long as the texture is.
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

    // The depth buffer a pass into this target attaches, and its DSV. Created
    // in DEPTH_WRITE and left there: nothing else ever uses the resource, so it
    // needs no barrier and no state tracking.
    //
    // The optimised clear value is not optional. D3D12 wants a resource that
    // will be cleared to say so at creation, and a ClearDepthStencilView that
    // does not match it is a validation error rather than a slow path - and it
    // must agree with the 1.0 the pass clears to.
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

    // The view a kernel writes through. It comes out of the same shader-visible
    // heap as the SRV - the allocator is CBV_SRV_UAV - so a writable texture
    // costs one more descriptor and no new heap.
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

        // No sampler descriptor is created, and TextureDescriptor::filter and
        // ::addressMode are unused on this backend: the sampler comes from the
        // root signature's static samplers, picked by the shader's declared
        // TextureSampling. A sampler descriptor table cannot be relied on here -
        // see D3D12Context::createRootSignatures. Allocating one anyway would
        // also let the 256-entry sampler heap fail texture creation for nothing.
    }

    int width = 0;
    int height = 0;

    // Bytes per pixel of the texture's format; the (stubbed) zero-copy wrap
    // path stays at 4 because those buffers are always 32-bit BGRA/RGBA.
    int pixelStride = 4;
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
    // Texels are whole; round rather than truncate so a rect built from
    // accumulated float arithmetic lands on the texel it is nearest to.
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

// The depth resource and its descriptor live inside the same D3D12TextureData
// nativeTexture hands back, which is what Frame reaches them through - so this
// backend has no separate handle to give.
void* Texture::nativeDepthTexture() const
{
    return nullptr;
}
} // namespace eacp::GPU
