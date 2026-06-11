#include <eacp/Core/Utils/WinInclude.h>

#include "CommandBuffer.h"

#include "../Device/Device.h"
#include "../Windows/D3D12Types.h"

// Windows/D3D12 backend. Owns one CommandContext recording for its lifetime:
// passes record onto its list, commit() executes it on the direct queue, and
// an uncommitted recording is discarded on destruction. The fence wait inside
// Buffer::read serialises behind the committed work, so a read after commit
// sees the kernel's output.

namespace eacp::GPU
{
struct CommandBuffer::Native
{
    explicit Native(Device& device)
    {
        if (device.isValid())
            commands = getD3D12Context().acquire();
    }

    ~Native()
    {
        if (commands != nullptr && !committed)
            getD3D12Context().discard(commands);
    }

    CommandContext* commands = nullptr;
    bool committed = false;
};

CommandBuffer::CommandBuffer(Device& device)
    : impl(device)
{
}

ComputePass CommandBuffer::beginCompute()
{
    if (impl->commands == nullptr || impl->committed)
        return ComputePass(nullptr);

    // The root signature is fixed for every compute pipeline, so binding it
    // here frees the pass from caring about setPipeline/set*Buffer ordering.
    impl->commands->list->SetComputeRootSignature(
        getD3D12Context().getComputeRootSignature());

    return ComputePass(new D3D12ComputeEncoder {impl->commands});
}

void CommandBuffer::commit()
{
    if (impl->commands == nullptr || impl->committed)
        return;

    impl->committed = true;
    getD3D12Context().submit(impl->commands);
}

bool CommandBuffer::isValid() const
{
    return impl->commands != nullptr;
}
} // namespace eacp::GPU
