#include "ShaderExpressionPrinter.h"

#include "ShaderGroupSpelling.h"

#include <eacp/Core/Utils/Format.h>

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

std::string printNumberedName(const char* prefix, int index)
{
    return Strings::format("{}{}", prefix, index);
}

std::string printSubscript(const char* prefix, int index, const std::string& at)
{
    return Strings::format("{}{}[{}]", prefix, index, at);
}

bool isUnsignedConstant(const Expr& expr)
{
    return expr.type == ValueType::UInt;
}

// The operator is a char unless it did not fit in one, which is only the two
// shifts.
std::string getBinaryOperator(const Expr& expr)
{
    return expr.text.empty() ? std::string(1, expr.op) : expr.text;
}

bool isShiftOperator(const std::string& op)
{
    return op == "<<" || op == ">>";
}
} // namespace

// -1 when the stage reads the attribute itself; empty outside the fragment
// stage.
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

// Float, whose width is one, when nothing is to be broadcast.
ValueType ExprPrinter::getBroadcastType(const Expr& call) const
{
    if (backend != Backend::Vulkan || !isGenTypeCall(call.text))
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

// Metal takes the layer as an index and rounds nothing, so a Float layer is
// rounded to nearest even first - the rule Vulkan and D3D apply to the
// coordinate's third component, which is how the other two take it.
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

// HLSL takes the level-zero form because a depth plane has one level and
// because SampleCmp needs derivatives, which a PCF loop does not have.
// GLSL folds the reference into the coordinate's last component.
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
    static constexpr ExpressionPrintEntry printers[] = {
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
    };

    const auto& expr = graph.expr(node);

    for (const auto& entry: printers)
        if (entry.kind == expr.kind)
            return (this->*entry.print)(expr);

    return {};
}

// Only the vertex stage is handed the attributes, so a fragment-stage read of
// one reads the varying carrying it across.
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

std::string ExprPrinter::printUniform(const Expr& expr) const
{
    return printNumberedName("uniforms.u", expr.index);
}

// The uint, int and bool spellings are shared by MSL and HLSL, like floatN. A
// signed literal needs no suffix at all: an integer literal is already an int
// in both languages.
//
// Expr::index is the int the three of them share, so a uint above INT_MAX is
// held there as a negative and has to be read back as what it was: 4294967295u,
// never -1u.
std::string ExprPrinter::printConstant(const Expr& expr) const
{
    if (isUnsignedConstant(expr))
    {
        auto bits = static_cast<unsigned>(expr.index);
        return Strings::format("{}u", bits);
    }

    if (expr.type == ValueType::Int)
        return Strings::format("{}", expr.index);

    if (expr.type == ValueType::Bool)
        return expr.index != 0 ? "true" : "false";

    return formatShaderFloatLiteral(expr.value);
}

// MSL and GLSL fill a matrix from columns, HLSL from rows, so transpose() is
// what restores the column-major value there.
std::string ExprPrinter::printConstruct(const Expr& expr) const
{
    auto text = Strings::format("{}({})",
                                getDialectTypeName(backend, expr.type),
                                printArguments(expr, ValueType::Float));

    if (backend == Backend::DirectX && isMatrix(expr.type))
        return Strings::format("transpose({})", text);

    return text;
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

// GLSL gives ! to a scalar bool only; the componentwise negation of a mask is
// not(). The operand gets its own parentheses: negating a negative constant
// must print (-(-1.0)), never the pre-decrement (--1.0).
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

// GLSL refuses a scalar on the left of a shift whose right operand is a vector;
// MSL and HLSL broadcast it themselves. GLSL also leaves % undefined on a
// negative operand, so the truncating remainder is written out of the division.
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

// A mask is the operator in two dialects, a function in GLSL.
std::string ExprPrinter::printCompare(const Expr& expr) const
{
    auto left = printReference(expr.args[0]);
    auto right = printReference(expr.args[1]);

    if (backend == Backend::Vulkan && componentCount(expr.type) > 1)
        if (auto name = getGlslComparisonFunction(expr.text); !name.empty())
            return Strings::format("{}({}, {})", name, left, right);

    return Strings::format("({} {} {})", left, expr.text, right);
}

// Both languages spell the conditional operator the same way, and both
// evaluate it without branching for scalar operands.
std::string ExprPrinter::printSelect(const Expr& expr) const
{
    return Strings::format("({} ? {} : {})",
                           printReference(expr.args[0]),
                           printReference(expr.args[1]),
                           printReference(expr.args[2]));
}

std::string ExprPrinter::printVarRead(const Expr& expr) const
{
    return printNumberedName("v", expr.index);
}

// MSL and GLSL spell a matrix product with *; HLSL uses mul(). All three read a
// vector on the left of one as a row.
std::string ExprPrinter::printMul(const Expr& expr) const
{
    auto left = printReference(expr.args[0]);
    auto right = printReference(expr.args[1]);

    if (backend != Backend::DirectX)
        return Strings::format("({} * {})", left, right);

    return Strings::format("mul({}, {})", left, right);
}

// Texture sample at a float2 coordinate. A second argument is the mip level the
// shader picked, which each backend spells its own way: Metal as an extra
// argument to the same call, HLSL as a different method.
//
// The two backends also name the sampler differently, and that is the one place
// their declarations genuinely differ. MSL passes a sampler as a function
// argument, so there is one per texture and it carries the texture's index;
// HLSL binds one to a register, and there is one per sampling configuration
// that every texture declaring that sampling shares. See TextureSampling.
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

// GLSL's sampler2D over a depth image hands back four channels, so the .r is
// the one float the node's type says.
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

// A texel read at integer coordinates. Metal takes them unsigned, so anything
// not already signed-integer goes through int2 first: a negative coordinate
// then wraps to a large unsigned one and reads as zero, which is what HLSL's
// Load does with it directly. The level is 0 - GPU::Texture has no mips - and
// D3D carries it in the coordinate's third component. GLSL takes the
// coordinate signed and the level explicitly - there are no derivatives to pick
// one from in a fetch.
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

// Both kernel scaffoldings declare the work-item id as gid: a uint over the
// flat count in a 1D kernel, a uint2 or uint3 over the grid otherwise, where
// the node carries which component it asked for - or the whole of it.
std::string ExprPrinter::printThreadId(const Expr& expr) const
{
    return getIndexReference("gid", graph.dispatchRank(), expr.index);
}

std::string ExprPrinter::printBufferRead(const Expr& expr) const
{
    return printSubscript("buffer", expr.index, printReference(expr.args[0]));
}

// One load where the dialect has a spelling for one, and the componentwise
// construct it stands in for where it has not. See metalPackedVectorType for
// why the Metal form reinterprets the pointer as a packed type rather than as a
// plain float4.
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

// HLSL has nothing to spell: a UAV element of an RWStructuredBuffer<uint> is
// already the thing an interlocked operation acts on, and reading one is a
// subscript. MSL wraps its atomic_uint, so the value has to be taken out of it.
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

// The threadgroup indices ride the same scaffolding as gid: both backends'
// entry points bind them to these names, a scalar in a 1D kernel and a vector
// of the rank's width otherwise.
std::string ExprPrinter::printLocalId(const Expr& expr) const
{
    return getIndexReference("lid", graph.dispatchRank(), expr.index);
}

std::string ExprPrinter::printGroupId(const Expr& expr) const
{
    return getIndexReference("tgid", graph.dispatchRank(), expr.index);
}

// The implicit bound the dispatch appended to the uniform block, under the
// names the block declares it with.
std::string ExprPrinter::printGridExtent(const Expr& expr) const
{
    if (graph.dispatchRank() == DispatchRank::OneD)
        return "uniforms.count";

    return Strings::format("uniforms.{}", getGridExtentName(expr.index));
}

std::string ExprPrinter::printSharedRead(const Expr& expr) const
{
    return printSubscript("s", expr.index, printReference(expr.args[0]));
}

// Metal has the builtin; the other two divide the flat local index by the
// width, which is the same numbering and the one the fallback's own arithmetic
// is written against.
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

// HLSL's SV_Position hands the fragment stage clip w where the other two hand
// 1 / w, so it is turned round to agree. All three put the origin at the top
// left, y down, in pixels with centres at .5: Vulkan has no other origin, and
// eacp's flipped viewport moves clip space, not the framebuffer.
std::string ExprPrinter::printBuiltIn(ShaderBuiltIn which) const
{
    if (which == ShaderBuiltIn::FragmentCoordinate)
    {
        if (backend == Backend::Vulkan)
            return "gl_FragCoord";

        if (backend == Backend::DirectX)
            return "float4(input.position.xyz, 1.0 / input.position.w)";

        return "input.position";
    }

    auto vertexId = which == ShaderBuiltIn::VertexId;

    if (backend == Backend::Vulkan)
        return vertexId ? "uint(gl_VertexIndex)" : "uint(gl_InstanceIndex)";

    return vertexId ? "vertexId" : "instanceId";
}
} // namespace eacp::GPU::ShaderEmission
