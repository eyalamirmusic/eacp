#include <eacp/Core/Utils/WinInclude.h>

#include "ShaderLibrary.h"

#include "../Device/Device.h"
#include "../Windows/D3D12Types.h"
#include "ShaderBinaryCache.h"
#include "ShaderSource.h"

#include <d3dcompiler.h>

#include <cstring>
#include <string>
#include <string_view>

#include <winrt/base.h>

// Windows/D3D12 backend. Compiles the HLSL source with FXC into vertex/pixel
// (or compute) bytecode; D3D12 consumes the blobs directly at pipeline
// creation, so no shader objects exist at this level. SM 5.0 DXBC remains
// valid input for D3D12 pipelines, which keeps the hand-written HLSL in tests
// and examples working unchanged.
//
// FXC is the slow half of building a pipeline, so what it produces is kept on
// disk (ShaderBinaryCache) and a later launch reads the bytecode back instead of
// compiling the same source again.

namespace eacp::GPU
{
namespace
{
constexpr auto compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;

std::string fxcIdentity()
{
    return "fxc-" + std::to_string(D3D_COMPILER_VERSION) + "-flags"
           + std::to_string(compileFlags);
}

std::string
    cacheKey(const std::string& source, const std::string& entry, const char* target)
{
    return entry + '\n' + target + '\n' + source;
}

winrt::com_ptr<ID3DBlob> blobHolding(const std::string& bytes)
{
    winrt::com_ptr<ID3DBlob> blob;

    if (FAILED(D3DCreateBlob(bytes.size(), blob.put())))
        return nullptr;

    std::memcpy(blob->GetBufferPointer(), bytes.data(), bytes.size());
    return blob;
}

winrt::com_ptr<ID3DBlob> compileStage(const std::string& source,
                                      const std::string& entry,
                                      const char* target)
{
    auto key = cacheKey(source, entry, target);

    if (auto cached = ShaderBinaryCache::load(fxcIdentity(), key))
        if (auto blob = blobHolding(*cached))
            return blob;

    winrt::com_ptr<ID3DBlob> code;
    winrt::com_ptr<ID3DBlob> errors;

    auto hr = D3DCompile(source.data(),
                         source.size(),
                         nullptr,
                         nullptr,
                         nullptr,
                         entry.c_str(),
                         target,
                         compileFlags,
                         0,
                         code.put(),
                         errors.put());

    if (FAILED(hr))
    {
        if (errors)
            LOG(static_cast<const char*>(errors->GetBufferPointer()));

        return nullptr;
    }

    ShaderBinaryCache::store(
        fxcIdentity(),
        key,
        std::string_view {static_cast<const char*>(code->GetBufferPointer()),
                          code->GetBufferSize()});

    return code;
}
} // namespace

struct ShaderLibrary::Native
{
    Native(Device& device, const ShaderSource& source)
    {
        // An empty source is a build something declined to make, and whatever
        // declined it has already said why - see ComputeProgram::prepare. There
        // is nothing here to compile and nothing for FXC to complain about.
        if (!device.isValid() || source.source.empty())
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
    , groupShape(source.threadGroup)
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
