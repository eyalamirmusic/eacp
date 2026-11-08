#include "ShaderDeclarations.h"

#include "ShaderStructEmission.h"
#include "UniformLayout.h"

#include <eacp/Core/Utils/Format.h>

#include <array>
#include <cassert>
#include <span>

namespace eacp::GPU::ShaderEmission
{
namespace
{
// HLSL holds an f16 buffer as its words, two halves to each.
struct BufferTypeSpelling
{
    BufferAccess access;
    std::string_view metalFloat;
    std::string_view metalUInt;
    std::string_view metalInt;
    std::string_view metalHalf;
    std::string_view hlslFloat;
    std::string_view hlslUInt;
    std::string_view hlslInt;
    std::string_view hlslHalf;
};

constexpr auto bufferTypeSpellings = Array<BufferTypeSpelling, 3> {
    {BufferAccess::Read,
     "device const float*",
     "device const uint*",
     "device const int*",
     "device const half*",
     "StructuredBuffer<float>",
     "StructuredBuffer<uint>",
     "StructuredBuffer<int>",
     "StructuredBuffer<uint>"},
    {BufferAccess::Write,
     "device float*",
     "device uint*",
     "device int*",
     "device half*",
     "RWStructuredBuffer<float>",
     "RWStructuredBuffer<uint>",
     "RWStructuredBuffer<int>",
     "RWStructuredBuffer<uint>"},
    {BufferAccess::Atomic,
     "device atomic_uint*",
     "device atomic_uint*",
     "device atomic_int*",
     "device atomic_uint*",
     "RWStructuredBuffer<uint>",
     "RWStructuredBuffer<uint>",
     "RWStructuredBuffer<int>",
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

constexpr auto textureTypeSpellings = Array<TextureTypeSpelling, 9> {
    {TextureKind::Texture2D, "texture2d<float>", "Texture2D"},
    {TextureKind::Cube, "texturecube<float>", "TextureCube"},
    {TextureKind::Depth2D, "depth2d<float>", "Texture2D<float>"},
    {TextureKind::Texture2DArray, "texture2d_array<float>", "Texture2DArray"},
    {TextureKind::Depth2DArray, "depth2d_array<float>", "Texture2DArray<float>"},
    {TextureKind::Texture3D, "texture3d<float>", "Texture3D"},
    {TextureKind::CubeArray, "texturecube_array<float>", "TextureCubeArray"},
    {TextureKind::DepthCube, "depthcube<float>", "TextureCube<float>"},
    {TextureKind::DepthCubeArray,
     "depthcube_array<float>",
     "TextureCubeArray<float>"},
};

const TextureTypeSpelling& findTextureTypeSpelling(TextureKind kind)
{
    for (const auto& spelling: textureTypeSpellings)
        if (spelling.kind == kind)
            return spelling;

    assert(false && "eacp: no spelling for this texture kind");
    return textureTypeSpellings[0];
}

// A storage format's GLSL layout qualifier and how many channels it holds,
// which is the width HLSL declares a typed UAV at.
struct StorageFormatSpelling
{
    TextureFormat format;
    std::string_view glsl;
    int channels;
};

constexpr auto storageFormatSpellings = Array<StorageFormatSpelling, 32> {{
    {TextureFormat::RGBA8Unorm, "rgba8", 4},
    {TextureFormat::R8Unorm, "r8", 1},
    {TextureFormat::RG8Unorm, "rg8", 2},
    {TextureFormat::RGBA16Float, "rgba16f", 4},
    {TextureFormat::RGBA32Float, "rgba32f", 4},
    {TextureFormat::R32Float, "r32f", 1},
    {TextureFormat::R16Float, "r16f", 1},
    {TextureFormat::RG16Float, "rg16f", 2},
    {TextureFormat::RG32Float, "rg32f", 2},
    {TextureFormat::R16Unorm, "r16", 1},
    {TextureFormat::R32Uint, "r32ui", 1},
    {TextureFormat::RG16Unorm, "rg16", 2},
    {TextureFormat::RGBA16Unorm, "rgba16", 4},
    {TextureFormat::RGB10A2Unorm, "rgb10_a2", 4},
    {TextureFormat::RG11B10Float, "r11f_g11f_b10f", 3},
    {TextureFormat::R8Uint, "r8ui", 1},
    {TextureFormat::RG8Uint, "rg8ui", 2},
    {TextureFormat::RGBA8Uint, "rgba8ui", 4},
    {TextureFormat::R16Uint, "r16ui", 1},
    {TextureFormat::RG16Uint, "rg16ui", 2},
    {TextureFormat::RGBA16Uint, "rgba16ui", 4},
    {TextureFormat::RG32Uint, "rg32ui", 2},
    {TextureFormat::RGBA32Uint, "rgba32ui", 4},
    {TextureFormat::R8Sint, "r8i", 1},
    {TextureFormat::RG8Sint, "rg8i", 2},
    {TextureFormat::RGBA8Sint, "rgba8i", 4},
    {TextureFormat::R16Sint, "r16i", 1},
    {TextureFormat::RG16Sint, "rg16i", 2},
    {TextureFormat::RGBA16Sint, "rgba16i", 4},
    {TextureFormat::R32Sint, "r32i", 1},
    {TextureFormat::RG32Sint, "rg32i", 2},
    {TextureFormat::RGBA32Sint, "rgba32i", 4},
}};

const StorageFormatSpelling& findStorageFormatSpelling(TextureFormat format)
{
    for (const auto& spelling: storageFormatSpellings)
        if (spelling.format == format)
            return spelling;

    assert(false && "eacp: no storage spelling for this format");
    return storageFormatSpellings[0];
}

std::string_view getStorageScalarName(TextureFormat format)
{
    switch (storageTexelType(format))
    {
        case ValueType::UInt4:
            return "uint";
        case ValueType::Int4:
            return "int";
        default:
            return "float";
    }
}

std::string_view getGlslTypePrefix(TextureFormat format)
{
    switch (storageTexelType(format))
    {
        case ValueType::UInt4:
            return "u";
        case ValueType::Int4:
            return "i";
        default:
            return "";
    }
}

std::string_view getStorageDimensionName(TextureKind kind)
{
    if (kind == TextureKind::Texture2DArray)
        return "2DArray";

    return kind == TextureKind::Texture3D ? "3D" : "2D";
}

std::string_view getMetalStorageKindName(TextureKind kind)
{
    if (kind == TextureKind::Texture2DArray)
        return "texture2d_array";

    return kind == TextureKind::Texture3D ? "texture3d" : "texture2d";
}

std::string_view getMetalAccessName(TextureAccess access)
{
    if (access == TextureAccess::Read)
        return "read";

    return access == TextureAccess::ReadWrite ? "read_write" : "write";
}

std::string getHlslStorageTypeName(TextureKind kind,
                                   TextureFormat format,
                                   TextureAccess access)
{
    const auto read = access == TextureAccess::Read;

    return Strings::format("{}Texture{}<{}>",
                           read ? "" : "RW",
                           getStorageDimensionName(kind),
                           read ? getHlslReadTexelType(format)
                                : getHlslStorageElementType(format));
}

std::string formatGlslStorageTexture(const ShaderGraph& graph, int slot, int binding)
{
    const auto kind = graph.textureKind(slot);
    const auto format = graph.textureFormat(slot);
    const auto prefix = getGlslTypePrefix(format);
    const auto dimension = getStorageDimensionName(kind);

    switch (graph.textureAccess(slot))
    {
        case TextureAccess::Read:
            return Strings::format(
                "layout(set = 0, binding = {}) uniform {}sampler{} texture{};\n",
                binding,
                prefix,
                dimension,
                slot);

        case TextureAccess::ReadWrite:
            return Strings::format(
                "layout(set = 0, binding = {}, {}) uniform {}image{} texture{};\n",
                binding,
                getGlslImageFormat(format),
                prefix,
                dimension,
                slot);

        default:
            return Strings::format("layout(set = 0, binding = {}) uniform "
                                   "writeonly {}image{} texture{};\n",
                                   binding,
                                   prefix,
                                   dimension,
                                   slot);
    }
}

std::string_view getGlslBufferQualifier(BufferAccess access)
{
    return access == BufferAccess::Read ? "readonly " : "";
}

std::string_view getGlslBufferElement(BufferAccess access, ValueType elementType)
{
    if (elementType == ValueType::Int)
        return "int";

    if (access == BufferAccess::Atomic || elementType == ValueType::UInt)
        return "uint";

    if (elementType == ValueType::Half)
        return "float16_t";

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
        case TextureKind::Texture3D:
            return "sampler3D";
        case TextureKind::CubeArray:
            return "samplerCubeArray";
        case TextureKind::DepthCube:
            return compare ? "samplerCubeShadow" : "samplerCube";
        case TextureKind::DepthCubeArray:
            return compare ? "samplerCubeArrayShadow" : "samplerCubeArray";
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

        cursor = offset + uniformPackedSize(type, glsl);
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
        if (graph.textureAccess(i) == TextureAccess::Sample)
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

    if (elementType == ValueType::Int)
        return spelling.metalInt;

    if (elementType == ValueType::Half)
        return spelling.metalHalf;

    return elementType == ValueType::UInt ? spelling.metalUInt : spelling.metalFloat;
}

std::string_view getHlslBufferType(BufferAccess access, ValueType elementType)
{
    const auto& spelling = findBufferTypeSpelling(access);

    if (elementType == ValueType::Int)
        return spelling.hlslInt;

    if (elementType == ValueType::Half)
        return spelling.hlslHalf;

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

        if (backend == Backend::Metal && isMetalWidenedUniform(type))
        {
            source += Strings::format(
                "    float4 {}[{}];\n", names[i], matrixColumns(type));
            continue;
        }

        source += Strings::format(
            "    {} {};\n", getDialectTypeName(backend, type), names[i]);
    }

    source += formatUniformBlockClosing(backend);
    return source;
}

namespace
{
// No instance name, so the elements print as buffer<slot> as in the others.
std::string formatGlslBufferBlockOf(BufferAccess access,
                                    std::string_view element,
                                    int slot,
                                    int binding)
{
    return Strings::format(
        "layout(std430, set = 0, binding = {}) {}buffer Buffer{}\n{{\n    {} "
        "buffer{}[];\n}};\n",
        binding,
        getGlslBufferQualifier(access),
        slot,
        element,
        slot);
}
} // namespace

std::string formatGlslBufferBlock(BufferAccess access,
                                  ValueType elementType,
                                  int slot,
                                  int binding)
{
    return formatGlslBufferBlockOf(
        access, getGlslBufferElement(access, elementType), slot, binding);
}

std::string
    getMetalSlotBufferType(const ShaderGraph& graph, BufferAccess access, int slot)
{
    if (graph.storageStructType(slot) >= 0)
        return getMetalStructBufferType(graph, access, slot);

    return std::string(getMetalBufferType(access, graph.storageElementType(slot)));
}

std::string
    getHlslSlotBufferType(const ShaderGraph& graph, BufferAccess access, int slot)
{
    if (graph.storageStructType(slot) >= 0)
        return std::string(getHlslStructBufferType(access));

    return std::string(getHlslBufferType(access, graph.storageElementType(slot)));
}

std::string formatGlslSlotBufferBlock(const ShaderGraph& graph,
                                      BufferAccess access,
                                      int slot,
                                      int binding)
{
    auto structId = graph.storageStructType(slot);

    if (structId >= 0)
        return formatGlslBufferBlockOf(
            access, getStructName(graph, structId), slot, binding);

    return formatGlslBufferBlock(
        access, graph.storageElementType(slot), slot, binding);
}

std::string_view getMetalTextureType(TextureKind kind)
{
    return findTextureTypeSpelling(kind).metal;
}

std::string_view getHlslTextureType(TextureKind kind)
{
    return findTextureTypeSpelling(kind).hlsl;
}

std::string_view getGlslImageFormat(TextureFormat format)
{
    return findStorageFormatSpelling(format).glsl;
}

int getStorageChannelCount(TextureFormat format)
{
    return findStorageFormatSpelling(format).channels;
}

std::string getHlslStorageElementType(TextureFormat format)
{
    const auto channels = getStorageChannelCount(format);
    const auto scalar = getStorageScalarName(format);

    if (channels == 1)
        return std::string(scalar);

    return Strings::format("{}{}", scalar, channels);
}

std::string getHlslReadTexelType(TextureFormat format)
{
    return Strings::format("{}4", getStorageScalarName(format));
}

std::string getMetalTextureParameterType(const ShaderGraph& graph, int slot)
{
    const auto access = graph.textureAccess(slot);
    const auto kind = graph.textureKind(slot);

    if (!isStorageAccess(access))
        return std::string(getMetalTextureType(kind));

    return Strings::format("{}<{}, access::{}>",
                           getMetalStorageKindName(kind),
                           getStorageScalarName(graph.textureFormat(slot)),
                           getMetalAccessName(access));
}

std::string getHlslTextureDeclarationType(const ShaderGraph& graph, int slot)
{
    const auto access = graph.textureAccess(slot);
    const auto kind = graph.textureKind(slot);

    if (!isStorageAccess(access))
        return std::string(getHlslTextureType(kind));

    return getHlslStorageTypeName(kind, graph.textureFormat(slot), access);
}

char getHlslTextureRegisterSpace(const ShaderGraph& graph, int slot)
{
    const auto access = graph.textureAccess(slot);
    return access == TextureAccess::Write || access == TextureAccess::ReadWrite
               ? 'u'
               : 't';
}

std::string formatGlslTexture(const ShaderGraph& graph, int slot, int binding)
{
    if (isStorageAccess(graph.textureAccess(slot)))
        return formatGlslStorageTexture(graph, slot, binding);

    if (graph.textureAccess(slot) == TextureAccess::Sampler)
        return Strings::format(
            "layout(set = 0, binding = {}) uniform {} sampler{};\n",
            binding,
            graph.isComparisonSampler(slot) ? "samplerShadow" : "sampler",
            slot);

    const auto kind = graph.textureKind(slot);
    auto source =
        Strings::format("layout(set = 0, binding = {}) uniform {} texture{};\n",
                        binding,
                        getGlslTextureType(kind, graph.textureSampling(slot)),
                        slot);

    if (isSampledThroughSampler(graph, slot))
        source += Strings::format(
            "layout(set = 0, binding = {}) uniform {} textureImage{};\n",
            binding,
            getGlslImageType(kind),
            slot);

    return source;
}

std::string formatHlslSampler(const ShaderGraph& graph, int slot)
{
    return Strings::format("{} sampler{} : register(s{});\n",
                           graph.isComparisonSampler(slot) ? "SamplerComparisonState"
                                                           : "SamplerState",
                           slot,
                           RenderPass::samplerRegisterBase + slot);
}

bool isSampledThroughSampler(const ShaderGraph& graph, int slot)
{
    for (auto node = 0; node < graph.nodeCount(); ++node)
    {
        const auto& expr = graph.expr(node);

        if (expr.kind == ExprKind::TextureFunction && expr.index == slot
            && expr.sampler >= 0)
            return true;
    }

    return false;
}

std::string_view getGlslImageType(TextureKind kind)
{
    switch (kind)
    {
        case TextureKind::Cube:
        case TextureKind::DepthCube:
            return "textureCube";
        case TextureKind::Texture2DArray:
        case TextureKind::Depth2DArray:
            return "texture2DArray";
        case TextureKind::Texture3D:
            return "texture3D";
        case TextureKind::CubeArray:
        case TextureKind::DepthCubeArray:
            return "textureCubeArray";
        default:
            return "texture2D";
    }
}

std::string_view getGlslSampledType(TextureKind kind, bool comparison)
{
    auto sampling = TextureSampling {};
    sampling.compare = comparison ? TextureCompare::LessEqual : TextureCompare::None;

    return getGlslTextureType(kind, sampling);
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
