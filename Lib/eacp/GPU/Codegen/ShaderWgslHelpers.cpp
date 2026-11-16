#include "ShaderWgslHelpers.h"

#include <eacp/Core/Utils/Format.h>

#include <algorithm>
#include <cmath>
#include <span>

namespace eacp::GPU::ShaderEmission
{
namespace
{
struct WgslHelper
{
    std::string_view name;
    std::string_view definition;
};

// The EDSL's other helpers are WGSL's own built-ins (the renames in
// ShaderDialect.cpp); these are the ones it has no single call for.
constexpr auto wgslHelpers = Array<WgslHelper, 20> {{
    {"eacpReadHalf",
     "fn eacpReadHalf(bits: u32, parity: u32) -> f32\n"
     "{\n"
     "    return unpack2x16float(bits >> (16u * parity)).x;\n"
     "}\n\n"},
    {"eacpUnpackBFloat16x2",
     "fn eacpUnpackBFloat16x2(bits: u32) -> vec2<f32>\n"
     "{\n"
     "    return vec2<f32>(bitcast<f32>(bits << 16u), bitcast<f32>(bits & "
     "0xffff0000u));\n"
     "}\n\n"},
    {"eacpReadBFloat16",
     "fn eacpReadBFloat16(bits: u32, parity: u32) -> f32\n"
     "{\n"
     "    return bitcast<f32>((bits >> (16u * parity)) << 16u);\n"
     "}\n\n"},
    {"eacpPackBFloat16x2",
     "fn eacpPackBFloat16x2(values: vec2<f32>) -> u32\n"
     "{\n"
     "    var low = bitcast<u32>(values.x);\n"
     "    var high = bitcast<u32>(values.y);\n"
     "    let lowIsNaN = (low & 0x7fffffffu) > 0x7f800000u;\n"
     "    let highIsNaN = (high & 0x7fffffffu) > 0x7f800000u;\n"
     "    low = select(low + 0x7fffu + ((low >> 16u) & 1u), low | 0x400000u, "
     "lowIsNaN);\n"
     "    high = select(high + 0x7fffu + ((high >> 16u) & 1u), high | 0x400000u, "
     "highIsNaN);\n"
     "    return (low >> 16u) | (high & 0xffff0000u);\n"
     "}\n\n"},
    {"eacpReadInt8",
     "fn eacpReadInt8(bits: u32, byteIndex: u32) -> f32\n"
     "{\n"
     "    let value = (bits >> (8u * byteIndex)) & 0xffu;\n"
     "    return f32(value ^ 0x80u) - 128.0;\n"
     "}\n\n"},
    {"eacpReadUInt8",
     "fn eacpReadUInt8(bits: u32, byteIndex: u32) -> f32\n"
     "{\n"
     "    return f32((bits >> (8u * byteIndex)) & 0xffu);\n"
     "}\n\n"},
    {"eacpUnpackInt8x4",
     "fn eacpUnpackInt8x4(bits: u32) -> vec4<f32>\n"
     "{\n"
     "    return vec4<f32>(f32((bits & 0xffu) ^ 0x80u) - 128.0,\n"
     "                     f32(((bits >> 8u) & 0xffu) ^ 0x80u) - 128.0,\n"
     "                     f32(((bits >> 16u) & 0xffu) ^ 0x80u) - 128.0,\n"
     "                     f32(((bits >> 24u) & 0xffu) ^ 0x80u) - 128.0);\n"
     "}\n\n"},
    {"eacpUnpackUInt8x4",
     "fn eacpUnpackUInt8x4(bits: u32) -> vec4<f32>\n"
     "{\n"
     "    return vec4<f32>(f32(bits & 0xffu),\n"
     "                     f32((bits >> 8u) & 0xffu),\n"
     "                     f32((bits >> 16u) & 0xffu),\n"
     "                     f32((bits >> 24u) & 0xffu));\n"
     "}\n\n"},
    {"eacpUnpackInt4x4",
     "fn eacpUnpackInt4x4(bits: u32) -> vec4<f32>\n"
     "{\n"
     "    return vec4<f32>(f32((bits & 0xfu) ^ 0x8u) - 8.0,\n"
     "                     f32(((bits >> 4u) & 0xfu) ^ 0x8u) - 8.0,\n"
     "                     f32(((bits >> 8u) & 0xfu) ^ 0x8u) - 8.0,\n"
     "                     f32(((bits >> 12u) & 0xfu) ^ 0x8u) - 8.0);\n"
     "}\n\n"},
    {"eacpUnpackUInt4x4",
     "fn eacpUnpackUInt4x4(bits: u32) -> vec4<f32>\n"
     "{\n"
     "    return vec4<f32>(f32(bits & 0xfu),\n"
     "                     f32((bits >> 4u) & 0xfu),\n"
     "                     f32((bits >> 8u) & 0xfu),\n"
     "                     f32((bits >> 12u) & 0xfu));\n"
     "}\n\n"},
    {"eacpPackInt8x4",
     "fn eacpPackInt8x4(values: vec4<i32>) -> u32\n"
     "{\n"
     "    return u32(values.x & 0xff) | (u32(values.y & 0xff) << 8u)\n"
     "           | (u32(values.z & 0xff) << 16u)\n"
     "           | (u32(values.w & 0xff) << 24u);\n"
     "}\n\n"},
    {"eacpPackUInt8x4",
     "fn eacpPackUInt8x4(values: vec4<u32>) -> u32\n"
     "{\n"
     "    return (values.x & 0xffu) | ((values.y & 0xffu) << 8u)\n"
     "           | ((values.z & 0xffu) << 16u)\n"
     "           | ((values.w & 0xffu) << 24u);\n"
     "}\n\n"},
    {"eacpPackUnorm8x4",
     "fn eacpPackUnorm8x4(values: vec4<f32>) -> u32\n"
     "{\n"
     "    let bytes = vec4<u32>(floor(0.5 + 255.0 * saturate(values)));\n"
     "    return bytes.x | (bytes.y << 8u) | (bytes.z << 16u) | (bytes.w << 24u);\n"
     "}\n\n"},
    {"eacpPackSnorm8x4",
     "fn eacpPackSnorm8x4(values: vec4<f32>) -> u32\n"
     "{\n"
     "    let bytes = vec4<i32>(floor(0.5 + 127.0 * clamp(values, vec4<f32>(-1.0), "
     "vec4<f32>(1.0))))\n"
     "                & vec4<i32>(0xff);\n"
     "    return u32(bytes.x) | (u32(bytes.y) << 8u) | (u32(bytes.z) << 16u)\n"
     "           | (u32(bytes.w) << 24u);\n"
     "}\n\n"},
    {"eacpPackUnorm16x2",
     "fn eacpPackUnorm16x2(values: vec2<f32>) -> u32\n"
     "{\n"
     "    let halves = vec2<u32>(floor(0.5 + 65535.0 * saturate(values)));\n"
     "    return halves.x | (halves.y << 16u);\n"
     "}\n\n"},
    {"eacpPackSnorm16x2",
     "fn eacpPackSnorm16x2(values: vec2<f32>) -> u32\n"
     "{\n"
     "    let halves = vec2<i32>(floor(0.5 + 32767.0 * clamp(values, "
     "vec2<f32>(-1.0), vec2<f32>(1.0))))\n"
     "                 & vec2<i32>(0xffff);\n"
     "    return u32(halves.x) | (u32(halves.y) << 16u);\n"
     "}\n\n"},
    {"eacpUnpackUnorm8x4",
     "fn eacpUnpackUnorm8x4(bits: u32) -> vec4<f32>\n"
     "{\n"
     "    return vec4<f32>(f32(bits & 0xffu), f32((bits >> 8u) & 0xffu),\n"
     "                     f32((bits >> 16u) & 0xffu), f32(bits >> 24u)) / 255.0;\n"
     "}\n\n"},
    {"eacpUnpackSnorm8x4",
     "fn eacpUnpackSnorm8x4(bits: u32) -> vec4<f32>\n"
     "{\n"
     "    let bytes = vec4<f32>(f32((bits & 0xffu) ^ 0x80u) - 128.0,\n"
     "                          f32(((bits >> 8u) & 0xffu) ^ 0x80u) - 128.0,\n"
     "                          f32(((bits >> 16u) & 0xffu) ^ 0x80u) - 128.0,\n"
     "                          f32((bits >> 24u) ^ 0x80u) - 128.0);\n"
     "    return max(bytes / 127.0, vec4<f32>(-1.0));\n"
     "}\n\n"},
    {"eacpUnpackUnorm16x2",
     "fn eacpUnpackUnorm16x2(bits: u32) -> vec2<f32>\n"
     "{\n"
     "    return vec2<f32>(f32(bits & 0xffffu), f32(bits >> 16u)) / 65535.0;\n"
     "}\n\n"},
    {"eacpUnpackSnorm16x2",
     "fn eacpUnpackSnorm16x2(bits: u32) -> vec2<f32>\n"
     "{\n"
     "    let halves = vec2<f32>(f32((bits & 0xffffu) ^ 0x8000u) - 32768.0,\n"
     "                           f32((bits >> 16u) ^ 0x8000u) - 32768.0);\n"
     "    return max(halves / 32767.0, vec2<f32>(-1.0));\n"
     "}\n\n"},
}};

// One body per helper WGSL cannot overload, written once over the argument's
// whole type {0}, so a vector runs it componentwise without a loop. The
// helpers' own reasons are the C-family ones' (ShaderHelpers.cpp).
constexpr auto wgslTypedHelperBodies = Array<WgslHelper, 5> {{
    {"asinh",
     "fn {1}(x: {0}) -> {0}\n"
     "{{\n"
     "    let r = asinh(abs(x));\n"
     "    return select(r, -r, x < {0}(0.0));\n"
     "}}\n\n"},
    {"eacpErf",
     "fn {1}(x: {0}) -> {0}\n"
     "{{\n"
     "    let a = abs(x);\n"
     "    let t = 1.0 / (1.0 + 0.3275911 * a);\n"
     "    let e = 1.0 - t * (0.254829592 + t * (-0.284496736 + t * (1.421413741\n"
     "            + t * (-1.453152027 + t * 1.061405429)))) * exp(-a * a);\n"
     "    return select(select(e, -e, x < {0}(0.0)), x, a == {0}(0.0));\n"
     "}}\n\n"},
    {"eacpErfc",
     "fn {1}(x: {0}) -> {0}\n"
     "{{\n"
     "    let a = abs(x);\n"
     "    let t = 1.0 / (1.0 + 0.3275911 * a);\n"
     "    let e = t * (0.254829592 + t * (-0.284496736 + t * (1.421413741\n"
     "            + t * (-1.453152027 + t * 1.061405429)))) * exp(-a * a);\n"
     "    return select(select(e, 2.0 - e, x < {0}(0.0)), {0}(1.0), a == "
     "{0}(0.0));\n"
     "}}\n\n"},
    {"eacpSaturatingTanh",
     "fn {1}(x: {0}) -> {0}\n"
     "{{\n"
     "    return select(select(tanh(x), {0}(-1.0), x <= {0}(-10.0)), {0}(1.0),\n"
     "                  x >= {0}(10.0));\n"
     "}}\n\n"},
    {"log10",
     "fn {1}(x: {0}) -> {0}\n"
     "{{\n"
     "    return log2(x) * 0.30102999566;\n"
     "}}\n\n"},
}};

const WgslHelper* findWgslTypedHelper(std::string_view name)
{
    for (const auto& helper: wgslTypedHelperBodies)
        if (helper.name == name)
            return &helper;

    return nullptr;
}

bool doesGraphCallWgslHelper(const ShaderGraph& graph, std::string_view name)
{
    for (auto node = 0; node < graph.nodeCount(); ++node)
    {
        const auto& expr = graph.expr(node);

        if (expr.kind == ExprKind::Call && expr.text == name)
            return true;
    }

    return false;
}

// The packed fragment fallback widens through these without a call naming
// them.
bool isWgslHelperUsed(const ShaderGraph& graph, std::string_view name)
{
    if (name == "eacpReadHalf"
        && graph.usesPackedSimdMatrix(SimdMatrixElement::Half))
        return true;

    if (name == "eacpReadBFloat16"
        && graph.usesPackedSimdMatrix(SimdMatrixElement::BFloat16))
        return true;

    return doesGraphCallWgslHelper(graph, name);
}

std::string getWgslTypedHelperDefinitions(const ShaderGraph& graph)
{
    auto defined = Vector<std::string> {};
    auto definitions = std::string {};

    for (auto node = 0; node < graph.nodeCount(); ++node)
    {
        const auto& expr = graph.expr(node);
        const auto* helper =
            expr.kind == ExprKind::Call ? findWgslTypedHelper(expr.text) : nullptr;

        if (helper == nullptr || expr.args.empty())
            continue;

        auto argument = graph.expr(expr.args[0]).type;
        auto name = getWgslTypedHelperName(expr.text, argument);

        if (defined.contains(name))
            continue;

        defined.add(name);
        definitions +=
            Strings::format(helper->definition, wgslTypeName(argument), name);
    }

    return definitions;
}

std::string getWgslMatrixDivideDefinition(ValueType matrix)
{
    auto columns = std::string {};

    for (auto column = 0; column < matrixColumns(matrix); ++column)
        columns += Strings::format("{}m[{}] / s", column > 0 ? ", " : "", column);

    return Strings::format(
        "fn {0}(m: {1}, s: {2}) -> {1}\n{{\n    return {1}({3});\n}}\n\n",
        getWgslMatrixDivideName(matrix),
        wgslTypeName(matrix),
        isHalf(matrix) ? "f16" : "f32",
        columns);
}

std::string getWgslMatrixDivideDefinitions(const ShaderGraph& graph)
{
    auto defined = Vector<ValueType> {};
    auto definitions = std::string {};

    for (auto node = 0; node < graph.nodeCount(); ++node)
    {
        const auto& expr = graph.expr(node);
        auto divides = expr.kind == ExprKind::Binary && expr.text.empty()
                       && expr.op == '/' && isMatrix(expr.type);

        if (!divides || defined.contains(expr.type))
            continue;

        defined.add(expr.type);
        definitions += getWgslMatrixDivideDefinition(expr.type);
    }

    return definitions;
}

bool hasNonFiniteConstant(const ShaderGraph& graph)
{
    for (auto node = 0; node < graph.nodeCount(); ++node)
    {
        const auto& expr = graph.expr(node);
        auto isFloat = expr.type == ValueType::Float || expr.type == ValueType::Half;

        if (expr.kind == ExprKind::Constant && isFloat && !std::isfinite(expr.value))
            return true;
    }

    return false;
}

bool doesHoldHalf(const ShaderGraph& graph)
{
    if (graph.usesHalf() || graph.usesHalfStorage())
        return true;

    for (const auto& declaration: graph.overrides())
        if (isHalf(declaration.type))
            return true;

    for (auto structId = 0; structId < graph.structCount(); ++structId)
        for (const auto& member: graph.structType(structId).members)
            if (isHalf(member.type))
                return true;

    return false;
}

// WGSL's keywords and the words it reserves, none of which an identifier may
// be.
constexpr std::string_view wgslReservedWords[] = {"alias",
                                                  "break",
                                                  "case",
                                                  "const",
                                                  "const_assert",
                                                  "continue",
                                                  "continuing",
                                                  "default",
                                                  "diagnostic",
                                                  "discard",
                                                  "else",
                                                  "enable",
                                                  "false",
                                                  "fn",
                                                  "for",
                                                  "if",
                                                  "let",
                                                  "loop",
                                                  "override",
                                                  "requires",
                                                  "return",
                                                  "struct",
                                                  "switch",
                                                  "true",
                                                  "var",
                                                  "while",
                                                  "NULL",
                                                  "Self",
                                                  "abstract",
                                                  "active",
                                                  "alignas",
                                                  "alignof",
                                                  "as",
                                                  "asm",
                                                  "asm_fragment",
                                                  "async",
                                                  "attribute",
                                                  "auto",
                                                  "await",
                                                  "become",
                                                  "cast",
                                                  "catch",
                                                  "class",
                                                  "co_await",
                                                  "co_return",
                                                  "co_yield",
                                                  "coherent",
                                                  "column_major",
                                                  "common",
                                                  "compile",
                                                  "compile_fragment",
                                                  "concept",
                                                  "const_cast",
                                                  "consteval",
                                                  "constexpr",
                                                  "constinit",
                                                  "crate",
                                                  "debugger",
                                                  "decltype",
                                                  "delete",
                                                  "demote",
                                                  "demote_to_helper",
                                                  "do",
                                                  "dynamic_cast",
                                                  "enum",
                                                  "explicit",
                                                  "export",
                                                  "extends",
                                                  "extern",
                                                  "external",
                                                  "fallthrough",
                                                  "filter",
                                                  "final",
                                                  "finally",
                                                  "friend",
                                                  "from",
                                                  "fxgroup",
                                                  "get",
                                                  "goto",
                                                  "groupshared",
                                                  "highp",
                                                  "impl",
                                                  "implements",
                                                  "import",
                                                  "inline",
                                                  "instanceof",
                                                  "interface",
                                                  "layout",
                                                  "lowp",
                                                  "macro",
                                                  "macro_rules",
                                                  "match",
                                                  "mediump",
                                                  "meta",
                                                  "mod",
                                                  "module",
                                                  "move",
                                                  "mut",
                                                  "mutable",
                                                  "namespace",
                                                  "new",
                                                  "nil",
                                                  "noexcept",
                                                  "noinline",
                                                  "nointerpolation",
                                                  "non_coherent",
                                                  "noncoherent",
                                                  "noperspective",
                                                  "null",
                                                  "nullptr",
                                                  "of",
                                                  "operator",
                                                  "package",
                                                  "packoffset",
                                                  "partition",
                                                  "pass",
                                                  "patch",
                                                  "pixelfragment",
                                                  "precise",
                                                  "precision",
                                                  "premerge",
                                                  "priv",
                                                  "protected",
                                                  "pub",
                                                  "public",
                                                  "readonly",
                                                  "ref",
                                                  "regardless",
                                                  "register",
                                                  "reinterpret_cast",
                                                  "require",
                                                  "resource",
                                                  "restrict",
                                                  "self",
                                                  "set",
                                                  "shared",
                                                  "signed",
                                                  "sizeof",
                                                  "smooth",
                                                  "snorm",
                                                  "static",
                                                  "static_assert",
                                                  "static_cast",
                                                  "std",
                                                  "subroutine",
                                                  "super",
                                                  "target",
                                                  "template",
                                                  "this",
                                                  "thread_local",
                                                  "throw",
                                                  "trait",
                                                  "try",
                                                  "type",
                                                  "typedef",
                                                  "typeid",
                                                  "typename",
                                                  "typeof",
                                                  "union",
                                                  "unless",
                                                  "unorm",
                                                  "unsafe",
                                                  "unsized",
                                                  "use",
                                                  "using",
                                                  "varying",
                                                  "virtual",
                                                  "volatile",
                                                  "wgsl",
                                                  "where",
                                                  "with",
                                                  "writeonly",
                                                  "yield"};

bool isWgslIdentifier(std::string_view name)
{
    if (name.empty() || name == "_" || name.starts_with("__"))
        return false;

    return std::find(
               std::begin(wgslReservedWords), std::end(wgslReservedWords), name)
           == std::end(wgslReservedWords);
}
} // namespace

std::string getWgslHeader(const ShaderGraph& graph)
{
    auto header = std::string {};

    if (doesHoldHalf(graph))
        header += "enable f16;\n";

    if (!graph.isCompute())
        header += "diagnostic(off, derivative_uniformity);\n";

    return header.empty() ? header : header + "\n";
}

std::string getWgslHelperDefinitions(const ShaderGraph& graph)
{
    auto definitions = std::string {};

    if (hasNonFiniteConstant(graph))
        definitions +=
            Strings::format("fn {}(bits: u32) -> u32\n{{\n    return bits;\n}}\n\n",
                            wgslRuntimeBitsHelper);

    for (const auto& helper: wgslHelpers)
        if (isWgslHelperUsed(graph, helper.name))
            definitions += helper.definition;

    return definitions + getWgslTypedHelperDefinitions(graph)
           + getWgslMatrixDivideDefinitions(graph);
}

bool usesOnlyWgslIdentifiers(const ShaderGraph& graph)
{
    for (auto structId = 0; structId < graph.structCount(); ++structId)
    {
        const auto& type = graph.structType(structId);

        if (!isWgslIdentifier(type.name))
            return false;

        for (const auto& member: type.members)
            if (!isWgslIdentifier(member.name))
                return false;
    }

    for (auto function = 0; function < graph.functionCount(); ++function)
        if (graph.function(function).name.find("__") != std::string::npos)
            return false;

    for (const auto& declaration: graph.overrides())
        if (declaration.name.find("__") != std::string::npos)
            return false;

    return true;
}
} // namespace eacp::GPU::ShaderEmission
