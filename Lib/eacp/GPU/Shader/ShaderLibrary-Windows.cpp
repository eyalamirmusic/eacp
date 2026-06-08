#include <eacp/Core/Utils/WinInclude.h>

#include "ShaderLibrary.h"

#include "../Device/Device.h"
#include "../Windows/D3DTypes.h"
#include "ShaderSource.h"

#include <eacp/Core/Utils/Logging.h>

#include <d3d11.h>
#include <d3dcompiler.h>

#include <winrt/base.h>

// Windows/D3D11 backend. Compiles the HLSL source into a vertex and pixel shader
// (D3D11 keeps them as separate objects, unlike a single Metal library) and
// keeps the vertex bytecode so the pipeline can validate its input layout.

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
        auto* d3dDevice = static_cast<ID3D11Device*>(device.nativeDevice());

        if (d3dDevice == nullptr)
            return;

        if (source.isCompute())
            compileCompute(d3dDevice, source);
        else
            compileRender(d3dDevice, source);
    }

    void compileRender(ID3D11Device* d3dDevice, const ShaderSource& source)
    {
        auto vertexBlob = compileStage(source.source, source.vertexEntry, "vs_5_0");
        auto fragmentBlob =
            compileStage(source.source, source.fragmentEntry, "ps_5_0");

        if (!vertexBlob || !fragmentBlob)
            return;

        d3dDevice->CreateVertexShader(vertexBlob->GetBufferPointer(),
                                      vertexBlob->GetBufferSize(),
                                      nullptr,
                                      program.vertexShader.put());

        d3dDevice->CreatePixelShader(fragmentBlob->GetBufferPointer(),
                                     fragmentBlob->GetBufferSize(),
                                     nullptr,
                                     program.pixelShader.put());

        program.vertexBytecode = vertexBlob;
    }

    void compileCompute(ID3D11Device* d3dDevice, const ShaderSource& source)
    {
        auto computeBlob =
            compileStage(source.source, source.computeEntry, "cs_5_0");

        if (!computeBlob)
            return;

        d3dDevice->CreateComputeShader(computeBlob->GetBufferPointer(),
                                       computeBlob->GetBufferSize(),
                                       nullptr,
                                       program.computeShader.put());
    }

    D3DShaderProgram program;
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
    if (impl->program.computeShader != nullptr)
        return true;

    return impl->program.vertexShader != nullptr
           && impl->program.pixelShader != nullptr;
}

void* ShaderLibrary::nativeLibrary() const
{
    return const_cast<D3DShaderProgram*>(&impl->program);
}
} // namespace eacp::GPU
