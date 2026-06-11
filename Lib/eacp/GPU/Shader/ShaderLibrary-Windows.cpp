#include <eacp/Core/Utils/WinInclude.h>

#include "ShaderLibrary.h"

#include "../Device/Device.h"
#include "../Windows/D3D12Types.h"
#include "ShaderSource.h"

#include <eacp/Core/Utils/Logging.h>

#include <d3dcompiler.h>

#include <winrt/base.h>

// Windows/D3D12 backend. Compiles the HLSL source with FXC into vertex/pixel
// (or compute) bytecode; D3D12 consumes the blobs directly at pipeline
// creation, so no shader objects exist at this level. SM 5.0 DXBC remains
// valid input for D3D12 pipelines, which keeps the hand-written HLSL in tests
// and examples working unchanged.

namespace eacp::GPU
{
namespace
{
winrt::com_ptr<ID3DBlob> compileStage(const std::string& source,
                                      const std::string& entry,
                                      const char* target)
{
    winrt::com_ptr<ID3DBlob> code;
    winrt::com_ptr<ID3DBlob> errors;

    auto hr = D3DCompile(source.data(),
                         source.size(),
                         nullptr,
                         nullptr,
                         nullptr,
                         entry.c_str(),
                         target,
                         D3DCOMPILE_ENABLE_STRICTNESS,
                         0,
                         code.put(),
                         errors.put());

    if (FAILED(hr))
    {
        if (errors)
            LOG(static_cast<const char*>(errors->GetBufferPointer()));

        return nullptr;
    }

    return code;
}
} // namespace

struct ShaderLibrary::Native
{
    Native(Device& device, const ShaderSource& source)
    {
        if (!device.isValid())
            return;

        if (source.isCompute())
        {
            program.computeBytecode =
                compileStage(source.source, source.computeEntry, "cs_5_0");
        }
        else
        {
            program.vertexBytecode =
                compileStage(source.source, source.vertexEntry, "vs_5_0");
            program.pixelBytecode =
                compileStage(source.source, source.fragmentEntry, "ps_5_0");
        }
    }

    D3D12ShaderProgram program;
};

ShaderLibrary::ShaderLibrary(Device& device, const ShaderSource& source)
    : vertexEntryName(source.vertexEntry)
    , fragmentEntryName(source.fragmentEntry)
    , computeEntryName(source.computeEntry)
    , impl(device, source)
{
}

bool ShaderLibrary::isValid() const
{
    if (impl->program.computeBytecode != nullptr)
        return true;

    return impl->program.vertexBytecode != nullptr
           && impl->program.pixelBytecode != nullptr;
}

void* ShaderLibrary::nativeLibrary() const
{
    return const_cast<D3D12ShaderProgram*>(&impl->program);
}
} // namespace eacp::GPU
