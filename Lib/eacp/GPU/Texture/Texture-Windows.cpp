#include <eacp/Core/Utils/WinInclude.h>

#include "Texture.h"

#include "../Device/Device.h"
#include "../Windows/D3D12Types.h"
#include "MipChain.h"

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
        : context(getD3D12Context(device))
        , width(descriptor.width)
        , height(descriptor.height)
        , pixelStride(bytesPerPixel(descriptor.format))
        , format(descriptor.format)
    {
        if (!context.isValid() || width <= 0 || height <= 0)
            return;

        // Only with pixels to build one from: a render target or a kernel output
        // has none at creation, so it would get levels nothing ever writes and
        // the sampler would read them.
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
    Native(Device& device, void*)
        : context(getD3D12Context(device))
    {
    }

    ~Native()
    {
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
                    int regionHeight,
                    int mipLevel = 0,
                    bool transitionAfterwards = true)
    {
        auto desc = data.resource->GetDesc();

        // Footprints are asked for at the *region's* size, not the texture's, so
        // the staging buffer and its row pitch describe only what is being
        // uploaded. The copy is then placed at destX/destY in the destination.
        auto regionDesc = desc;
        regionDesc.Width = static_cast<UINT64>(regionWidth);
        regionDesc.Height = static_cast<UINT>(regionHeight);

        // One level, always: this describes the *staging* side, which is a flat
        // rectangle of pixels. Leaving the resource's own count here would ask
        // for the footprint of a chain a region this size cannot have.
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

        // Held back while a chain is mid-upload: every level goes into the same
        // resource, so transitioning after each one would move it out of
        // COPY_DEST and straight back for the next - a pair of barriers per
        // level, for nothing.
        if (transitionAfterwards)
            transitionTextureForUse(commands->list.get(),
                                    data,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

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

        // The resource was created in COPY_DEST, so the copies record with no
        // leading barrier.
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

    // The whole texture, as one level or as a chain. Every level is copied
    // before the resource moves back to PIXEL_SHADER_RESOURCE, so the transition
    // happens once however many levels there are.
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

    // The re-upload path for a mipmapped texture. Unlike updateRegion it has to
    // move the resource back to COPY_DEST first, since by now it is being
    // sampled.
    void updateChain(const void* pixels, std::size_t bytesPerRow)
    {
        if (data.resource == nullptr || pixels == nullptr)
            return;

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

    // Both read() overloads land here, as the update() pair land in
    // updateRegion. The download side of copyPixels, and the same three pieces:
    // the footprint of the *region*, a pooled staging resource in the read-back
    // heap, and the row pitch it reports, which is 256-byte aligned and almost
    // never the row the caller wants.
    //
    // The submission is waited on rather than left running - the pixels are
    // wanted now, which is what a read-back is - and the resource is put back
    // where the rest of this file expects to find it.
    void readRegion(int x,
                    int y,
                    int regionWidth,
                    int regionHeight,
                    void* dst,
                    std::size_t bytesPerRow) const
    {
        if (data.resource == nullptr || dst == nullptr || !context.isValid())
            return;

        if (regionWidth <= 0 || regionHeight <= 0)
            return;

        // Dropped rather than clamped, exactly as the upload side drops it.
        if (x < 0 || y < 0 || x + regionWidth > width || y + regionHeight > height)
            return;

        auto regionDesc = data.resource->GetDesc();
        regionDesc.Width = static_cast<UINT64>(regionWidth);
        regionDesc.Height = static_cast<UINT>(regionHeight);
        regionDesc.MipLevels = 1;

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT rows = 0;
        UINT64 rowBytes = 0;
        UINT64 totalBytes = 0;
        context.getDevice()->GetCopyableFootprints(
            &regionDesc, 0, 1, 0, &footprint, &rows, &rowBytes, &totalBytes);

        auto* commands = context.acquire();

        if (commands == nullptr)
            return;

        auto* staging = context.acquireReadbackBuffer(*commands, totalBytes);

        if (staging == nullptr)
        {
            context.discard(commands);
            return;
        }

        transitionTextureForUse(
            commands->list.get(), data, D3D12_RESOURCE_STATE_COPY_SOURCE);

        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = staging;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;

        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = data.resource.get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;

        D3D12_BOX box = {};
        box.left = static_cast<UINT>(x);
        box.top = static_cast<UINT>(y);
        box.front = 0;
        box.right = static_cast<UINT>(x + regionWidth);
        box.bottom = static_cast<UINT>(y + regionHeight);
        box.back = 1;

        commands->list->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);

        // Back to where a texture rests between frames, so the next bind finds
        // a sampleable resource whatever this one was doing before the read.
        transitionTextureForUse(
            commands->list.get(), data, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // The copy went onto the same queue as the writes, so waiting for this
        // submission's fence also waits for them - which is the rule the header
        // states, and the same one Buffer::read leans on.
        context.waitFor(context.submit(commands));

        void* mapped = nullptr;
        const D3D12_RANGE readRange = {0, static_cast<SIZE_T>(totalBytes)};

        if (FAILED(staging->Map(0, &readRange, &mapped)) || mapped == nullptr)
            return;

        const auto stride =
            bytesPerRow != 0 ? bytesPerRow : static_cast<std::size_t>(rowBytes);

        auto* out = static_cast<unsigned char*>(dst);
        const auto* in = static_cast<const unsigned char*>(mapped);

        for (auto row = UINT {0}; row < rows; ++row)
            std::memcpy(
                out + static_cast<std::size_t>(row) * stride,
                in + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
                static_cast<std::size_t>(rowBytes));

        const D3D12_RANGE noWrite = {0, 0};
        staging->Unmap(0, &noWrite);
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
    void createDepthBuffer(D3D12Context& context, bool withStencil)
    {
        const auto format = depthAttachmentFormat(withStencil);

        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = static_cast<UINT64>(width);
        desc.Height = static_cast<UINT>(height);
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = format;
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
        viewDesc.Format = format;
        viewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

        auto handle = data.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        context.getDevice()->CreateDepthStencilView(
            data.depthResource.get(), &viewDesc, handle);

        data.dsv = handle;
        data.depthHasStencil = withStencil;
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

        if (descriptor.renderTarget && (descriptor.depth || descriptor.stencil)
            && data.rtv.ptr != 0)
            createDepthBuffer(context, descriptor.stencil);

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

    // The Device's context, held for the texture's lifetime: the descriptor
    // slots came out of its heaps and go back to them, and every upload runs
    // on its queue. A texture never moves between Devices.
    D3D12Context& context;

    int width = 0;
    int height = 0;

    // Bytes per pixel of the texture's format; the (stubbed) zero-copy wrap
    // path stays at 4 because those buffers are always 32-bit BGRA/RGBA.
    int pixelStride = 4;
    TextureFormat format = TextureFormat::RGBA8Unorm;

    // 1 unless a chain was asked for and there were pixels to build one from.
    // The SRV is created from a null description, which covers every level the
    // resource has, so nothing else here needs to know the count.
    int levels = 1;

    bool computeWrite = false;

    // Mutable because the state tracking advances inside the const read(): the
    // copy to the read-back buffer is a use like any other, and Buffer's
    // Native carries the same note for the same reason.
    mutable D3D12TextureData data;
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

void Texture::read(void* dst, std::size_t bytesPerRow) const
{
    impl->readRegion(0, 0, impl->width, impl->height, dst, bytesPerRow);
}

void Texture::read(const Graphics::Rect& region,
                   void* dst,
                   std::size_t bytesPerRow) const
{
    // Rounded rather than truncated, as update()'s region is.
    impl->readRegion(static_cast<int>(std::lround(region.x)),
                     static_cast<int>(std::lround(region.y)),
                     static_cast<int>(std::lround(region.w)),
                     static_cast<int>(std::lround(region.h)),
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

bool Texture::hasStencil() const
{
    return isValid() && impl->data.hasStencil();
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
