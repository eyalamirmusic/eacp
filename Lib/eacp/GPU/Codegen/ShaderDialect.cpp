#include "ShaderDialect.h"

#include <eacp/Core/Utils/Format.h>

#include <algorithm>
#include <cassert>
#include <span>
#include <string_view>

namespace eacp::GPU::ShaderEmission
{
// The n is for nan and inf.
constexpr auto floatLiteralMarkers = std::string_view {".en"};

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
    std::string_view renamed;
};

// HLSL's fma is double-only; mad is its float multiply-add. The inverse
// hyperbolics are helpers there, keyed on the canonical name.
constexpr auto hlslRenames = Array<BuiltinRename, 8> {{"fract", "frac"},
                                                      {"mix", "lerp"},
                                                      {"dfdx", "ddx"},
                                                      {"dfdy", "ddy"},
                                                      {"fma", "mad"},
                                                      {"asinh", "eacpAsinh"},
                                                      {"acosh", "eacpAcosh"},
                                                      {"atanh", "eacpAtanh"}};

// Metal's own asinh, under the fast math its library compiles with, cancels for
// a negative argument; the helper takes it of the magnitude.
constexpr auto metalRenames =
    Array<BuiltinRename, 1> {BuiltinRename {"asinh", "eacpAsinh"}};

constexpr auto hlslPrefixRenames = Array<BuiltinRename, 2> {
    {"as_type<uint", "asuint"}, {"as_type<float", "asfloat"}};

// GLSL has no log10; the helper table carries the definition.
constexpr auto glslRenames = Array<BuiltinRename, 5> {{"atan2", "atan"},
                                                      {"rsqrt", "inversesqrt"},
                                                      {"dfdx", "dFdx"},
                                                      {"dfdy", "dFdy"},
                                                      {"log10", "eacpLog10"}};

constexpr auto glslPrefixRenames = Array<BuiltinRename, 2> {
    {"as_type<uint", "floatBitsToUint"}, {"as_type<float", "uintBitsToFloat"}};

std::string_view findExactRename(std::span<const BuiltinRename> renames,
                                 const std::string& name)
{
    for (const auto& rename: renames)
        if (name == rename.canonical)
            return rename.renamed;

    return {};
}

std::string_view findPrefixRename(std::span<const BuiltinRename> renames,
                                  const std::string& name)
{
    for (const auto& rename: renames)
        if (name.starts_with(rename.canonical))
            return rename.renamed;

    return {};
}

std::string_view findRename(std::span<const BuiltinRename> exact,
                            std::span<const BuiltinRename> prefixed,
                            const std::string& name)
{
    if (auto renamed = findExactRename(exact, name); !renamed.empty())
        return renamed;

    return findPrefixRename(prefixed, name);
}

// A constructor-style cast is recorded as a call under the target's canonical
// type name, so float2(v) leaves as vec2(v).
std::string_view findDialectRename(Backend backend, const std::string& name)
{
    if (backend == Backend::DirectX)
        return findRename(hlslRenames, hlslPrefixRenames, name);

    if (backend == Backend::Metal)
        return findExactRename(metalRenames, name);

    if (const auto* spelling = glslTypeNameFor(name); spelling != nullptr)
        return spelling;

    return findRename(glslRenames, glslPrefixRenames, name);
}

std::string getCallName(Backend backend, const std::string& name)
{
    if (auto renamed = findDialectRename(backend, name); !renamed.empty())
        return std::string(renamed);

    return name;
}

// GLSL takes a scalar beside a vector only in a few trailing positions.
bool isGenTypeCall(const std::string& name)
{
    return name == "min" || name == "max" || name == "clamp" || name == "mix"
           || name == "step" || name == "smoothstep" || name == "pow"
           || name == "atan2" || name == "fma";
}

// MSL's fma takes one type throughout; HLSL promotes a scalar itself, and has no
// one-argument vector constructor to widen it with.
bool isMetalGenTypeCall(const std::string& name)
{
    return name == "fma";
}

constexpr auto glslComparisonFunctions =
    Array<BuiltinRename, 6> {{"<", "lessThan"},
                             {"<=", "lessThanEqual"},
                             {">", "greaterThan"},
                             {">=", "greaterThanEqual"},
                             {"==", "equal"},
                             {"!=", "notEqual"}};

// GLSL reserves the comparison operators for scalars. Empty for an operator
// with no componentwise function.
std::string_view getGlslComparisonFunction(const std::string& op)
{
    return findExactRename(glslComparisonFunctions, op);
}

// One static sampler per sampling configuration, shared by every texture.
std::string getHlslSamplerName(const TextureSampling& sampling)
{
    return Strings::format("samplerConfig{}", samplingIndex(sampling));
}

std::string_view getVectorComponentSuffix(int component)
{
    assert(component >= 0 && component < 4
           && "eacp: a vector has no component past .w");

    constexpr auto lanes = Array<std::string_view, 4> {".x", ".y", ".z", ".w"};
    return lanes[component];
}

// packed_float4 rather than float4: a BufferRange may start at any four-byte
// offset, and float4 needs sixteen-byte alignment.
std::string_view getMetalPackedVectorType(ValueType type)
{
    constexpr auto floats = Array<std::string_view, 3> {
        "packed_float2", "packed_float3", "packed_float4"};
    constexpr auto uints =
        Array<std::string_view, 3> {"packed_uint2", "packed_uint3", "packed_uint4"};
    constexpr auto ints =
        Array<std::string_view, 3> {"packed_int2", "packed_int3", "packed_int4"};

    auto width = std::clamp(componentCount(type), 2, 4) - 2;

    if (isSignedInteger(type))
        return ints[width];

    return isUnsignedInteger(type) ? uints[width] : floats[width];
}

std::string
    getIndexReference(std::string_view name, DispatchRank rank, int component)
{
    if (rank == DispatchRank::OneD || component == allComponents)
        return std::string(name);

    return Strings::format("{}{}", name, getComponentSuffix(component));
}

std::string wrapInParentheses(const std::string& expression)
{
    return Strings::format("({})", expression);
}
} // namespace eacp::GPU::ShaderEmission
