#include <eacp/Core/Utils/WinInclude.h>

#include "ComputePipeline.h"

#include "../Device/Device.h"
#include "../Shader/ShaderLibrary.h"
#include "../Windows/D3D12Types.h"

#include <winrt/base.h>

// Windows/D3D12 backend. Bakes the library's compute bytecode and the shared
// compute root signature into a pipeline-state object; nativeState() hands it
// to the pass, which binds it with SetPipelineState.

namespace eacp::GPU
{
struct ComputePipeline::Native
{
    Native(Device&, const ShaderLibrary& library)
    {
        auto& context = getD3D12Context();

        if (!context.isValid() || context.getComputeRootSignature() == nullptr)
            return;

        auto* program = static_cast<D3D12ShaderProgram*>(library.nativeLibrary());

        if (program == nullptr || program->computeBytecode == nullptr)
            return;

        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = context.getComputeRootSignature();
        desc.CS.pShaderBytecode = program->computeBytecode->GetBufferPointer();
        desc.CS.BytecodeLength = program->computeBytecode->GetBufferSize();

        context.getDevice()->CreateComputePipelineState(
            &desc, __uuidof(ID3D12PipelineState), state.put_void());
    }

    winrt::com_ptr<ID3D12PipelineState> state;
};

ComputePipeline::ComputePipeline(Device& device, const ShaderLibrary& library)
    : impl(device, library)
{
}

bool ComputePipeline::isValid() const
{
    return impl->state != nullptr;
}

void* ComputePipeline::nativeState() const
{
    return impl->state.get();
}
} // namespace eacp::GPU
