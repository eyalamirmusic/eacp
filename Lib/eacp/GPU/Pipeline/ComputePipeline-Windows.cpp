#include <eacp/Core/Utils/WinInclude.h>

#include "ComputePipeline.h"

#include "../Device/Device.h"
#include "../Shader/ShaderLibrary.h"
#include "../Windows/D3DTypes.h"

#include <d3d11.h>

#include <winrt/base.h>

// Windows/D3D11 backend. D3D11 has no compute pipeline-state object, so this
// just holds the compute shader compiled into the library; nativeState() hands
// it to the pass, which binds it with CSSetShader.

namespace eacp::GPU
{
struct ComputePipeline::Native
{
    Native(Device&, const ShaderLibrary& library)
    {
        if (auto* program = static_cast<D3DShaderProgram*>(library.nativeLibrary()))
            shader = program->computeShader;
    }

    winrt::com_ptr<ID3D11ComputeShader> shader;
};

ComputePipeline::ComputePipeline(Device& device, const ShaderLibrary& library)
    : impl(device, library)
{
}

bool ComputePipeline::isValid() const
{
    return impl->shader != nullptr;
}

void* ComputePipeline::nativeState() const
{
    return impl->shader.get();
}
} // namespace eacp::GPU
