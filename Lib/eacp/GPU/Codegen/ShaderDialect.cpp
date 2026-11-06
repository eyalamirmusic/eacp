#include "ShaderDialect.h"

#include <eacp/Core/Utils/Format.h>

#include <cassert>
#include <span>
#include <string_view>

namespace eacp::GPU::ShaderEmission
{
const char* typeName(Backend backend, ValueType type)
{
    return backend == Backend::Vulkan ? glslTypeName(type) : typeName(type);
}

std::string floatLiteral(float value)
{
    auto text = Strings::shortestRoundTrip(value);

    if (text.find('.') == std::string::npos && text.find('e') == std::string::npos
        && text.find('n') == std::string::npos)
        text += ".0";

    return text;
}

// GLSL rejects a non-flat integer stage input outright, HLSL wants
// nointerpolation and MSL [[flat]]; a float varying takes none of the three.
bool isFlatVarying(ValueType type)
{
    return isSignedInteger(type) || isUnsignedInteger(type);
}

std::string attributeSemantic(Backend backend, int index)
{
    if (backend == Backend::Vulkan)
        return {};

    if (backend == Backend::Metal)
        return " [[attribute(" + std::to_string(index) + ")]]";

    return " : TEXCOORD" + std::to_string(index);
}

// Metal hangs the flat qualifier off the member rather than putting it in front
// of the type, so it rides here beside the semantic.
std::string varyingSemantic(Backend backend, int index, ValueType type)
{
    if (backend == Backend::Metal)
        return isFlatVarying(type) ? " [[flat]]" : std::string {};

    if (backend != Backend::DirectX)
        return {};

    return " : TEXCOORD" + std::to_string(index);
}

std::string positionSemantic(Backend backend)
{
    if (backend == Backend::Vulkan)
        return {};

    if (backend == Backend::Metal)
        return " [[position]]";

    return " : SV_Position";
}

std::string flatQualifier(Backend backend, ValueType type)
{
    if (backend == Backend::Metal || !isFlatVarying(type))
        return {};

    return backend == Backend::Vulkan ? "flat " : "nointerpolation ";
}

// Attribute i and varying i take location i, which is what the Vulkan
// vertex-input state and the vertex/fragment interface match on.
std::string locationLayout(int index)
{
    return "layout(location = " + std::to_string(index) + ") ";
}

// GLSL reads a stage's I/O out of globals, spelled attrN and varyN because aN
// and vN are taken there by a constant array and a mutable local.
std::string attributeName(Backend backend, int slot)
{
    if (backend == Backend::Vulkan)
        return "attr" + std::to_string(slot);

    return "input.a" + std::to_string(slot);
}

std::string varyingName(Backend backend, int index)
{
    if (backend == Backend::Vulkan)
        return "vary" + std::to_string(index);

    return "input.v" + std::to_string(index);
}

struct BuiltinRename
{
    std::string_view canonical;
    const char* renamed;
};

// One HLSL name per direction, whatever the width MSL named.
constexpr BuiltinRename hlslRenames[] = {
    {"fract", "frac"}, {"mix", "lerp"}, {"dfdx", "ddx"}, {"dfdy", "ddy"}};

constexpr BuiltinRename hlslPrefixRenames[] = {{"as_type<uint", "asuint"},
                                               {"as_type<float", "asfloat"}};

// GLSL has no log10; the helper table carries the definition.
constexpr BuiltinRename glslRenames[] = {{"atan2", "atan"},
                                         {"rsqrt", "inversesqrt"},
                                         {"dfdx", "dFdx"},
                                         {"dfdy", "dFdy"},
                                         {"log10", "eacpLog10"}};

// Both are genType in GLSL, so one name per direction covers every width MSL
// named.
constexpr BuiltinRename glslPrefixRenames[] = {{"as_type<uint", "floatBitsToUint"},
                                               {"as_type<float", "uintBitsToFloat"}};

const char* findExactRename(std::span<const BuiltinRename> renames,
                            const std::string& name)
{
    for (const auto& rename: renames)
        if (name == rename.canonical)
            return rename.renamed;

    return nullptr;
}

const char* findPrefixRename(std::span<const BuiltinRename> renames,
                             const std::string& name)
{
    for (const auto& rename: renames)
        if (name.starts_with(rename.canonical))
            return rename.renamed;

    return nullptr;
}

const char* findRename(std::span<const BuiltinRename> exact,
                       std::span<const BuiltinRename> prefixed,
                       const std::string& name)
{
    if (const auto* renamed = findExactRename(exact, name))
        return renamed;

    return findPrefixRename(prefixed, name);
}

// A constructor-style cast is recorded as a call under the target's canonical
// type name, so float2(v) leaves as vec2(v).
const char* findGlslRename(const std::string& name)
{
    if (const auto* spelling = glslTypeNameFor(name))
        return spelling;

    return findRename(glslRenames, glslPrefixRenames, name);
}

const char* findDialectRename(Backend backend, const std::string& name)
{
    if (backend == Backend::DirectX)
        return findRename(hlslRenames, hlslPrefixRenames, name);

    if (backend == Backend::Vulkan)
        return findGlslRename(name);

    return nullptr;
}

// Call nodes carry the canonical (MSL) builtin name; renamed here per dialect.
std::string callName(Backend backend, const std::string& name)
{
    if (const auto* renamed = findDialectRename(backend, name))
        return renamed;

    return name;
}

// The builtins GLSL overloads per width: it takes a scalar beside a vector only
// in a few trailing positions, where MSL converts and HLSL promotes it.
bool isGenTypeCall(const std::string& name)
{
    return name == "min" || name == "max" || name == "clamp" || name == "mix"
           || name == "step" || name == "smoothstep" || name == "pow"
           || name == "atan2";
}

constexpr BuiltinRename glslComparisonFunctions[] = {{"<", "lessThan"},
                                                     {"<=", "lessThanEqual"},
                                                     {">", "greaterThan"},
                                                     {">=", "greaterThanEqual"},
                                                     {"==", "equal"},
                                                     {"!=", "notEqual"}};

// GLSL reserves the relational and equality operators for scalars, so a
// componentwise mask is a function there. Asked only for a boolean vector.
const char* glslComparison(const std::string& op)
{
    return findExactRename(glslComparisonFunctions, op);
}

// The HLSL sampler a texture with this sampling reads through. Named for the
// configuration rather than for the texture, because that is what it is: the
// root signature declares samplingConfigurations static samplers and every
// texture sampled that way shares one. See TextureSampling.
std::string hlslSamplerName(const TextureSampling& sampling)
{
    return "samplerConfig" + std::to_string(samplingIndex(sampling));
}

// The component a 2D or 3D kernel's thread index node asked for, and the
// uniform its matching grid extent is declared under.
const char* componentSuffix(int component)
{
    if (component == 0)
        return ".x";

    return component == 1 ? ".y" : ".z";
}

// The lane of a vector value, which is a different question: a thread index
// reaches three components and a vector reaches four, so the one above stops
// where it does and this one names .w. Nothing in the EDSL is wider than four,
// so a fifth component is a caller's mistake rather than a lane to name.
const char* vectorComponentSuffix(int component)
{
    assert(component >= 0 && component < 4
           && "eacp: a vector has no component past .w");

    constexpr const char* lanes[] = {".x", ".y", ".z", ".w"};
    return lanes[component];
}

// The MSL type a run of consecutive buffer elements is loaded through.
//
// The packed one and not float4, because the alignment is not ours to promise.
// A plain float4 wants a sixteen-byte-aligned address, and eacp binds a
// BufferRange at any four-byte offset - Device::storageBufferOffsetAlignment
// answers four on this backend - so a range starting one float into a buffer
// would make every such load undefined. packed_float4 is the same sixteen bytes
// with an alignment of four, which is exactly what the binding guarantees, and
// it converts to float4 on the way out.
const char* metalPackedVectorType(ValueType type)
{
    auto integers = isUnsignedInteger(type);

    switch (componentCount(type))
    {
        case 2:
            return integers ? "packed_uint2" : "packed_float2";
        case 3:
            return integers ? "packed_uint3" : "packed_float3";
        default:
            return integers ? "packed_uint4" : "packed_float4";
    }
}

// A thread index under the name both kernel scaffoldings bind it to: the whole
// value where the node took the position as one, one lane otherwise.
std::string indexReference(const char* name, DispatchRank rank, int component)
{
    if (rank == DispatchRank::OneD || component == allComponents)
        return name;

    return name + std::string(componentSuffix(component));
}

const char* gridExtentName(int component)
{
    if (component == 0)
        return "width";

    return component == 1 ? "height" : "depth";
}

int gridExtentCount(DispatchRank rank)
{
    if (rank == DispatchRank::OneD)
        return 1;

    return rank == DispatchRank::TwoD ? 2 : 3;
}

// The type the entry point declares its indices as, and the swizzle HLSL and
// GLSL take them out of their three-component builtins with.
const char* indexTypeName(DispatchRank rank)
{
    if (rank == DispatchRank::OneD)
        return "uint";

    return rank == DispatchRank::TwoD ? "uint2" : "uint3";
}

const char* glslIndexTypeName(DispatchRank rank)
{
    if (rank == DispatchRank::OneD)
        return "uint";

    return rank == DispatchRank::TwoD ? "uvec2" : "uvec3";
}

const char* indexSwizzle(DispatchRank rank)
{
    if (rank == DispatchRank::OneD)
        return ".x";

    return rank == DispatchRank::TwoD ? ".xy" : ".xyz";
}

// A whole expression parenthesised, since an offset or a stride is printed
// into the middle of an index computation.
std::string bracketed(const std::string& expression)
{
    return "(" + expression + ")";
}
} // namespace eacp::GPU::ShaderEmission
