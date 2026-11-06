#include "ShaderDialect.h"

#include <eacp/Core/Utils/Format.h>

#include <cassert>
#include <span>
#include <string_view>

namespace eacp::GPU::ShaderEmission
{
// A decimal point, an exponent, or the n of nan and inf's spelling all keep a
// number from reading as an integer literal.
constexpr auto floatLiteralMarkers = ".en";

std::string formatShaderFloatLiteral(float value)
{
    auto text = Strings::shortestRoundTrip(value);

    if (text.find_first_of(floatLiteralMarkers) == std::string::npos)
        text += ".0";

    return text;
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
const char* findDialectRename(Backend backend, const std::string& name)
{
    if (backend == Backend::DirectX)
        return findRename(hlslRenames, hlslPrefixRenames, name);

    if (backend != Backend::Vulkan)
        return nullptr;

    if (const auto* spelling = glslTypeNameFor(name))
        return spelling;

    return findRename(glslRenames, glslPrefixRenames, name);
}

// Call nodes carry the canonical (MSL) builtin name; renamed here per dialect.
std::string getCallName(Backend backend, const std::string& name)
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
// componentwise mask is a function there. Asked only for a boolean vector;
// empty for an operator with no such function.
std::string_view getGlslComparisonFunction(const std::string& op)
{
    const auto* function = findExactRename(glslComparisonFunctions, op);

    if (function == nullptr)
        return {};

    return function;
}

// The HLSL sampler a texture with this sampling reads through. Named for the
// configuration rather than for the texture, because that is what it is: the
// root signature declares samplingConfigurations static samplers and every
// texture sampled that way shares one. See TextureSampling.
std::string getHlslSamplerName(const TextureSampling& sampling)
{
    return "samplerConfig" + std::to_string(samplingIndex(sampling));
}

// The lane of a vector value, which is a different question: a thread index
// reaches three components and a vector reaches four, so the one above stops
// where it does and this one names .w. Nothing in the EDSL is wider than four,
// so a fifth component is a caller's mistake rather than a lane to name.
std::string_view getVectorComponentSuffix(int component)
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
std::string_view getMetalPackedVectorType(ValueType type)
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

} // namespace eacp::GPU::ShaderEmission
