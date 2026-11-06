#include "ShaderExpressionPrinter.h"

#include "ShaderGroupSpelling.h"
#include "UniformLayout.h"

#include <eacp/Core/Utils/Format.h>

#include <climits>

namespace eacp::GPU::ShaderEmission
{
namespace
{
using ExpressionPrintFunction = std::string (ExprPrinter::*)(const Expr&) const;

struct ExpressionPrintEntry
{
    ExprKind kind;
    ExpressionPrintFunction print;
};

std::string printNumberedName(std::string_view prefix, int index)
{
    return Strings::format("{}{}", prefix, index);
}

std::string printSubscript(std::string_view prefix, int index, const std::string& at)
{
    return Strings::format("{}{}[{}]", prefix, index, at);
}

bool isUnsignedConstant(const Expr& expr)
{
    return expr.type == ValueType::UInt;
}

// A char unless it did not fit in one: only the two shifts.
std::string getBinaryOperator(const Expr& expr)
{
    return expr.text.empty() ? std::string(1, expr.op) : expr.text;
}

bool isShiftOperator(const std::string& op)
{
    return op == "<<" || op == ">>";
}
} // namespace

int ExprPrinter::findCarryingVarying(int slot) const
{
    return slot < attributeVaryings.size() ? attributeVaryings[slot] : -1;
}

std::string ExprPrinter::printReference(int node) const
{
    if (locals[node] >= 0)
        return Strings::format("t{}", locals[node]);

    return print(node);
}

ValueType ExprPrinter::getBroadcastType(const Expr& call) const
{
    auto widens = (backend == Backend::Vulkan && isGenTypeCall(call.text))
                  || (backend == Backend::Metal && isMetalGenTypeCall(call.text));

    if (!widens)
        return ValueType::Float;

    auto widest = ValueType::Float;

    for (auto argument: call.args)
    {
        auto type = graph.expr(argument).type;

        if (componentCount(type) > componentCount(widest))
            widest = type;
    }

    return widest;
}

// Metal rounds nothing, so a Float layer is rounded to nearest even first, as
// Vulkan and D3D do.
std::string ExprPrinter::printMetalLayer(int node) const
{
    if (graph.expr(node).type == ValueType::Int)
        return Strings::format("uint({})", printReference(node));

    return Strings::format("uint(rint({}))", printReference(node));
}

std::string ExprPrinter::printFloatLayer(int layer) const
{
    if (graph.expr(layer).type == ValueType::Int)
        return Strings::format("float({})", printReference(layer));

    return printReference(layer);
}

std::string ExprPrinter::printLayeredCoordinate(int uv, int layer) const
{
    return Strings::format("{}({}, {})",
                           getDialectTypeName(backend, ValueType::Float3),
                           printReference(uv),
                           printFloatLayer(layer));
}

std::string ExprPrinter::printLayerSample(const Expr& expr,
                                          const std::string& name,
                                          const std::string& sampler) const
{
    auto uv = expr.args[0];
    auto layer = expr.args[1];

    if (backend == Backend::Metal)
        return Strings::format("{}.sample({}, {}, {})",
                               name,
                               sampler,
                               printReference(uv),
                               printMetalLayer(layer));

    if (backend == Backend::DirectX)
        return Strings::format(
            "{}.Sample({}, {})", name, sampler, printLayeredCoordinate(uv, layer));

    auto call =
        Strings::format("texture({}, {})", name, printLayeredCoordinate(uv, layer));

    return isDepth(graph.textureKind(expr.index)) ? call + ".r" : call;
}

// HLSL's level-zero form: SampleCmp needs derivatives a PCF loop lacks.
std::string ExprPrinter::printCompareSample(const Expr& expr,
                                            const std::string& name,
                                            const std::string& sampler) const
{
    auto layered = expr.args.size() == 3;
    auto uv = expr.args[0];
    auto reference = printReference(expr.args.back());

    if (backend == Backend::Metal)
    {
        auto layer = layered ? Strings::format("{}, ", printMetalLayer(expr.args[1]))
                             : std::string {};

        return Strings::format("{}.sample_compare({}, {}, {}{})",
                               name,
                               sampler,
                               printReference(uv),
                               layer,
                               reference);
    }

    if (backend == Backend::DirectX)
    {
        auto coordinate =
            layered ? printLayeredCoordinate(uv, expr.args[1]) : printReference(uv);

        return Strings::format("{}.SampleCmpLevelZero({}, {}, {})",
                               name,
                               sampler,
                               coordinate,
                               reference);
    }

    if (layered)
        return Strings::format("texture({}, vec4({}, {}, {}))",
                               name,
                               printReference(uv),
                               printFloatLayer(expr.args[1]),
                               reference);

    return Strings::format(
        "texture({}, vec3({}, {}))", name, printReference(uv), reference);
}

std::string ExprPrinter::printWidened(int node, ValueType wide) const
{
    auto argument = printReference(node);

    if (componentCount(wide) == 1 || componentCount(graph.expr(node).type) > 1)
        return argument;

    return Strings::format("{}({})", getDialectTypeName(backend, wide), argument);
}

std::string ExprPrinter::printArguments(const Expr& expr, ValueType wide) const
{
    auto text = std::string {};

    for (auto i = 0; i < expr.args.size(); ++i)
    {
        if (i > 0)
            text += ", ";

        text += printWidened(expr.args[i], wide);
    }

    return text;
}

std::string ExprPrinter::print(int node) const
{
    static constexpr auto printers = Array<ExpressionPrintEntry, 29> {
        {ExprKind::Input, &ExprPrinter::printInput},
        {ExprKind::Varying, &ExprPrinter::printVarying},
        {ExprKind::Uniform, &ExprPrinter::printUniform},
        {ExprKind::Constant, &ExprPrinter::printConstant},
        {ExprKind::Construct, &ExprPrinter::printConstruct},
        {ExprKind::Swizzle, &ExprPrinter::printSwizzle},
        {ExprKind::Call, &ExprPrinter::printCall},
        {ExprKind::Unary, &ExprPrinter::printUnary},
        {ExprKind::Binary, &ExprPrinter::printBinary},
        {ExprKind::Compare, &ExprPrinter::printCompare},
        {ExprKind::Select, &ExprPrinter::printSelect},
        {ExprKind::VarRead, &ExprPrinter::printVarRead},
        {ExprKind::Mul, &ExprPrinter::printMul},
        {ExprKind::Sample, &ExprPrinter::printSample},
        {ExprKind::Fetch, &ExprPrinter::printFetch},
        {ExprKind::ThreadId, &ExprPrinter::printThreadId},
        {ExprKind::BufferRead, &ExprPrinter::printBufferRead},
        {ExprKind::BufferVectorRead, &ExprPrinter::printBufferVectorRead},
        {ExprKind::AtomicLoad, &ExprPrinter::printAtomicLoad},
        {ExprKind::ArrayRead, &ExprPrinter::printArrayRead},
        {ExprKind::LocalId, &ExprPrinter::printLocalId},
        {ExprKind::GroupId, &ExprPrinter::printGroupId},
        {ExprKind::GridExtent, &ExprPrinter::printGridExtent},
        {ExprKind::SharedRead, &ExprPrinter::printSharedRead},
        {ExprKind::SimdGroupIndex, &ExprPrinter::printSimdGroupIndex},
        {ExprKind::BuiltIn, &ExprPrinter::printBuiltInNode},
        {ExprKind::TextureFunction, &ExprPrinter::printTextureFunction},
        {ExprKind::TextureQuery, &ExprPrinter::printTextureQuery},
        {ExprKind::MatrixColumn, &ExprPrinter::printMatrixColumn},
    };

    const auto& expr = graph.expr(node);

    for (const auto& entry: printers)
        if (entry.kind == expr.kind)
            return (this->*entry.print)(expr);

    return {};
}

std::string ExprPrinter::printInput(const Expr& expr) const
{
    auto carrier = findCarryingVarying(expr.index);

    if (carrier >= 0)
        return getVaryingName(backend, carrier);

    return getAttributeName(backend, expr.index);
}

std::string ExprPrinter::printVarying(const Expr& expr) const
{
    return getVaryingName(backend, expr.index);
}

// A two-row matrix is held as float4 columns on Metal (UniformLayout.h).
std::string ExprPrinter::printUniform(const Expr& expr) const
{
    auto name = printNumberedName("uniforms.u", expr.index);

    if (backend != Backend::Metal || !isMetalWidenedUniform(expr.type))
        return name;

    auto columns = std::string {};

    for (auto column = 0; column < matrixColumns(expr.type); ++column)
        columns +=
            Strings::format("{}{}[{}].xy", column > 0 ? ", " : "", name, column);

    return Strings::format("{}({})", typeName(expr.type), columns);
}

// A uint above INT_MAX is held in Expr::index as a negative.
std::string ExprPrinter::printConstant(const Expr& expr) const
{
    if (isUnsignedConstant(expr))
    {
        auto bits = static_cast<unsigned>(expr.index);
        return Strings::format("{}u", bits);
    }

    // -2147483648 is the negation of a literal no int holds, which GLSL refuses.
    if (expr.type == ValueType::Int && expr.index == INT_MIN)
        return "(-2147483647 - 1)";

    if (expr.type == ValueType::Int)
        return Strings::format("{}", expr.index);

    if (expr.type == ValueType::Bool)
        return expr.index != 0 ? "true" : "false";

    return formatShaderFloatLiteral(expr.value);
}

// HLSL fills a matrix from rows, so the columns are taken as the rows of the
// transposed shape - MSL's spelling, rows first in HLSL - and transpose()
// restores the column-major value.
std::string ExprPrinter::printConstruct(const Expr& expr) const
{
    auto transposes = backend == Backend::DirectX && isMatrix(expr.type);
    auto name = transposes ? std::string_view {typeName(expr.type)}
                           : getDialectTypeName(backend, expr.type);
    auto text =
        Strings::format("{}({})", name, printArguments(expr, ValueType::Float));

    if (transposes)
        return Strings::format("transpose({})", text);

    return text;
}

// HLSL holds the matrix itself, whose subscript is a row; a column of it is a
// row of its transpose.
std::string ExprPrinter::printMatrixColumn(const Expr& expr) const
{
    auto matrix = printReference(expr.args[0]);
    auto index = printReference(expr.args[1]);

    if (backend == Backend::DirectX)
        return Strings::format("transpose({})[{}]", matrix, index);

    return Strings::format("({})[{}]", matrix, index);
}

std::string ExprPrinter::printSwizzle(const Expr& expr) const
{
    return Strings::format("({}).{}", printReference(expr.args[0]), expr.text);
}

std::string ExprPrinter::printCall(const Expr& expr) const
{
    return Strings::format("{}({})",
                           getCallName(backend, expr.text),
                           printArguments(expr, getBroadcastType(expr)));
}

// The operand gets its own parentheses so -(-1.0) never prints as --1.0.
std::string ExprPrinter::printUnary(const Expr& expr) const
{
    auto isGlslMaskNegation = backend == Backend::Vulkan && expr.op == '!'
                              && componentCount(expr.type) > 1;

    if (isGlslMaskNegation)
        return Strings::format("not({})", printReference(expr.args[0]));

    return Strings::format("({}({}))", expr.op, printReference(expr.args[0]));
}

bool ExprPrinter::isGlslShift(const std::string& op) const
{
    return backend == Backend::Vulkan && isShiftOperator(op);
}

bool ExprPrinter::isGlslSignedRemainder(const std::string& op, ValueType type) const
{
    return backend == Backend::Vulkan && op == "%" && isSignedInteger(type);
}

// GLSL refuses a scalar on the left of a vector shift, and leaves % undefined
// on a negative operand.
std::string ExprPrinter::printBinary(const Expr& expr) const
{
    auto op = getBinaryOperator(expr);

    auto left = isGlslShift(op) ? printWidened(expr.args[0], expr.type)
                                : printReference(expr.args[0]);
    auto right = printReference(expr.args[1]);

    if (isGlslSignedRemainder(op, expr.type))
        return Strings::format("({} - (({} / {}) * {}))", left, left, right, right);

    return Strings::format("({} {} {})", left, op, right);
}

std::string ExprPrinter::printCompare(const Expr& expr) const
{
    auto left = printReference(expr.args[0]);
    auto right = printReference(expr.args[1]);

    if (backend == Backend::Vulkan && componentCount(expr.type) > 1)
        if (auto name = getGlslComparisonFunction(expr.text); !name.empty())
            return Strings::format("{}({}, {})", name, left, right);

    return Strings::format("({} {} {})", left, expr.text, right);
}

std::string ExprPrinter::printSelect(const Expr& expr) const
{
    return Strings::format("({} ? {} : {})",
                           printReference(expr.args[0]),
                           printReference(expr.args[1]),
                           printReference(expr.args[2]));
}

// An element of a private array carries its (already clamped) index.
std::string ExprPrinter::printVarRead(const Expr& expr) const
{
    if (expr.args.empty())
        return printNumberedName("v", expr.index);

    return printSubscript("v", expr.index, printReference(expr.args[0]));
}

std::string ExprPrinter::printMul(const Expr& expr) const
{
    auto left = printReference(expr.args[0]);
    auto right = printReference(expr.args[1]);

    if (backend != Backend::DirectX)
        return Strings::format("({} * {})", left, right);

    return Strings::format("mul({}, {})", left, right);
}

std::string ExprPrinter::printSample(const Expr& expr) const
{
    auto name = printNumberedName("texture", expr.index);
    auto sampler = backend == Backend::Metal
                       ? printNumberedName("sampler", expr.index)
                       : getHlslSamplerName(graph.textureSampling(expr.index));

    if (expr.text == "compare")
        return printCompareSample(expr, name, sampler);

    if (isLayered(graph.textureKind(expr.index)))
        return printLayerSample(expr, name, sampler);

    return printPlainSample(expr, name, sampler);
}

// GLSL's sampler2D over a depth image returns four channels.
std::string ExprPrinter::printPlainSample(const Expr& expr,
                                          const std::string& name,
                                          const std::string& sampler) const
{
    auto uv = printReference(expr.args[0]);
    auto hasLevel = expr.args.size() >= 2;

    if (backend == Backend::Vulkan)
    {
        auto call = hasLevel ? Strings::format("textureLod({}, {}, {})",
                                               name,
                                               uv,
                                               printReference(expr.args[1]))
                             : Strings::format("texture({}, {})", name, uv);

        if (graph.textureKind(expr.index) == TextureKind::Depth2D)
            return call + ".r";

        return call;
    }

    if (!hasLevel)
    {
        auto method = backend == Backend::Metal ? "sample" : "Sample";
        return Strings::format("{}.{}({}, {})", name, method, sampler, uv);
    }

    auto level = printReference(expr.args[1]);

    if (backend == Backend::Metal)
        return Strings::format(
            "{}.sample({}, {}, level({}))", name, sampler, uv, level);

    return Strings::format("{}.SampleLevel({}, {}, {})", name, sampler, uv, level);
}

// Metal takes unsigned coordinates, so they go through int2 first: a negative
// one then reads as zero, as HLSL's Load does.
std::string ExprPrinter::printFetch(const Expr& expr) const
{
    auto name = printNumberedName("texture", expr.index);
    auto given = printReference(expr.args[0]);
    auto coordinates =
        graph.expr(expr.args[0]).type == ValueType::Int2
            ? given
            : Strings::format(
                  "{}({})", getDialectTypeName(backend, ValueType::Int2), given);

    if (backend == Backend::Metal)
        return Strings::format("{}.read(uint2({}))", name, coordinates);

    if (backend == Backend::Vulkan)
        return Strings::format("texelFetch({}, {}, 0)", name, coordinates);

    return Strings::format("{}.Load(int3({}, 0))", name, coordinates);
}

std::string ExprPrinter::printThreadId(const Expr& expr) const
{
    return getIndexReference("gid", graph.dispatchRank(), expr.index);
}

std::string ExprPrinter::printBufferRead(const Expr& expr) const
{
    return printSubscript("buffer", expr.index, printReference(expr.args[0]));
}

std::string ExprPrinter::printBufferVectorRead(const Expr& expr) const
{
    auto name = printNumberedName("buffer", expr.index);
    auto base = printReference(expr.args[0]);
    auto type = getDialectTypeName(backend, expr.type);

    if (backend == Backend::Metal)
        return Strings::format("{}(*((device const {}*) ({} + {})))",
                               type,
                               getMetalPackedVectorType(expr.type),
                               name,
                               base);

    auto elements = std::string {};

    for (auto component = 0; component < componentCount(expr.type); ++component)
    {
        if (component > 0)
            elements += ", ";

        elements += Strings::format("{}[{}", name, base);

        if (component > 0)
            elements += Strings::format(" + {}u", component);

        elements += "]";
    }

    return Strings::format("{}({})", type, elements);
}

std::string ExprPrinter::printAtomicLoad(const Expr& expr) const
{
    auto element =
        printSubscript("buffer", expr.index, printReference(expr.args[0]));

    if (backend == Backend::Metal)
        return Strings::format("atomic_load_explicit(&{}, memory_order_relaxed)",
                               element);

    return element;
}

std::string ExprPrinter::printArrayRead(const Expr& expr) const
{
    return printSubscript("a", expr.index, printReference(expr.args[0]));
}

std::string ExprPrinter::printLocalId(const Expr& expr) const
{
    return getIndexReference("lid", graph.dispatchRank(), expr.index);
}

std::string ExprPrinter::printGroupId(const Expr& expr) const
{
    return getIndexReference("tgid", graph.dispatchRank(), expr.index);
}

std::string ExprPrinter::printGridExtent(const Expr& expr) const
{
    if (graph.dispatchRank() == DispatchRank::OneD)
        return "uniforms.count";

    return Strings::format("uniforms.{}", getGridExtentName(expr.index));
}

std::string ExprPrinter::printSharedRead(const Expr& expr) const
{
    auto element = printSubscript("s", expr.index, printReference(expr.args[0]));

    if (backend == Backend::Metal && graph.sharedArrays()[expr.index].atomic)
        return Strings::format("atomic_load_explicit(&{}, memory_order_relaxed)",
                               element);

    return element;
}

std::string ExprPrinter::printSimdGroupIndex(const Expr&) const
{
    if (backend == Backend::Metal)
        return "simdIndex";

    return Strings::format("({} / {}u)", getGroupLaneName(backend), simdGroupWidth);
}

std::string ExprPrinter::printBuiltInNode(const Expr& expr) const
{
    auto which = static_cast<ShaderBuiltIn>(expr.index);
    return printBuiltIn(which);
}

// HLSL's SV_Position gives clip w where the others give 1 / w.
std::string ExprPrinter::printFragmentCoordinate() const
{
    if (backend == Backend::Vulkan)
        return "gl_FragCoord";

    if (backend == Backend::DirectX)
        return "float4(input.position.xyz, 1.0 / input.position.w)";

    return "input.position";
}

// Metal declares it with gid's width, every kernel input having to agree.
std::string ExprPrinter::printMetalGroupCount() const
{
    const auto rank = graph.dispatchRank();

    if (rank == DispatchRank::OneD)
        return "uint3(groupCount, 1u, 1u)";

    return rank == DispatchRank::TwoD ? "uint3(groupCount, 1u)" : "groupCount";
}

// D3D has no SV_ for it below SM 6, so there it is the extent the dispatch
// supplied over the group, rounded up: what every direct dispatch encodes.
std::string ExprPrinter::printGroupCount() const
{
    if (backend == Backend::Metal)
        return printMetalGroupCount();

    if (backend == Backend::Vulkan)
        return "gl_NumWorkGroups";

    const auto rank = graph.dispatchRank();
    const auto group = graph.threadGroupShape();
    const auto sizes = Array<int, 3> {group.x, group.y, group.z};
    auto axes = Array<std::string, 3> {"1u", "1u", "1u"};

    for (auto axis = 0; axis < getGridExtentCount(rank); ++axis)
    {
        auto extent = rank == DispatchRank::OneD
                          ? std::string {"uniforms.count"}
                          : Strings::format("uniforms.{}", getGridExtentName(axis));

        axes[axis] = Strings::format("(({} - 1u) / {}u + 1u)", extent, sizes[axis]);
    }

    return Strings::format("uint3({}, {}, {})", axes[0], axes[1], axes[2]);
}

std::string ExprPrinter::printBuiltIn(ShaderBuiltIn which) const
{
    auto isGlsl = backend == Backend::Vulkan;
    auto isMetal = backend == Backend::Metal;

    switch (which)
    {
        case ShaderBuiltIn::FragmentCoordinate:
            return printFragmentCoordinate();
        case ShaderBuiltIn::VertexId:
            return isGlsl ? "uint(gl_VertexIndex)" : "vertexId";
        case ShaderBuiltIn::InstanceId:
            return isGlsl ? "uint(gl_InstanceIndex)" : "instanceId";
        case ShaderBuiltIn::FrontFacing:
            return isGlsl ? "gl_FrontFacing" : "frontFacing";
        case ShaderBuiltIn::SampleIndex:
            return isGlsl ? "uint(gl_SampleID)" : "sampleIndex";
        case ShaderBuiltIn::SampleMask:
            return isGlsl ? "uint(gl_SampleMaskIn[0])" : "sampleMaskIn";
        case ShaderBuiltIn::LocalIndex:
            return isMetal ? "localIndex" : std::string(getGroupLaneName(backend));
        case ShaderBuiltIn::GroupCount:
            return printGroupCount();
        case ShaderBuiltIn::SimdLaneIndex:
            if (isMetal)
                return "simdLane";

            return Strings::format(
                "({} % {}u)", getGroupLaneName(backend), simdGroupWidth);
        case ShaderBuiltIn::SimdGroupSize:
            return isMetal ? "simdSize" : Strings::format("{}u", simdGroupWidth);
    }

    return {};
}
} // namespace eacp::GPU::ShaderEmission
