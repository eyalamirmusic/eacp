#include "ShaderDialect.h"

#include <eacp/Core/Utils/Format.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
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

std::string formatWgslFloatLiteral(float value, bool half)
{
    if (std::isfinite(value))
        return formatShaderFloatLiteral(value) + (half ? "h" : "");

    // Through a function, so the value is not a constant expression, which WGSL
    // refuses to evaluate to one that is not finite.
    auto bits = Strings::format("bitcast<f32>({}({}u))",
                                wgslRuntimeBitsHelper,
                                std::bit_cast<std::uint32_t>(value));

    return half ? Strings::format("f16({})", bits) : bits;
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

// The EDSL's built-ins are WGSL's, so most of its helpers are WGSL's own
// functions under their own names. The normalized packs are not: Chrome on
// Metal lowers them to Metal's, which round differently from WGSL's formula,
// so they are helpers spelling it (ShaderWgslHelpers.cpp).
constexpr auto wgslRenames =
    Array<BuiltinRename, 16> {{{"rsqrt", "inverseSqrt"},
                               {"dfdx", "dpdx"},
                               {"dfdy", "dpdy"},
                               {"faceforward", "faceForward"},
                               {"eacpCountOneBits", "countOneBits"},
                               {"eacpReverseBits", "reverseBits"},
                               {"eacpCountLeadingZeros", "countLeadingZeros"},
                               {"eacpCountTrailingZeros", "countTrailingZeros"},
                               {"eacpFirstLeadingBit", "firstLeadingBit"},
                               {"eacpFirstTrailingBit", "firstTrailingBit"},
                               {"eacpExtractBits", "extractBits"},
                               {"eacpInsertBits", "insertBits"},
                               {"eacpQuantizeToF16", "quantizeToF16"},
                               {"eacpUnpackHalf2", "unpack2x16float"},
                               {"eacpPackHalf2", "pack2x16float"},
                               {"log10", "eacpLog10"}}};

// The helpers WGSL is given one definition per argument type of.
constexpr auto wgslTypedHelpers = Array<std::string_view, 5> {
    "eacpErf", "eacpErfc", "eacpSaturatingTanh", "log10", "asinh"};

// Chrome on Metal lowers WGSL's asinh to Metal's, which cancels for a negative
// argument under fast math, so it is taken of the magnitude as on Metal.
constexpr auto wgslTypedRenames =
    Array<BuiltinRename, 2> {{"log10", "eacpLog10"}, {"asinh", "eacpAsinh"}};

constexpr auto bitcastPrefix = std::string_view {"as_type<"};

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
    {
        if (const auto* spelling = hlslTypeNameFor(name); spelling != nullptr)
            return spelling;

        return findRename(hlslRenames, hlslPrefixRenames, name);
    }

    if (backend == Backend::Metal)
        return findExactRename(metalRenames, name);

    if (backend == Backend::WebGPU)
    {
        if (const auto* spelling = wgslTypeNameFor(name); spelling != nullptr)
            return spelling;

        return findExactRename(wgslRenames, name);
    }

    if (const auto* spelling = glslTypeNameFor(name); spelling != nullptr)
        return spelling;

    return findRename(glslRenames, glslPrefixRenames, name);
}

constexpr auto halfExactCalls = Array<std::string_view, 10> {"abs",
                                                             "min",
                                                             "max",
                                                             "clamp",
                                                             "floor",
                                                             "ceil",
                                                             "trunc",
                                                             "round",
                                                             "sign",
                                                             "transpose"};

bool isRoundedToHalf(const Expr& expr)
{
    if (!isHalf(expr.type))
        return false;

    if (expr.kind == ExprKind::Binary || expr.kind == ExprKind::Mul)
        return true;

    return expr.kind == ExprKind::Call
           && !halfExactCalls.contains(std::string_view {expr.text});
}

std::string getRoundHalfFunction(ValueType type)
{
    if (!isMatrix(type))
        return "eacpRoundHalf";

    return Strings::format(
        "eacpRoundHalf{}x{}", matrixRows(type), matrixColumns(type));
}

// as_type<uint2>(x) is bitcast<vec2<u32>>(x).
std::string getWgslBitcastName(const std::string& name)
{
    auto target = name.substr(bitcastPrefix.size());

    if (!target.empty() && target.back() == '>')
        target.pop_back();

    const auto* spelling = wgslTypeNameFor(target);
    return Strings::format("bitcast<{}>", spelling != nullptr ? spelling : target);
}

bool isWgslTypedHelper(const std::string& name)
{
    return wgslTypedHelpers.contains(std::string_view {name});
}

std::string getWgslTypedHelperName(const std::string& name, ValueType argument)
{
    if (!isWgslTypedHelper(name))
        return {};

    auto renamed = findExactRename(wgslTypedRenames, name);
    auto base = renamed.empty() ? name : std::string(renamed);
    return Strings::format("{}_{}", base, typeName(argument));
}

std::string getWgslMatrixDivideName(ValueType matrix)
{
    return Strings::format("eacpMatrixDivide_{}", typeName(matrix));
}

std::string getCallName(Backend backend, const std::string& name)
{
    if (backend == Backend::WebGPU && name.starts_with(bitcastPrefix))
        return getWgslBitcastName(name);

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
    constexpr auto halves =
        Array<std::string_view, 3> {"packed_half2", "packed_half3", "packed_half4"};

    auto width = std::clamp(componentCount(type), 2, 4) - 2;

    if (isSignedInteger(type))
        return ints[width];

    if (isHalf(type))
        return halves[width];

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
