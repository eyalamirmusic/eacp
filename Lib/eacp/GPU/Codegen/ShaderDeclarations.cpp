#include "ShaderDeclarations.h"

#include "UniformLayout.h"

#include <eacp/Core/Utils/Format.h>

#include <array>
#include <cassert>
#include <span>

namespace eacp::GPU::ShaderEmission
{
namespace
{
struct BufferTypeSpelling
{
    BufferAccess access;
    std::string_view metalFloat;
    std::string_view metalUInt;
    std::string_view hlslFloat;
    std::string_view hlslUInt;
};

constexpr auto bufferTypeSpellings = Array<BufferTypeSpelling, 3> {
    {BufferAccess::Read,
     "device const float*",
     "device const uint*",
     "StructuredBuffer<float>",
     "StructuredBuffer<uint>"},
    {BufferAccess::Write,
     "device float*",
     "device uint*",
     "RWStructuredBuffer<float>",
     "RWStructuredBuffer<uint>"},
    {BufferAccess::Atomic,
     "device atomic_uint*",
     "device atomic_uint*",
     "RWStructuredBuffer<uint>",
     "RWStructuredBuffer<uint>"},
};

const BufferTypeSpelling& findBufferTypeSpelling(BufferAccess access)
{
    for (const auto& spelling: bufferTypeSpellings)
        if (spelling.access == access)
            return spelling;

    assert(false && "eacp: no spelling for this buffer access");
    return bufferTypeSpellings[0];
}

// HLSL's depth forms are typed so Sample returns one float, as MSL's depth2d.
struct TextureTypeSpelling
{
    TextureKind kind;
    std::string_view metal;
    std::string_view hlsl;
};

constexpr auto textureTypeSpellings = Array<TextureTypeSpelling, 5> {
    {TextureKind::Texture2D, "texture2d<float>", "Texture2D"},
    {TextureKind::Cube, "texturecube<float>", "TextureCube"},
    {TextureKind::Depth2D, "depth2d<float>", "Texture2D<float>"},
    {TextureKind::Texture2DArray, "texture2d_array<float>", "Texture2DArray"},
    {TextureKind::Depth2DArray, "depth2d_array<float>", "Texture2DArray<float>"},
};

const TextureTypeSpelling& findTextureTypeSpelling(TextureKind kind)
{
    for (const auto& spelling: textureTypeSpellings)
        if (spelling.kind == kind)
            return spelling;

    assert(false && "eacp: no spelling for this texture kind");
    return textureTypeSpellings[0];
}

std::string_view getGlslBufferQualifier(BufferAccess access)
{
    return access == BufferAccess::Read ? "readonly " : "";
}

std::string_view getGlslBufferElement(BufferAccess access, ValueType elementType)
{
    if (access == BufferAccess::Atomic || elementType == ValueType::UInt)
        return "uint";

    return "float";
}

// The sampler travels in the descriptor write; only a comparison is spelled in
// the type, and the bound VkSampler must then enable it.
std::string_view getGlslTextureType(TextureKind kind,
                                    const TextureSampling& sampling)
{
    const auto compare = sampling.compare != TextureCompare::None;

    switch (kind)
    {
        case TextureKind::Cube:
            return "samplerCube";
        case TextureKind::Texture2DArray:
            return "sampler2DArray";
        case TextureKind::Depth2DArray:
            return compare ? "sampler2DArrayShadow" : "sampler2DArray";
        case TextureKind::Depth2D:
            return compare ? "sampler2DShadow" : "sampler2D";
        default:
            return "sampler2D";
    }
}

constexpr auto uniformPadBytes = 4;

struct UniformPadding
{
    std::string formatPaddingTo(int offset, ValueType type)
    {
        auto source = std::string {};

        while (getPackedOffset(cursor, type) < offset)
        {
            source += Strings::format("    float pad{};\n", padCount++);
            cursor += uniformPadBytes;
        }

        cursor = offset + byteSize(type);
        return source;
    }

    int getPackedOffset(int at, ValueType type) const
    {
        return glsl ? std140PackedOffset(at, type) : hlslPackedOffset(at, type);
    }

    bool glsl = false;
    int cursor = 0;
    int padCount = 0;
};

std::string formatUniformBlockOpening(Backend backend, int binding)
{
    if (backend == Backend::Vulkan)
        return Strings::format(
            "layout(std140, set = 0, binding = {}) uniform Uniforms\n{{\n", binding);

    return "struct Uniforms\n{\n";
}

std::string_view formatUniformBlockClosing(Backend backend)
{
    if (backend == Backend::Vulkan)
        return "} uniforms;\n\n";

    if (backend == Backend::DirectX)
        return "};\n\ncbuffer UniformsCB : register(b0)\n{\n"
               "    Uniforms uniforms;\n};\n\n";

    return "};\n\n";
}

std::array<bool, samplingConfigurations>
    findSampledConfigurations(const ShaderGraph& graph)
{
    auto used = std::array<bool, samplingConfigurations> {};

    for (auto i = 0; i < graph.textureCount(); ++i)
        if (graph.textureAccess(i) != TextureAccess::Write)
            used[samplingIndex(graph.textureSampling(i))] = true;

    return used;
}

std::string_view getHlslSamplerStateType(int configuration)
{
    const auto compares =
        samplingConfiguration(configuration).compare != TextureCompare::None;

    return compares ? "SamplerComparisonState" : "SamplerState";
}
} // namespace

std::string_view getMetalBufferType(BufferAccess access, ValueType elementType)
{
    const auto& spelling = findBufferTypeSpelling(access);
    return elementType == ValueType::UInt ? spelling.metalUInt : spelling.metalFloat;
}

std::string_view getHlslBufferType(BufferAccess access, ValueType elementType)
{
    const auto& spelling = findBufferTypeSpelling(access);
    return elementType == ValueType::UInt ? spelling.hlslUInt : spelling.hlslFloat;
}

// The CPU block uses MSL alignment (UniformLayout.h). HLSL cbuffer and std140
// packing disagree with it in places, so pad scalars are emitted there.
std::string formatUniformBlock(Backend backend,
                               const Vector<ValueType>& types,
                               const Vector<std::string>& names,
                               int binding)
{
    auto source = formatUniformBlockOpening(backend, binding);
    auto offsets = uniformOffsets(types);
    auto padding = UniformPadding {backend == Backend::Vulkan};

    for (auto i = 0; i < types.size(); ++i)
    {
        auto type = types[i];

        if (backend != Backend::Metal)
            source += padding.formatPaddingTo(offsets[i], type);

        source += Strings::format(
            "    {} {};\n", getDialectTypeName(backend, type), names[i]);
    }

    source += formatUniformBlockClosing(backend);
    return source;
}

// No instance name, so the elements print as buffer<slot> as in the others.
std::string formatGlslBufferBlock(BufferAccess access,
                                  ValueType elementType,
                                  int slot,
                                  int binding)
{
    return Strings::format(
        "layout(std430, set = 0, binding = {}) {}buffer Buffer{}\n{{\n    {} "
        "buffer{}[];\n}};\n",
        binding,
        getGlslBufferQualifier(access),
        slot,
        getGlslBufferElement(access, elementType),
        slot);
}

std::string_view getMetalTextureType(TextureKind kind)
{
    return findTextureTypeSpelling(kind).metal;
}

std::string_view getHlslTextureType(TextureKind kind)
{
    return findTextureTypeSpelling(kind).hlsl;
}

std::string formatGlslWritableTexture(int slot, int binding)
{
    return Strings::format(
        "layout(set = 0, binding = {}) uniform writeonly image2D texture{};\n",
        binding,
        slot);
}

std::string formatGlslSampledTexture(const ShaderGraph& graph, int slot, int binding)
{
    return Strings::format(
        "layout(set = 0, binding = {}) uniform {} texture{};\n",
        binding,
        getGlslTextureType(graph.textureKind(slot), graph.textureSampling(slot)),
        slot);
}

// One sampler per sampling configuration rather than per texture: HLSL has
// only sixteen sampler registers.
std::string formatHlslSamplerDeclarations(const ShaderGraph& graph)
{
    auto used = findSampledConfigurations(graph);
    auto source = std::string {};

    for (auto configuration = 0; configuration < samplingConfigurations;
         ++configuration)
        if (used[configuration])
            source += Strings::format("{} samplerConfig{} : register(s{});\n",
                                      getHlslSamplerStateType(configuration),
                                      configuration,
                                      configuration);

    return source;
}

std::string formatBoundsGuard(DispatchRank rank)
{
    if (rank == DispatchRank::OneD)
        return "    if (gid >= uniforms.count)\n        return;\n";

    auto condition = std::string {};

    for (auto i = 0; i < getGridExtentCount(rank); ++i)
    {
        if (i > 0)
            condition += " || ";

        condition += Strings::format(
            "gid{} >= uniforms.{}", getComponentSuffix(i), getGridExtentName(i));
    }

    return Strings::format("    if ({})\n        return;\n", condition);
}
} // namespace eacp::GPU::ShaderEmission
