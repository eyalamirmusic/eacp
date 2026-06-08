#include <eacp/Core/Utils/WinInclude.h>

#include "CommandBuffer.h"

#include "../Device/Device.h"
#include "../Windows/D3DTypes.h"

#include <d3d11.h>

// Windows/D3D11 backend. D3D11 records onto the immediate context with no
// separate command buffer, so this wraps that context; commit() flushes it. The
// staging copy in Buffer::read serialises behind the dispatch, so a read after
// commit sees the kernel's output.

namespace eacp::GPU
{
struct CommandBuffer::Native
{
    explicit Native(Device& device)
        : context(static_cast<ID3D11DeviceContext*>(device.nativeQueue()))
    {
    }

    ID3D11DeviceContext* context = nullptr;
};

CommandBuffer::CommandBuffer(Device& device)
    : impl(device)
{
}

ComputePass CommandBuffer::beginCompute()
{
    if (impl->context == nullptr)
        return ComputePass(nullptr);

    return ComputePass(new D3DComputeEncoder {impl->context});
}

void CommandBuffer::commit()
{
    if (impl->context != nullptr)
        impl->context->Flush();
}

bool CommandBuffer::isValid() const
{
    return impl->context != nullptr;
}
} // namespace eacp::GPU
