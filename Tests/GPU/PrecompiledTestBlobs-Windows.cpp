#include "PrecompiledTestBlobs.h"

#include <eacp/Core/Utils/WinInclude.h>
#include <eacp/GPU/Shader/PrecompiledNames.h>

#include <d3dcompiler.h>

#include <winrt/base.h>

namespace eacp::GPU::PrecompiledTestBlobs
{
namespace
{
Vector<std::uint8_t> compileHlslStage(const ShaderSource& source,
                                      const std::string& entry,
                                      const char* target)
{
    winrt::com_ptr<ID3DBlob> code;
    winrt::com_ptr<ID3DBlob> errors;

    auto hr = D3DCompile(source.source.data(),
                         source.source.size(),
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
        return {};

    return bytesOf(code->GetBufferPointer(), code->GetBufferSize());
}
} // namespace

Vector<std::string> resourceNamesFor(const ShaderSource& source)
{
    const auto key = Precompiled::keyFor(source);

    if (source.isCompute())
        return {Precompiled::resourceName(key, Precompiled::computeDxbcSuffix)};

    auto names = Vector<std::string> {
        Precompiled::resourceName(key, Precompiled::vertexDxbcSuffix)};

    if (source.hasFragment())
        names.add(Precompiled::resourceName(key, Precompiled::pixelDxbcSuffix));

    return names;
}

Resources compileAheadOfTime(const ShaderSource& source)
{
    const auto key = Precompiled::keyFor(source);
    auto resources = Resources {};

    auto add =
        [&](std::string_view suffix, const std::string& entry, const char* target)
    {
        resources.add({Precompiled::resourceName(key, suffix),
                       compileHlslStage(source, entry, target)});
    };

    if (source.isCompute())
        add(Precompiled::computeDxbcSuffix, source.computeEntry, "cs_5_0");
    else
    {
        add(Precompiled::vertexDxbcSuffix, source.vertexEntry, "vs_5_0");

        if (source.hasFragment())
            add(Precompiled::pixelDxbcSuffix, source.fragmentEntry, "ps_5_0");
    }

    for (const auto& resource: resources)
        if (resource.bytes.empty())
            return {};

    return resources;
}
} // namespace eacp::GPU::PrecompiledTestBlobs
