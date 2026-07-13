#include <eacp/Core/Utils/WinInclude.h>

#include "Texture.h"

#include "../Device/Device.h"
#include "../Windows/D3D12Types.h"

// Windows/D3D12 backend. A texture is a default-heap resource plus an SRV and
// a sampler descriptor living in the context's shader-visible heaps for the
// texture's whole lifetime, so binding is a single root-table update. Pixels
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
        default:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

D3D12_FILTER toD3DFilter(TextureFilter filter)
{
    return filter == TextureFilter::Nearest ? D3D12_FILTER_MIN_MAG_MIP_POINT
                                            : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
}

D3D12_TEXTURE_ADDRESS_MODE toD3DAddressMode(TextureAddressMode mode)
{
    return mode == TextureAddressMode::Repeat ? D3D12_TEXTURE_ADDRESS_MODE_WRAP
                                              : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
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

        auto initialState = pixels != nullptr
                                ? D3D12_RESOURCE_STATE_COPY_DEST
                                : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

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
    Native(Device&, void*, TextureFilter, TextureAddressMode) {}

    ~Native()
    {
        auto& context = getD3D12Context();
        context.freeTextureDescriptor(data.srv);
        context.freeSamplerDescriptor(data.sampler);
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
                    std::size_t sourcePitch)
    {
        auto desc = data.resource->GetDesc();

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT rows = 0;
        UINT64 rowBytes = 0;
        UINT64 totalBytes = 0;
        context.getDevice()->GetCopyableFootprints(
            &desc, 0, 1, 0, &footprint, &rows, &rowBytes, &totalBytes);

        auto staging = context.makeUploadBuffer(nullptr, totalBytes);

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
        source.pResource = staging.get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint;

        commands->list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        transition(commands->list.get(),
                   data.resource.get(),
                   D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        commands->transients.add(std::move(staging));
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
                        static_cast<std::size_t>(width * pixelStride)))
        {
            context.discard(commands);
            data.resource = nullptr;
            return;
        }

        context.submit(commands);
    }

    void update(const void* pixels, std::size_t bytesPerRow)
    {
        if (data.resource == nullptr || pixels == nullptr || width <= 0
            || height <= 0)
            return;

        auto& context = getD3D12Context();

        if (!context.isValid())
            return;

        auto* commands = context.acquire();

        if (commands == nullptr)
            return;

        auto sourcePitch = bytesPerRow != 0
                               ? bytesPerRow
                               : static_cast<std::size_t>(width * pixelStride);

        // The resource rests in PIXEL_SHADER_RESOURCE between frames; move it to
        // COPY_DEST for the upload, and put it back if staging fails so the next
        // bind still sees a sampleable resource.
        transition(commands->list.get(),
                   data.resource.get(),
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_DEST);

        if (!copyPixels(context, commands, pixels, sourcePitch))
            transition(commands->list.get(),
                       data.resource.get(),
                       D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        context.submit(commands);
    }

    void createDescriptors(D3D12Context& context,
                           const TextureDescriptor& descriptor)
    {
        data.srv = context.allocateTextureDescriptor();
        data.sampler = context.allocateSamplerDescriptor();

        if (data.srv.cpu.ptr == 0 || data.sampler.cpu.ptr == 0)
            return;

        context.getDevice()->CreateShaderResourceView(
            data.resource.get(), nullptr, data.srv.cpu);

        D3D12_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = toD3DFilter(descriptor.filter);
        samplerDesc.AddressU = toD3DAddressMode(descriptor.addressMode);
        samplerDesc.AddressV = toD3DAddressMode(descriptor.addressMode);
        samplerDesc.AddressW = toD3DAddressMode(descriptor.addressMode);
        samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;

        context.getDevice()->CreateSampler(&samplerDesc, data.sampler.cpu);
    }

    int width = 0;
    int height = 0;

    // Bytes per pixel of the texture's format; the (stubbed) zero-copy wrap
    // path stays at 4 because those buffers are always 32-bit BGRA/RGBA.
    int pixelStride = 4;
    D3D12TextureData data;
};

Texture::Texture(Device& device,
                 const TextureDescriptor& descriptor,
                 const void* pixels)
    : impl(device, descriptor, pixels)
{
}

Texture::Texture(Device& device,
                 void* nativePixelBuffer,
                 TextureFilter filter,
                 TextureAddressMode addressMode)
    : impl(device, nativePixelBuffer, filter, addressMode)
{
}

void Texture::update(const void* pixels, std::size_t bytesPerRow)
{
    impl->update(pixels, bytesPerRow);
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
    return impl->data.resource != nullptr && impl->data.srv.cpu.ptr != 0
           && impl->data.sampler.cpu.ptr != 0;
}

void* Texture::nativeTexture() const
{
    return const_cast<D3D12TextureData*>(&impl->data);
}

void* Texture::nativeSampler() const
{
    return const_cast<D3D12TextureData*>(&impl->data);
}

void* Texture::nativeReadView() const
{
    return const_cast<D3D12TextureData*>(&impl->data);
}
} // namespace eacp::GPU
