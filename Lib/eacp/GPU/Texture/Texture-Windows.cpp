#include <eacp/Core/Utils/WinInclude.h>

#include "Texture.h"

#include "../Device/Device.h"

#include <d3d11.h>

#include <winrt/base.h>

// Windows/D3D11 backend. A texture is an ID3D11Texture2D plus the
// shader-resource view the fragment stage binds and the sampler state baked
// from the descriptor, mirroring the Metal texture + sampler pair.

namespace eacp::GPU
{
namespace
{
DXGI_FORMAT toDXGIFormat(TextureFormat format)
{
    return format == TextureFormat::BGRA8Unorm ? DXGI_FORMAT_B8G8R8A8_UNORM
                                               : DXGI_FORMAT_R8G8B8A8_UNORM;
}

D3D11_FILTER toD3DFilter(TextureFilter filter)
{
    return filter == TextureFilter::Nearest ? D3D11_FILTER_MIN_MAG_MIP_POINT
                                            : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
}

D3D11_TEXTURE_ADDRESS_MODE toD3DAddressMode(TextureAddressMode mode)
{
    return mode == TextureAddressMode::Repeat ? D3D11_TEXTURE_ADDRESS_WRAP
                                              : D3D11_TEXTURE_ADDRESS_CLAMP;
}
} // namespace

struct Texture::Native
{
    Native(Device& device, const TextureDescriptor& descriptor, const void* pixels)
        : width(descriptor.width)
        , height(descriptor.height)
    {
        auto* d3dDevice = static_cast<ID3D11Device*>(device.nativeDevice());

        if (d3dDevice == nullptr || width <= 0 || height <= 0)
            return;

        D3D11_TEXTURE2D_DESC textureDescriptor = {};
        textureDescriptor.Width = static_cast<UINT>(width);
        textureDescriptor.Height = static_cast<UINT>(height);
        textureDescriptor.MipLevels = 1;
        textureDescriptor.ArraySize = 1;
        textureDescriptor.Format = toDXGIFormat(descriptor.format);
        textureDescriptor.SampleDesc.Count = 1;
        textureDescriptor.Usage = D3D11_USAGE_DEFAULT;
        textureDescriptor.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initialData = {};
        initialData.pSysMem = pixels;
        initialData.SysMemPitch = static_cast<UINT>(width) * 4;

        if (FAILED(d3dDevice->CreateTexture2D(&textureDescriptor,
                                              pixels != nullptr ? &initialData
                                                                : nullptr,
                                              texture.put())))
            return;

        d3dDevice->CreateShaderResourceView(texture.get(), nullptr, readView.put());

        D3D11_SAMPLER_DESC samplerDescriptor = {};
        samplerDescriptor.Filter = toD3DFilter(descriptor.filter);
        samplerDescriptor.AddressU = toD3DAddressMode(descriptor.addressMode);
        samplerDescriptor.AddressV = toD3DAddressMode(descriptor.addressMode);
        samplerDescriptor.AddressW = toD3DAddressMode(descriptor.addressMode);
        samplerDescriptor.MaxLOD = D3D11_FLOAT32_MAX;

        d3dDevice->CreateSamplerState(&samplerDescriptor, sampler.put());
    }

    int width = 0;
    int height = 0;
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::com_ptr<ID3D11ShaderResourceView> readView;
    winrt::com_ptr<ID3D11SamplerState> sampler;
};

Texture::Texture(Device& device,
                 const TextureDescriptor& descriptor,
                 const void* pixels)
    : impl(device, descriptor, pixels)
{
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
    return impl->readView != nullptr && impl->sampler != nullptr;
}

void* Texture::nativeTexture() const
{
    return impl->texture.get();
}

void* Texture::nativeSampler() const
{
    return impl->sampler.get();
}

void* Texture::nativeReadView() const
{
    return impl->readView.get();
}
} // namespace eacp::GPU
