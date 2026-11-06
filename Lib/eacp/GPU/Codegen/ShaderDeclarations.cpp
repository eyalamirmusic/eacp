#include "ShaderDeclarations.h"

#include "../Frame/RenderPass.h"
#include "ShaderGraphAnalysis.h"
#include "UniformLayout.h"

#include <eacp/Core/Utils/Format.h>

#include <array>
#include <span>

namespace eacp::GPU::ShaderEmission
{
namespace
{
// How a buffer slot spells itself: the access decides the qualifier, the
// element type what is qualified. An atomic slot answers to neither - its
// elements are the type each language requires an interlocked operation to act
// through.
struct BufferTypeSpelling
{
    BufferAccess access;
    const char* metalFloat;
    const char* metalUInt;
    const char* hlslFloat;
    const char* hlslUInt;
};

constexpr BufferTypeSpelling bufferTypeSpellings[] = {
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

    return bufferTypeSpellings[0];
}

// What a sampled texture slot is declared as, which is the whole of what a cube
// changes: every declaration goes through these, so no two can disagree.
//
// HLSL's depth forms are `Texture2D<float>` rather than the bare `Texture2D` a
// colour slot gets: the SRV over the depth resource is a single-channel
// R32_FLOAT view (see depthShaderResourceFormat), and the typed declaration is
// what makes Sample return the one float MSL's depth2d does.
//
// GLSL is the exception: its sampler2D over a depth image returns four
// channels, so ExprKind::Sample takes the .r there.
struct TextureTypeSpelling
{
    TextureKind kind;
    const char* metal;
    const char* hlsl;
};

constexpr TextureTypeSpelling plainTextureSpelling = {
    TextureKind::Texture2D, "texture2d<float>", "Texture2D"};

constexpr TextureTypeSpelling textureTypeSpellings[] = {
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

    return plainTextureSpelling;
}

// A GLSL storage block is read-write by default; a read one says so.
const char* getGlslBufferQualifier(BufferAccess access)
{
    return access == BufferAccess::Read ? "readonly " : "";
}

// The element type on the same terms metalBufferType and hlslBufferType take
// it: an atomic slot is unsigned integers whatever it was declared with, since
// atomicAdd acts through nothing else.
const char* getGlslBufferElement(BufferAccess access, ValueType elementType)
{
    if (access == BufferAccess::Atomic || elementType == ValueType::UInt)
        return "uint";

    return "float";
}

// A combined image sampler: the sampler travels in the descriptor write, so the
// sampling configuration never reaches the source - except a comparison, which
// GLSL spells in the type and which the bound VkSampler must then enable.
const char* getGlslTextureType(TextureKind kind, const TextureSampling& sampling)
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

// Where the CPU block and the dialect's packing rules disagree, the pad scalars
// that move the next member to where the CPU wrote it.
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

// The instance name is what keeps uniforms.uN the one spelling in all three.
std::string formatUniformBlockClosing(Backend backend)
{
    if (backend == Backend::Vulkan)
        return "} uniforms;\n\n";

    auto source = std::string {"};\n\n"};

    if (backend == Backend::DirectX)
        source += "cbuffer UniformsCB : register(b0)\n{\n"
                  "    Uniforms uniforms;\n};\n\n";

    return source;
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

const char* getHlslSamplerStateType(int configuration)
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

// The MSL parameters for the storage buffers a render stage reads, always read
// only: a vertex or fragment function has no writable buffer here, which is the
// whole of what separates this from the kernel signature.
std::string formatBufferParameters(const ShaderGraph& graph,
                                   const Vector<int>& roots)
{
    auto used = findBufferSlotsUsedBy(graph, roots);
    auto source = std::string {};

    for (auto i = 0; i < used.size(); ++i)
        if (used[i] != 0)
            source += Strings::format(
                ",\n    {} buffer{} [[buffer({})]]",
                getMetalBufferType(BufferAccess::Read, graph.storageElementType(i)),
                i,
                RenderPass::bufferBase + i);

    return source;
}

// The Uniforms struct shared by both stages (and the HLSL cbuffer wrapping it).
// The CPU block is packed with MSL struct alignment (UniformLayout.h); HLSL
// cbuffer packing only forbids straddling a 16-byte register, so a vector after
// a scalar would land lower than the CPU wrote it - explicit pad scalars are
// emitted wherever the two rule sets disagree.
//
// std140 disagrees the other way: a vec3 is twelve bytes there, so a scalar
// after one needs a pad too.
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

    return source + formatUniformBlockClosing(backend);
}

// The instance name is left off so the run of elements is a global named
// buffer<slot>, which prints byte-identically to the other two dialects.
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

// imageStore is all a kernel does with one, so the image needs no format layout
// qualifier - which keeps it agnostic of the format it was created in.
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

// The SamplerState globals an HLSL stage needs: one per sampling configuration
// any of its readable textures asked for, at the register the root signature
// put that configuration's static sampler on.
//
// Declaring them per configuration rather than per texture is what stops the
// sampler registers from capping maxTextureSlots. HLSL has 16 of them
// (s0..s15), so one sampler per (slot, configuration) pair - which is what this
// emitted until a shader needed a fifth texture - ran out at four slots. Metal
// never had the question, because MSL passes a sampler as a function argument
// rather than binding it to a register, so the Metal path still declares one
// per texture.
//
// A write-access texture is skipped for the same reason it is declared as an
// RWTexture2D: there is nothing to sample it with.
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

// The early return the rounded-up dispatch needs, over as many extents as the
// rank has.
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
