#include <eacp/Core/Utils/WinInclude.h>

#include "ShaderLibrary.h"

#include "../Device/Device.h"
#include "../Windows/D3D12Types.h"
#include "DxcCompiler-Windows.h"
#include "PrecompiledShaders.h"
#include "ShaderSource.h"

#include <eacp/Core/Utils/Containers.h>
#include <eacp/Core/Utils/Format.h>

#include <d3dcompiler.h>

#include <winrt/base.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
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
    ShaderStage stage;
};

constexpr auto vertexTarget = StageTarget {"vs_5_0", "vs_6_0", ShaderStage::Vertex};
constexpr auto pixelTarget = StageTarget {"ps_5_0", "ps_6_0", ShaderStage::Fragment};
constexpr auto computeTarget =
    StageTarget {"cs_5_0", "cs_6_0", ShaderStage::Compute};

std::string fxcLogOf(ID3DBlob* errors)
{
    if (errors == nullptr || errors->GetBufferSize() == 0)
        return {};

    const auto* text = static_cast<const char*>(errors->GetBufferPointer());
    auto length = errors->GetBufferSize();

    while (length > 0 && text[length - 1] == '\0')
        --length;

    return {text, length};
}

winrt::com_ptr<ID3DBlob> compileWithFxc(const std::string& source,
                                        const std::string& entry,
                                        const StageTarget& target,
                                        const Vector<ShaderDefine>& defines,
                                        CompilationInfo& messages)
{
    winrt::com_ptr<ID3DBlob> code;
    winrt::com_ptr<ID3DBlob> errors;

    auto macros = Vector<D3D_SHADER_MACRO> {};

    for (const auto& define: defines)
        macros.add({define.name.c_str(), define.value.c_str()});

    macros.add({nullptr, nullptr});

    auto hr = D3DCompile(source.data(),
                         source.size(),
                         nullptr,
                         macros.data(),
                         nullptr,
                         entry.c_str(),
                         target.fxc,
                         D3DCOMPILE_ENABLE_STRICTNESS,
                         0,
                         code.put(),
                         errors.put());

    const auto log = fxcLogOf(errors.get());
    messages.append(parseFxcLog({log, target.stage, FAILED(hr)}));

    if (FAILED(hr))
    {
        if (!log.empty())
            LOG(log);

        if (!messages.hasErrors())
            messages.add(CompilationMessageType::Error,
                         Strings::format("D3DCompile failed (0x{:08X})",
                                         static_cast<unsigned>(hr)),
                         target.stage);

        return nullptr;
    }

    return code;
}

winrt::com_ptr<ID3DBlob> compileStage(const DxcCompiler& dxc,
                                      const std::string& source,
                                      const std::string& entry,
                                      const StageTarget& target,
                                      const Vector<ShaderDefine>& defines,
                                      CompilationInfo& messages)
{
    if (dxc != nullptr)
        return Dxc::Detail::compile(
            *dxc, source, entry, target.dxc, target.stage, defines, messages);

    return compileWithFxc(source, entry, target, defines, messages);
}

// A library's source compiled once more for one set of pipeline constants,
// and what compiling it said.
struct D3D12ProgramVariant
{
    PipelineConstants constants;
    D3D12ShaderProgram program;
    CompilationInfo compilation;
};

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
    Native(Device& device, const ShaderSource& source, CompilationInfo& messages)
    {
        if (!device.isValid())
        {
            messages.add(CompilationMessageType::Error,
                         "there is no D3D12 device to compile the library on");
            return;
        }

        // An empty source is a build something declined to make, and whatever
        // declined it has already said why - see ComputeProgram::prepare. There
        // is nothing here to compile and nothing for FXC to log.
        if (source.source.empty())
        {
            messages.add(CompilationMessageType::Error,
                         "the source is empty: what built it declined to");
            return;
        }

        d3dDevice = static_cast<ID3D12Device*>(device.nativeDevice());

        if (source.hasPipelineConstants)
            specializable = source;

        if (!loadPrecompiledOrNeedSource(source, messages))
            return;

        program = compileProgram(source, {}, messages);
    }

    D3D12ShaderProgram compileProgram(const ShaderSource& source,
                                      const Vector<ShaderDefine>& defines,
                                      CompilationInfo& messages) const
    {
        const auto dxc = Dxc::Detail::compilerFor(d3dDevice);
        auto compiled = D3D12ShaderProgram {};

        if (source.isCompute())
        {
            compiled.computeBytecode = compileStage(dxc,
                                                    source.source,
                                                    source.computeEntry,
                                                    computeTarget,
                                                    defines,
                                                    messages);
            return compiled;
        }

        compiled.vertexBytecode = compileStage(
            dxc, source.source, source.vertexEntry, vertexTarget, defines, messages);

        if (source.hasFragment())
            compiled.pixelBytecode = compileStage(dxc,
                                                  source.source,
                                                  source.fragmentEntry,
                                                  pixelTarget,
                                                  defines,
                                                  messages);

        return compiled;
    }

    // FXC has no specialisation: each set of constants is the source compiled
    // again with their macros defined, once, and kept with what compiling it
    // said. The empty set is the library as compiled - or as precompiled, the
    // only set a blob covers - and has no variant.
    const D3D12ProgramVariant* variantFor(const PipelineConstants& constants) const
    {
        if (constants.isEmpty() || !specializable.has_value())
            return nullptr;

        auto lock = std::lock_guard {variantsLock};

        for (const auto& variant: variants)
            if (variant->constants == constants)
                return variant.get();

        auto& variant = variants.createNew();
        variant.constants = constants;
        variant.program = compileProgram(
            *specializable, hlslDefinesFor(constants), variant.compilation);
        return &variant;
    }

    const D3D12ShaderProgram* programFor(const PipelineConstants& constants) const
    {
        if (const auto* variant = variantFor(constants))
            return &variant->program;

        return &program;
    }

    // Loads every stage's precompiled bytecode when all of them are there, and
    // says whether the source still has to be compiled: not once they have
    // loaded, and not under the require flag, which leaves the library invalid
    // instead. A render library needs its vertex and pixel halves together.
    bool loadPrecompiledOrNeedSource(const ShaderSource& source,
                                     CompilationInfo& messages)
    {
        const auto key = Precompiled::keyFor(source);
        auto loaded = D3D12ShaderProgram {};

        for (auto [suffix, bytecode]: precompiledStages(source, loaded))
        {
            const auto name = Precompiled::resourceName(key, suffix);
            const auto bytes = Precompiled::find(name);

            if (!bytes)
                return Precompiled::fallBackAfterMiss(name, messages);

            if (!isDxbcContainer(bytes))
                return Precompiled::fallBackAfterLoadFailure(
                    name, "it is not a DXBC container", messages);

            *bytecode = copyToBlob(bytes);

            if (*bytecode == nullptr)
                return Precompiled::fallBackAfterLoadFailure(
                    name, "D3DCreateBlob failed", messages);
        }

        program = std::move(loaded);
        precompiled = true;
        return false;
    }

    D3D12ShaderProgram program;
    bool precompiled = false;

    ID3D12Device* d3dDevice = nullptr;
    std::optional<ShaderSource> specializable;
    mutable std::mutex variantsLock;
    mutable OwnedVector<D3D12ProgramVariant> variants;
};

ShaderLibrary::ShaderLibrary(Device& device, const ShaderSource& source)
    : vertexEntryName(source.vertexEntry)
    , fragmentEntryName(source.fragmentEntry)
    , computeEntryName(source.computeEntry)
    , groupShape(source.threadGroup)
    , pipelineConstants(source.hasPipelineConstants)
    , impl(device, source, compilation)
{
}

void* ShaderLibrary::nativeLibrary(const PipelineConstants& constants) const
{
    return const_cast<D3D12ShaderProgram*>(impl->programFor(constants));
}

CompilationInfo
    ShaderLibrary::compilationInfo(const PipelineConstants& constants) const
{
    if (const auto* variant = impl->variantFor(constants))
        return variant->compilation;

    return compilation;
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
