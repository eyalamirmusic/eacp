#pragma once

#include "ShaderGraph.h"

#include <eacp/Core/Utils/Format.h>

#include <string>
#include <string_view>

namespace eacp::GPU::ShaderEmission
{
enum class Backend
{
    Metal,
    DirectX,
    Vulkan
};

constexpr bool isDialectLinked(Backend dialect)
{
    return EACP_ALL_SHADER_DIALECTS || dialect == Backend::EACP_HOST_SHADER_DIALECT;
}

// With one dialect linked every backend is that one, so a branch on it folds at
// compile time and the other dialects' code never reaches the binary.
constexpr Backend getEmittedDialect(Backend backend)
{
    return EACP_ALL_SHADER_DIALECTS ? backend : Backend::EACP_HOST_SHADER_DIALECT;
}

constexpr bool isDialect(Backend backend, Backend dialect)
{
    return getEmittedDialect(backend) == dialect;
}

constexpr std::string_view getDialectTypeName(Backend backend, ValueType type)
{
    if (isDialect(backend, Backend::Vulkan))
        return glslTypeName(type);

    return isDialect(backend, Backend::DirectX) ? hlslTypeName(type)
                                                : typeName(type);
}
std::string formatShaderFloatLiteral(float value);

// What makes a float literal an f16 one: MSL's 0.5h, GLSL's 0.5hf (from
// GL_EXT_shader_explicit_arithmetic_types_float16). HLSL holds f16 in float,
// and the literal is already a value a half holds.
constexpr std::string_view getHalfLiteralSuffix(Backend backend)
{
    if (isDialect(backend, Backend::Metal))
        return "h";

    return isDialect(backend, Backend::Vulkan) ? "hf" : "";
}

// MSL's interpolation attribute, written after the member name; nothing for
// perspective at the centre, which is what a member is without one.
constexpr std::string_view getMetalInterpolation(Interpolation interpolation)
{
    if (interpolation.type == InterpolationType::Flat)
        return " [[flat]]";

    const auto linear = interpolation.type == InterpolationType::Linear;

    switch (interpolation.sampling)
    {
        case InterpolationSampling::Centroid:
            return linear ? " [[centroid_no_perspective]]"
                          : " [[centroid_perspective]]";
        case InterpolationSampling::Sample:
            return linear ? " [[sample_no_perspective]]" : " [[sample_perspective]]";
        default:
            return linear ? " [[center_no_perspective]]" : std::string_view {};
    }
}

constexpr std::string_view getInterpolationTypeQualifier(Backend backend,
                                                         InterpolationType type)
{
    if (type == InterpolationType::Flat)
        return isDialect(backend, Backend::Vulkan) ? "flat " : "nointerpolation ";

    return type == InterpolationType::Linear ? "noperspective " : "";
}

constexpr std::string_view getSamplingQualifier(InterpolationSampling sampling)
{
    if (sampling == InterpolationSampling::Centroid)
        return "centroid ";

    return sampling == InterpolationSampling::Sample ? "sample " : "";
}

inline std::string getAttributeSemantic(Backend backend, int index)
{
    if (isDialect(backend, Backend::Vulkan))
        return {};

    if (isDialect(backend, Backend::Metal))
        return Strings::format(" [[attribute({})]]", index);

    return Strings::format(" : TEXCOORD{}", index);
}

// Metal writes the interpolation after the member name, as part of the semantic.
inline std::string
    getVaryingSemantic(Backend backend, int index, Interpolation interpolation)
{
    if (isDialect(backend, Backend::Metal))
        return std::string {getMetalInterpolation(interpolation)};

    if (!isDialect(backend, Backend::DirectX))
        return {};

    return Strings::format(" : TEXCOORD{}", index);
}

constexpr std::string_view getPositionSemantic(Backend backend)
{
    if (isDialect(backend, Backend::Vulkan))
        return {};

    if (isDialect(backend, Backend::Metal))
        return " [[position]]";

    return " : SV_Position";
}

// HLSL's and GLSL's modifiers ahead of the member: the type's, then the
// sampling's, so `noperspective centroid` in both.
inline std::string getInterpolationQualifier(Backend backend,
                                             Interpolation interpolation)
{
    if (isDialect(backend, Backend::Metal))
        return {};

    return std::string {getInterpolationTypeQualifier(backend, interpolation.type)}
           + std::string {getSamplingQualifier(interpolation.sampling)};
}

inline std::string getLocationLayout(int index)
{
    return Strings::format("layout(location = {}) ", index);
}

// GLSL names stage I/O attrN and varyN: aN and vN are taken by other locals.
inline std::string getAttributeName(Backend backend, int slot)
{
    if (isDialect(backend, Backend::Vulkan))
        return Strings::format("attr{}", slot);

    return Strings::format("input.a{}", slot);
}

inline std::string getVaryingName(Backend backend, int index)
{
    if (isDialect(backend, Backend::Vulkan))
        return Strings::format("vary{}", index);

    return Strings::format("input.v{}", index);
}

// Whether HLSL rounds a node's value to f16 where it is computed: HLSL holds a
// Half in float, so every arithmetic result of the f16 family - an operator, a
// product, a built-in, a conversion into it - goes to f16 bits and back,
// which is what makes it the value Metal's half and GLSL's float16_t hold. A
// built-in whose result is already a half of its arguments is left alone.
bool isRoundedToHalf(const Expr& expr);

// FXC converts a float4 to a float2x2 as readily as it passes it as a float4,
// so a matrix overload sharing the vectors' name makes their calls ambiguous:
// each matrix rounds under a name of its own, eacpRoundHalf2x4 for a float2x4.
std::string getRoundHalfFunction(ValueType type);

std::string getCallName(Backend backend, const std::string& name);
bool isGenTypeCall(const std::string& name);
bool isMetalGenTypeCall(const std::string& name);
std::string_view getGlslComparisonFunction(const std::string& op);

std::string getHlslSamplerName(const TextureSampling& sampling);
constexpr std::string_view getComponentSuffix(int component)
{
    if (component == 0)
        return ".x";

    return component == 1 ? ".y" : ".z";
}

std::string_view getVectorComponentSuffix(int component);
std::string_view getMetalPackedVectorType(ValueType type);

std::string
    getIndexReference(std::string_view name, DispatchRank rank, int component);

constexpr std::string_view getGridExtentName(int component)
{
    if (component == 0)
        return "width";

    return component == 1 ? "height" : "depth";
}

constexpr int getGridExtentCount(DispatchRank rank)
{
    if (rank == DispatchRank::OneD)
        return 1;

    return rank == DispatchRank::TwoD ? 2 : 3;
}

constexpr std::string_view getIndexSwizzle(DispatchRank rank)
{
    if (rank == DispatchRank::OneD)
        return ".x";

    return rank == DispatchRank::TwoD ? ".xy" : ".xyz";
}

std::string wrapInParentheses(const std::string& expression);
} // namespace eacp::GPU::ShaderEmission
