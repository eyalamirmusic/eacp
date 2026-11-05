#include <eacp/Core/Utils/WinInclude.h>

#include "ShaderLibrary.h"

#include "../Device/Device.h"
#include "../Windows/D3D12Types.h"
#include "DxcCompiler-Windows.h"
#include "PrecompiledShaders.h"
#include "ShaderSource.h"

#include <d3dcompiler.h>

#include <winrt/base.h>

#include <cstdint>
#include <cstring>
#include <utility>

// Windows/D3D12 backend. Compiles the HLSL source with FXC into vertex/pixel
// (or compute) bytecode; D3D12 consumes the blobs directly at pipeline
// creation, so no shader objects exist at this level. SM 5.0 DXBC remains
// valid input for D3D12 pipelines, which keeps the hand-written HLSL in tests
// and examples working unchanged. A precompiled library is the same DXBC,
// compiled by the same FXC call at build time and copied into a blob here.
// With Dxc::Options::enabled the source goes to DXC instead, as SM 6.0 DXIL in
// the same blobs (Dxc.h).

namespace eacp::GPU
{
namespace
{
using DxcCompiler = std::shared_ptr<const Dxc::Detail::Compiler>;

struct StageTarget
{
    const char* fxc;
    const char* dxc;
};

constexpr auto vertexTarget = StageTarget {"vs_5_0", "vs_6_0"};
constexpr auto pixelTarget = StageTarget {"ps_5_0", "ps_6_0"};
constexpr auto computeTarget = StageTarget {"cs_5_0", "cs_6_0"};

winrt::com_ptr<ID3DBlob> compileWithFxc(const std::string& source,
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

winrt::com_ptr<ID3DBlob> compileStage(const DxcCompiler& dxc,
                                      const std::string& source,
                                      const std::string& entry,
                                      const StageTarget& target)
{
    if (dxc != nullptr)
        return Dxc::Detail::compile(*dxc, source, entry, target.dxc);

    return compileWithFxc(source, entry, target.fxc);
}

// "DXBC", a 16-byte checksum, a version, then the container's own total size,
// which has to be the size it was handed. D3D12 only looks at the bytes when a
// pipeline is created, so this is what catches a damaged one in time to fall
// back to the source.
bool isDxbcContainer(const ResEmbed::DataView& bytes)
{
    constexpr auto headerSize = std::size_t {32};
    constexpr auto totalSizeOffset = std::size_t {24};

    if (bytes.size() < headerSize || std::memcmp(bytes.data(), "DXBC", 4) != 0)
        return false;

    auto totalSize = std::uint32_t {};
    std::memcpy(&totalSize, bytes.data() + totalSizeOffset, sizeof(totalSize));

    return totalSize == bytes.size();
}

winrt::com_ptr<ID3DBlob> copyToBlob(const ResEmbed::DataView& bytes)
{
    winrt::com_ptr<ID3DBlob> blob;

    if (FAILED(D3DCreateBlob(bytes.size(), blob.put())))
        return nullptr;

    std::memcpy(blob->GetBufferPointer(), bytes.data(), bytes.size());
    return blob;
}

using PrecompiledStage = std::pair<std::string_view, winrt::com_ptr<ID3DBlob>*>;

Vector<PrecompiledStage> precompiledStages(const ShaderSource& source,
                                           D3D12ShaderProgram& into)
{
    if (source.isCompute())
        return {{Precompiled::computeDxbcSuffix, &into.computeBytecode}};

    auto stages = Vector<PrecompiledStage> {
        {Precompiled::vertexDxbcSuffix, &into.vertexBytecode}};

    if (source.hasFragment())
        stages.add({Precompiled::pixelDxbcSuffix, &into.pixelBytecode});

    return stages;
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

        if (!loadPrecompiledOrNeedSource(source))
            return;

        const auto dxc = Dxc::Detail::compilerFor(
            static_cast<ID3D12Device*>(device.nativeDevice()));

        if (source.isCompute())
        {
            program.computeBytecode =
                compileStage(dxc, source.source, source.computeEntry, computeTarget);
        }
        else
        {
            program.vertexBytecode =
                compileStage(dxc, source.source, source.vertexEntry, vertexTarget);

            if (source.hasFragment())
                program.pixelBytecode = compileStage(
                    dxc, source.source, source.fragmentEntry, pixelTarget);
        }
    }

    // Loads every stage's precompiled bytecode when all of them are there, and
    // says whether the source still has to be compiled: not once they have
    // loaded, and not under the require flag, which leaves the library invalid
    // instead. A render library needs its vertex and pixel halves together.
    bool loadPrecompiledOrNeedSource(const ShaderSource& source)
    {
        const auto key = Precompiled::keyFor(source);
        auto loaded = D3D12ShaderProgram {};

        for (auto [suffix, bytecode]: precompiledStages(source, loaded))
        {
            const auto name = Precompiled::resourceName(key, suffix);
            const auto bytes = Precompiled::find(name);

            if (!bytes)
                return Precompiled::fallBackAfterMiss(name);

            if (!isDxbcContainer(bytes))
                return Precompiled::fallBackAfterLoadFailure(
                    name, "it is not a DXBC container");

            *bytecode = copyToBlob(bytes);

            if (*bytecode == nullptr)
                return Precompiled::fallBackAfterLoadFailure(name,
                                                             "D3DCreateBlob failed");
        }

        program = std::move(loaded);
        precompiled = true;
        return false;
    }

    D3D12ShaderProgram program;
    bool precompiled = false;
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
           && (!hasFragment() || impl->program.pixelBytecode != nullptr);
}

bool ShaderLibrary::isPrecompiled() const
{
    return impl->precompiled;
}

void* ShaderLibrary::nativeLibrary() const
{
    return const_cast<D3D12ShaderProgram*>(&impl->program);
}
} // namespace eacp::GPU
