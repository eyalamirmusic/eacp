#include <eacp/Core/Utils/WinInclude.h>

#include "Buffer.h"

#include "../Device/Device.h"

#include <d3d11.h>

#include <winrt/base.h>

// Windows/D3D11 backend. See Device-Windows.cpp.

namespace eacp::GPU
{
struct Buffer::Native
{
    Native(Device& device, const void* data, std::size_t bytes)
        : length(bytes)
    {
        auto* d3dDevice = static_cast<ID3D11Device*>(device.nativeDevice());

        if (d3dDevice == nullptr || bytes == 0)
            return;

        D3D11_BUFFER_DESC descriptor = {};
        descriptor.ByteWidth = static_cast<UINT>(bytes);
        descriptor.Usage = D3D11_USAGE_DEFAULT;
        descriptor.BindFlags = D3D11_BIND_VERTEX_BUFFER;

        D3D11_SUBRESOURCE_DATA initialData = {};
        initialData.pSysMem = data;

        d3dDevice->CreateBuffer(
            &descriptor, data != nullptr ? &initialData : nullptr, buffer.put());
    }

    winrt::com_ptr<ID3D11Buffer> buffer;
    std::size_t length = 0;
};

Buffer::Buffer(Device& device, const void* data, std::size_t bytes)
    : impl(device, data, bytes)
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

void* Buffer::nativeBuffer() const
{
    return impl->buffer.get();
}
} // namespace eacp::GPU
