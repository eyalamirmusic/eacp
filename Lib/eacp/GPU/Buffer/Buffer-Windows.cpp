#include <eacp/Core/Utils/WinInclude.h>

#include "Buffer.h"

#include "../Device/Device.h"

#include <d3d11.h>

#include <winrt/base.h>

#include <cstring>

// Windows/D3D11 backend. See Device-Windows.cpp. A Storage buffer is a
// structured buffer with both a shader-resource view (read) and an
// unordered-access view (write) so a compute pass can bind it either way; a
// DEFAULT buffer is not CPU-readable, so read() copies through a staging buffer.

namespace eacp::GPU
{
namespace
{
// Storage elements are 4-byte floats in this first cut (see the compute plan);
// typed elements arrive with the shader EDSL.
constexpr UINT storageStride = 4;
} // namespace

struct Buffer::Native
{
    Native(Device& device, const void* data, std::size_t bytes, BufferUsage usage)
        : length(bytes)
    {
        auto* d3dDevice = static_cast<ID3D11Device*>(device.nativeDevice());

        if (d3dDevice == nullptr || bytes == 0)
            return;

        auto storage = usage == BufferUsage::Storage;

        D3D11_BUFFER_DESC descriptor = {};
        descriptor.ByteWidth = static_cast<UINT>(bytes);
        descriptor.Usage = D3D11_USAGE_DEFAULT;
        descriptor.BindFlags =
            storage ? D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE
                    : D3D11_BIND_VERTEX_BUFFER;

        if (storage)
        {
            descriptor.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            descriptor.StructureByteStride = storageStride;
        }

        D3D11_SUBRESOURCE_DATA initialData = {};
        initialData.pSysMem = data;

        if (FAILED(d3dDevice->CreateBuffer(&descriptor,
                                           data != nullptr ? &initialData : nullptr,
                                           buffer.put())))
            return;

        if (storage)
            makeViews(d3dDevice, static_cast<UINT>(bytes / storageStride));
    }

    void makeViews(ID3D11Device* d3dDevice, UINT elements)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN; // required for a structured buffer
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.NumElements = elements;
        d3dDevice->CreateShaderResourceView(buffer.get(), &srvDesc, readView.put());

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = elements;
        d3dDevice->CreateUnorderedAccessView(
            buffer.get(), &uavDesc, writeView.put());
    }

    winrt::com_ptr<ID3D11Buffer> buffer;
    winrt::com_ptr<ID3D11ShaderResourceView> readView;
    winrt::com_ptr<ID3D11UnorderedAccessView> writeView;
    std::size_t length = 0;
};

Buffer::Buffer(Device& device,
               const void* data,
               std::size_t bytes,
               BufferUsage usage)
    : impl(device, data, bytes, usage)
{
}

std::size_t Buffer::size() const
{
    return impl->length;
}

bool Buffer::isValid() const
{
    return impl->buffer != nullptr;
}

void Buffer::read(void* dst, std::size_t bytes) const
{
    auto* source = impl->buffer.get();

    if (source == nullptr)
        return;

    winrt::com_ptr<ID3D11Device> device;
    source->GetDevice(device.put());

    winrt::com_ptr<ID3D11DeviceContext> context;
    device->GetImmediateContext(context.put());

    // A DEFAULT buffer cannot be mapped, so copy into a CPU-readable staging
    // buffer and map that. CopyResource also serialises behind the dispatch.
    D3D11_BUFFER_DESC descriptor = {};
    descriptor.ByteWidth = static_cast<UINT>(impl->length);
    descriptor.Usage = D3D11_USAGE_STAGING;
    descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    winrt::com_ptr<ID3D11Buffer> staging;

    if (FAILED(device->CreateBuffer(&descriptor, nullptr, staging.put())))
        return;

    context->CopyResource(staging.get(), source);

    D3D11_MAPPED_SUBRESOURCE mapped = {};

    if (SUCCEEDED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
    {
        std::memcpy(dst, mapped.pData, bytes);
        context->Unmap(staging.get(), 0);
    }
}

void* Buffer::nativeBuffer() const
{
    return impl->buffer.get();
}

void* Buffer::nativeReadView() const
{
    return impl->readView.get();
}

void* Buffer::nativeWriteView() const
{
    return impl->writeView.get();
}
} // namespace eacp::GPU
