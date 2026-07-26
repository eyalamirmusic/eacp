#include "ShaderEmitter.h"

#include "../Frame/ComputePass.h"
#include "../Frame/RenderPass.h"
#include "ShaderGraph.h"
#include "UniformLayout.h"

#include <cstdio>
#include <vector>

// The single source-of-truth walker. MSL and HLSL differ only in the binding
// syntax and the stage scaffolding captured by the helpers below; the expression
// printer is fully shared because vector constructors, swizzles, operators and
// the float2/3/4 type names spell identically in both languages.

namespace eacp::GPU
{
namespace
{
enum class Backend
{
    Metal,
    DirectX
};

std::string floatLiteral(float value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%g", value);

    auto text = std::string(buffer);

    if (text.find('.') == std::string::npos && text.find('e') == std::string::npos
        && text.find('n') == std::string::npos)
        text += ".0";

    return text;
}

std::string attributeSemantic(Backend backend, int index)
{
    if (backend == Backend::Metal)
        return " [[attribute(" + std::to_string(index) + ")]]";

    return " : TEXCOORD" + std::to_string(index);
}

std::string varyingSemantic(Backend backend, int index)
{
    if (backend == Backend::Metal)
        return {};

    return " : TEXCOORD" + std::to_string(index);
}

std::string positionSemantic(Backend backend)
{
    if (backend == Backend::Metal)
        return " [[position]]";

    return " : SV_Position";
}

// Call nodes carry the canonical (MSL) builtin name; the few HLSL spells
// differently are translated here.
std::string callName(Backend backend, const std::string& name)
{
    if (backend == Backend::DirectX)
    {
        if (name == "fract")
            return "frac";

        if (name == "mix")
            return "lerp";

        if (name == "dfdx")
            return "ddx";

        if (name == "dfdy")
            return "ddy";
    }

    return name;
}

// Prints one stage's expressions. Nodes the stage plan named as locals print
// as tN references; everything else prints inline. print() spells out a node's
// own expression (used for both inline nodes and local definitions), ref() is
// what children and outputs go through, so shared subtrees collapse to a name.
struct ExprPrinter
{
    std::string ref(int node) const
    {
        if (locals[node] >= 0)
            return "t" + std::to_string(locals[node]);

        return print(node);
    }

    std::string print(int node) const
    {
        const auto& expr = graph.expr(node);

        switch (expr.kind)
        {
            case ExprKind::Input:
                return "input.a" + std::to_string(expr.index);

            case ExprKind::Varying:
                return "input.v" + std::to_string(expr.index);

            case ExprKind::Uniform:
                return "uniforms.u" + std::to_string(expr.index);

            case ExprKind::Constant:
                // The uint, int and bool spellings are shared by MSL and HLSL,
                // like floatN. A signed literal needs no suffix at all: an
                // integer literal is already an int in both languages.
                if (expr.type == ValueType::UInt)
                    return std::to_string(expr.index) + "u";

                if (expr.type == ValueType::Int)
                    return std::to_string(expr.index);

                if (expr.type == ValueType::Bool)
                    return expr.index != 0 ? "true" : "false";

                return floatLiteral(expr.value);

            case ExprKind::Construct:
            {
                auto text = std::string(typeName(expr.type)) + "(";

                for (auto i = 0; i < expr.args.size(); ++i)
                {
                    if (i > 0)
                        text += ", ";

                    text += ref(expr.args[i]);
                }

                text += ")";

                // float2x2/float3x3/float4x4(c0..) pass the columns. MSL fills a
                // matrix from columns, but HLSL fills it from rows, so the same
                // call yields the transpose there; transpose() restores the
                // column-major value, so the mul() paths stay identical across
                // both backends.
                if (backend == Backend::DirectX && isMatrix(expr.type))
                    return "transpose(" + text + ")";

                return text;
            }

            case ExprKind::Swizzle:
                return "(" + ref(expr.args[0]) + ")." + expr.text;

            case ExprKind::Call:
            {
                auto text = callName(backend, expr.text) + "(";

                for (auto i = 0; i < expr.args.size(); ++i)
                {
                    if (i > 0)
                        text += ", ";

                    text += ref(expr.args[i]);
                }

                return text + ")";
            }

            case ExprKind::Unary:
                // The operand gets its own parentheses: negating a negative
                // constant must print (-(-1.0)), never the pre-decrement
                // (--1.0).
                return "(" + std::string(1, expr.op) + "(" + ref(expr.args[0])
                       + "))";

            case ExprKind::Binary:
            {
                // The operator is a char unless it did not fit in one, which is
                // only the two shifts.
                auto op = expr.text.empty() ? std::string(1, expr.op) : expr.text;

                return "(" + ref(expr.args[0]) + " " + op + " " + ref(expr.args[1])
                       + ")";
            }

            case ExprKind::Compare:
                return "(" + ref(expr.args[0]) + " " + expr.text + " "
                       + ref(expr.args[1]) + ")";

            case ExprKind::Select:
                // Both languages spell the conditional operator the same way,
                // and both evaluate it without branching for scalar operands.
                return "(" + ref(expr.args[0]) + " ? " + ref(expr.args[1]) + " : "
                       + ref(expr.args[2]) + ")";

            case ExprKind::VarRead:
                return "v" + std::to_string(expr.index);

            case ExprKind::Mul:
            {
                // Matrix * vector. MSL spells it with the * operator
                // (column-major); HLSL multiplies a matrix and vector with
                // mul().
                auto matrix = ref(expr.args[0]);
                auto vector = ref(expr.args[1]);

                if (backend == Backend::Metal)
                    return "(" + matrix + " * " + vector + ")";

                return "mul(" + matrix + ", " + vector + ")";
            }

            case ExprKind::Sample:
            {
                // Texture sample at a float2 coordinate, through the sampler
                // declared at the same index as the texture. A second argument
                // is the mip level the shader picked, which each backend spells
                // its own way: Metal as an extra argument to the same call,
                // HLSL as a different method.
                auto name = "texture" + std::to_string(expr.index);
                auto sampler = "sampler" + std::to_string(expr.index);
                auto uv = ref(expr.args[0]);

                if (expr.args.size() < 2)
                {
                    auto method =
                        backend == Backend::Metal ? ".sample(" : ".Sample(";

                    return name + method + sampler + ", " + uv + ")";
                }

                auto level = ref(expr.args[1]);

                if (backend == Backend::Metal)
                    return name + ".sample(" + sampler + ", " + uv + ", level("
                           + level + "))";

                return name + ".SampleLevel(" + sampler + ", " + uv + ", " + level
                       + ")";
            }

            case ExprKind::Fetch:
            {
                // A texel read at integer coordinates. Metal takes them
                // unsigned, so the float2 goes through int2 first: a negative
                // coordinate then wraps to a large unsigned one and reads as
                // zero, which is what HLSL's Load does with it directly. The
                // level is 0 - GPU::Texture has no mips - and D3D carries it in
                // the coordinate's third component.
                auto name = "texture" + std::to_string(expr.index);
                auto coordinates = "int2(" + ref(expr.args[0]) + ")";

                if (backend == Backend::Metal)
                    return name + ".read(uint2(" + coordinates + "))";

                return name + ".Load(int3(" + coordinates + ", 0))";
            }

            case ExprKind::ThreadId:
                // Both kernel scaffoldings declare the 1D work-item id as gid.
                return "gid";

            case ExprKind::BufferRead:
                return "buffer" + std::to_string(expr.index) + "["
                       + ref(expr.args[0]) + "]";

            case ExprKind::ArrayRead:
                return "a" + std::to_string(expr.index) + "[" + ref(expr.args[0])
                       + "]";
        }

        return {};
    }

    const ShaderGraph& graph;
    Backend backend;
    const std::vector<int>& locals; // node id -> local index, -1 = inline
};

// Operation nodes are worth naming when evaluated more than once; leaf reads
// and swizzles stay inline - naming them saves nothing and hurts readability.
bool wantsLocal(ExprKind kind)
{
    switch (kind)
    {
        case ExprKind::Construct:
        case ExprKind::Call:
        case ExprKind::Unary:
        case ExprKind::Binary:
        case ExprKind::Compare:
        case ExprKind::Select:
        case ExprKind::Mul:
        case ExprKind::Sample:
        case ExprKind::Fetch:
        case ExprKind::BufferRead:
        case ExprKind::ArrayRead:
            return true;

        case ExprKind::Input:
        case ExprKind::Varying:
        case ExprKind::Uniform:
        case ExprKind::Constant:
        case ExprKind::Swizzle:
        case ExprKind::VarRead:
        case ExprKind::ThreadId:
            return false;
    }

    return false;
}

// Counts how many references each node receives across the stage's roots: one
// per root plus one per parent edge, visiting each node's children only once.
void countUses(const ShaderGraph& graph,
               int node,
               std::vector<int>& uses,
               std::vector<char>& seen)
{
    if (node < 0)
        return;

    ++uses[node];

    if (seen[node])
        return;

    seen[node] = 1;

    for (auto argument: graph.expr(node).args)
        countUses(graph, argument, uses, seen);
}

// Which nodes a run of expressions evaluates more than once, in dependency
// (post) order so every definition precedes its uses. A node that already holds
// a name is left alone, and so is everything under it: it is already computed.
void orderLocals(const ShaderGraph& graph,
                 int node,
                 const std::vector<int>& uses,
                 const std::vector<int>& locals,
                 std::vector<char>& seen,
                 std::vector<int>& order)
{
    if (node < 0 || seen[node] || locals[node] >= 0)
        return;

    seen[node] = 1;

    for (auto argument: graph.expr(node).args)
        orderLocals(graph, argument, uses, locals, seen, order);

    if (uses[node] > 1 && wantsLocal(graph.expr(node).kind))
        order.push_back(node);
}

// Which constant arrays a run of expressions subscripts, following the elements
// of one that is used in case an element subscripts another.
void collectArrays(const ShaderGraph& graph,
                   int node,
                   std::vector<char>& used,
                   std::vector<char>& seen)
{
    if (node < 0 || seen[node])
        return;

    seen[node] = 1;

    const auto& expr = graph.expr(node);

    if (expr.kind == ExprKind::ArrayRead && used[expr.index] == 0)
    {
        used[expr.index] = 1;

        for (auto element: graph.arrays()[expr.index].elements)
            collectArrays(graph, element, used, seen);
    }

    for (auto argument: expr.args)
        collectArrays(graph, argument, used, seen);
}

// Which variables running a statement can leave holding something else -
// following the bodies of an if or a loop, since what they write is written
// just the same.
void collectWrites(const ShaderGraph& graph, int block, std::vector<char>& written);

void collectWrites(const ShaderGraph& graph,
                   const Statement& statement,
                   std::vector<char>& written)
{
    switch (statement.kind)
    {
        case StatementKind::Declare:
        case StatementKind::Assign:
            written[statement.slot] = 1;
            return;

        case StatementKind::If:
            collectWrites(graph, statement.body, written);

            if (statement.elseBody >= 0)
                collectWrites(graph, statement.elseBody, written);

            return;

        case StatementKind::Loop:
            collectWrites(graph, statement.body, written);
            return;

        case StatementKind::Break:
        case StatementKind::Continue:
            return;
    }
}

void collectWrites(const ShaderGraph& graph, int block, std::vector<char>& written)
{
    for (auto index: graph.block(block).statements)
        collectWrites(graph, graph.statement(index), written);
}

bool readsAny(const ShaderGraph& graph,
              int node,
              const std::vector<char>& written,
              std::vector<char>& seen)
{
    if (node < 0 || seen[node])
        return false;

    seen[node] = 1;

    const auto& expr = graph.expr(node);

    if (expr.kind == ExprKind::VarRead && written[expr.index] != 0)
        return true;

    for (auto argument: expr.args)
        if (readsAny(graph, argument, written, seen))
            return true;

    return false;
}

// Emits one stage: its statements, then the expressions its outputs are.
//
// Any operation evaluated more than once becomes a tN local, so a shared
// subtree is computed - and printed - once instead of being inlined at every
// use. Control flow is what bounds that sharing, and the two rules it imposes
// are the whole of what makes this different from printing an expression tree:
//
// A name is given up the moment a statement writes a variable the value behind
// it read - which is what stops `d` computed before an `if` from standing for
// the same thing after a body that moved what it was computed from.
//
// A loop condition takes no name at all. It is printed into the while header,
// so binding it to a local ahead of the loop would test a value that never
// changes again; every name open in the enclosing block is given up there too,
// since the header is re-evaluated after the body has run.
struct StageEmitter
{
    StageEmitter(const ShaderGraph& graphToUse, Backend backend)
        : locals((std::size_t) graphToUse.nodeCount(), -1)
        , printer {graphToUse, backend, locals}
    {
    }

    const ShaderGraph& graph() const { return printer.graph; }

    // The locals a standalone run of expressions needs - a stage's outputs,
    // which no statement follows - counted over just those expressions.
    std::string defineFor(const std::vector<int>& roots, const std::string& indent)
    {
        auto open = std::vector<int> {};
        return define(roots, indent, countUsesOver(roots), open);
    }

    // The constant arrays a stage subscripts, declared at the top of its
    // function. Both languages spell a const array of a floatN the same way, so
    // this needs no per-backend form; what it does need is to run before any
    // name has been handed out, which is why it is the first thing a stage
    // emits. That is also why an element may read a uniform or a varying but
    // not a mutable local: no local has been declared yet at that point.
    //
    // Emitted in slot order, so an array whose elements read another one finds
    // it already there.
    std::string declareArrays(const std::vector<int>& roots,
                              const std::string& indent)
    {
        const auto& arrays = graph().arrays();

        if (arrays.empty())
            return {};

        auto used = std::vector<char>((std::size_t) arrays.size(), 0);
        auto seen = std::vector<char>((std::size_t) graph().nodeCount(), 0);

        for (auto root: roots)
            collectArrays(graph(), root, used, seen);

        auto source = std::string {};

        for (auto slot = 0; slot < arrays.size(); ++slot)
        {
            if (used[(std::size_t) slot] == 0)
                continue;

            const auto& array = arrays[slot];

            source += indent + "const " + typeName(array.elementType) + " a"
                      + std::to_string(slot) + "["
                      + std::to_string(array.elements.size()) + "] = {";

            for (auto i = 0; i < array.elements.size(); ++i)
            {
                if (i > 0)
                    source += ", ";

                source += printer.ref(array.elements[i]);
            }

            source += "};\n";
        }

        return source;
    }

    std::string emitBlock(int block, const std::string& indent)
    {
        auto uses = blockUses(block);
        auto open = std::vector<int> {};
        auto source = std::string {};

        for (auto index: graph().block(block).statements)
            source += emitStatement(graph().statement(index), indent, uses, open);

        retire(open);
        return source;
    }

    std::string emitStatement(const Statement& statement,
                              const std::string& indent,
                              const std::vector<int>& uses,
                              std::vector<int>& open)
    {
        auto inner = indent + "    ";

        if (statement.kind == StatementKind::Loop)
        {
            retire(open);

            return indent + "while (" + printer.ref(statement.value) + ")\n" + indent
                   + "{\n" + emitBlock(statement.body, inner) + indent + "}\n";
        }

        // An if is emitted only after the names its bodies invalidate are given
        // up, so nothing inside stands for a value one of them has moved on
        // from. An assignment needs no such pass first: its right-hand side is
        // what the variable held before it, which is exactly what the open
        // names still stand for.
        if (statement.kind == StatementKind::If)
            dropStale(statement, open);

        auto source = std::string {};

        switch (statement.kind)
        {
            case StatementKind::Declare:
            case StatementKind::Assign:
            {
                auto declares = statement.kind == StatementKind::Declare;
                auto type =
                    declares
                        ? std::string(typeName(graph().variables()[statement.slot]))
                              + " "
                        : std::string {};

                source = define({statement.value}, indent, uses, open);
                source += indent + type + "v" + std::to_string(statement.slot)
                          + " = " + printer.ref(statement.value) + ";\n";
                break;
            }

            case StatementKind::If:
            {
                source = define({statement.value}, indent, uses, open);
                source += indent + "if (" + printer.ref(statement.value) + ")\n"
                          + indent + "{\n" + emitBlock(statement.body, inner)
                          + indent + "}\n";

                if (statement.elseBody >= 0)
                    source += indent + "else\n" + indent + "{\n"
                              + emitBlock(statement.elseBody, inner) + indent
                              + "}\n";

                break;
            }

            case StatementKind::Break:
                source = indent + "break;\n";
                break;

            case StatementKind::Continue:
                source = indent + "continue;\n";
                break;

            case StatementKind::Loop:
                break;
        }

        // Afterwards either way, for the names this statement's own expressions
        // introduced: a value read out of the variable it then wrote.
        dropStale(statement, open);
        return source;
    }

private:
    std::vector<int> countUsesOver(const std::vector<int>& roots) const
    {
        auto count = (std::size_t) graph().nodeCount();
        auto uses = std::vector<int>(count, 0);
        auto seen = std::vector<char>(count, 0);

        for (auto root: roots)
            countUses(graph(), root, uses, seen);

        return uses;
    }

    // How often the statements of one block reach each node - the block's own
    // statements only, since a nested body counts its own when it is emitted.
    // A loop's condition is left out deliberately: see the note above.
    std::vector<int> blockUses(int block) const
    {
        auto roots = std::vector<int> {};

        for (auto index: graph().block(block).statements)
        {
            const auto& statement = graph().statement(index);

            if (statement.kind != StatementKind::Loop)
                roots.push_back(statement.value);
        }

        return countUsesOver(roots);
    }

    std::string define(const std::vector<int>& roots,
                       const std::string& indent,
                       const std::vector<int>& uses,
                       std::vector<int>& open)
    {
        auto count = (std::size_t) graph().nodeCount();
        auto ordered = std::vector<char>(count, 0);
        auto order = std::vector<int> {};

        for (auto root: roots)
            orderLocals(graph(), root, uses, locals, ordered, order);

        auto source = std::string {};

        for (auto node: order)
        {
            locals[node] = localCount++;
            open.push_back(node);

            source += indent + std::string(typeName(graph().expr(node).type)) + " t"
                      + std::to_string(locals[node]) + " = " + printer.print(node)
                      + ";\n";
        }

        return source;
    }

    void retire(std::vector<int>& open)
    {
        for (auto node: open)
            locals[node] = -1;

        open.clear();
    }

    void dropStale(const Statement& statement, std::vector<int>& open)
    {
        if (open.empty() || graph().variables().empty())
            return;

        auto written =
            std::vector<char>((std::size_t) graph().variables().size(), 0);
        collectWrites(graph(), statement, written);

        auto kept = std::vector<int> {};

        for (auto node: open)
        {
            auto seen = std::vector<char>((std::size_t) graph().nodeCount(), 0);

            if (readsAny(graph(), node, written, seen))
                locals[node] = -1;
            else
                kept.push_back(node);
        }

        open = std::move(kept);
    }

public:
    std::vector<int> locals; // node id -> local index, -1 = inline
    ExprPrinter printer;
    int localCount = 0;
};

// Whether the expression tree under node reads a uniform. A Varying read is the
// fragment-stage boundary: its vertex-stage source tree is walked separately as
// part of the vertex stage, so the walk stops there.
bool referencesUniform(const ShaderGraph& graph, int node)
{
    if (node < 0)
        return false;

    const auto& expr = graph.expr(node);

    if (expr.kind == ExprKind::Uniform)
        return true;

    if (expr.kind == ExprKind::Varying)
        return false;

    for (auto argument: expr.args)
        if (referencesUniform(graph, argument))
            return true;

    return false;
}

// Every expression a block's statements evaluate, gathered so a stage sees what
// its statements read and not only what its output expression does. Without
// this a uniform read only from inside a loop would go undeclared: the
// expression walk starts at the fragment colour and never reaches it.
void collectStatementRoots(const ShaderGraph& graph,
                           int block,
                           std::vector<int>& roots)
{
    for (auto index: graph.block(block).statements)
    {
        const auto& statement = graph.statement(index);
        roots.push_back(statement.value);

        if (statement.body >= 0)
            collectStatementRoots(graph, statement.body, roots);

        if (statement.elseBody >= 0)
            collectStatementRoots(graph, statement.elseBody, roots);
    }
}

bool vertexUsesUniforms(const ShaderGraph& graph)
{
    if (referencesUniform(graph, graph.position()))
        return true;

    for (const auto& varying: graph.varyings())
        if (referencesUniform(graph, varying.sourceNode))
            return true;

    return false;
}

// The Uniforms struct shared by both stages (and the HLSL cbuffer wrapping it).
// The CPU block is packed with MSL struct alignment (UniformLayout.h); HLSL
// cbuffer packing only forbids straddling a 16-byte register, so a vector after
// a scalar would land lower than the CPU wrote it - explicit pad scalars are
// emitted wherever the two rule sets disagree.
std::string uniformBlock(Backend backend,
                         const Vector<ValueType>& types,
                         const Vector<std::string>& names)
{
    auto source = std::string {"struct Uniforms\n{\n"};

    auto offsets = uniformOffsets(types);
    auto hlslCursor = 0;
    auto padCount = 0;

    for (auto i = 0; i < types.size(); ++i)
    {
        auto type = types[i];

        if (backend == Backend::DirectX)
        {
            while (hlslPackedOffset(hlslCursor, type) < offsets[i])
            {
                source += "    float pad" + std::to_string(padCount++) + ";\n";
                hlslCursor += 4;
            }

            hlslCursor = offsets[i] + byteSize(type);
        }

        source += "    " + std::string(typeName(type)) + " " + names[i] + ";\n";
    }

    source += "};\n\n";

    if (backend == Backend::DirectX)
        source += "cbuffer UniformsCB : register(b0)\n{\n"
                  "    Uniforms uniforms;\n};\n\n";

    return source;
}

// Compute kernel emission. The expression printer is the render one; only the
// scaffolding differs: storage buffers and the uniform block are MSL kernel
// parameters but HLSL globals, and the 1D work-item id arrives as a uint on
// Metal and as SV_DispatchThreadID.x on D3D. The block always ends with an
// implicit uint element count, and the kernel opens with the bounds guard the
// rounded-up dispatch needs; ComputeProgram appends the matching CPU value.
std::string emitCompute(const ShaderGraph& graph, Backend backend)
{
    auto source = std::string {};

    if (backend == Backend::Metal)
        source += "#include <metal_stdlib>\nusing namespace metal;\n\n";

    auto uniformTypes = graph.uniforms();
    auto uniformNames = Vector<std::string> {};

    for (auto i = 0; i < uniformTypes.size(); ++i)
        uniformNames.add("u" + std::to_string(i));

    uniformTypes.add(ValueType::UInt);
    uniformNames.add("count");

    source += uniformBlock(backend, uniformTypes, uniformNames);

    const auto& buffers = graph.storageBuffers();

    if (backend == Backend::Metal)
    {
        source += "kernel void computeMain(";

        for (auto i = 0; i < buffers.size(); ++i)
        {
            auto writable = buffers[i] == BufferAccess::Write;
            source += std::string(writable ? "device float* buffer"
                                           : "device const float* buffer")
                      + std::to_string(i) + " [[buffer(" + std::to_string(i)
                      + ")]],\n    ";
        }

        source += "constant Uniforms& uniforms [[buffer("
                  + std::to_string(ComputePass::uniformBase) + ")]],\n    ";
        source += "uint gid [[thread_position_in_grid]])\n{\n";
    }
    else
    {
        // SRV t<slot> / UAV u<slot> with one shared slot counter, matching the
        // flat Metal indices ComputePass binds both backends with.
        for (auto i = 0; i < buffers.size(); ++i)
        {
            auto slot = std::to_string(i);
            auto writable = buffers[i] == BufferAccess::Write;

            source += writable ? "RWStructuredBuffer<float> buffer"
                               : "StructuredBuffer<float> buffer";
            source += slot;
            source += writable ? " : register(u" : " : register(t";
            source += slot + ");\n";
        }

        if (buffers.size() > 0)
            source += "\n";

        source += "[numthreads(" + std::to_string(ComputePass::threadGroupWidth)
                  + ", 1, 1)]\n";
        source += "void computeMain(uint3 threadId : SV_DispatchThreadID)\n{\n";
        source += "    uint gid = threadId.x;\n";
    }

    source += "    if (gid >= uniforms.count)\n        return;\n";

    auto roots = std::vector<int> {};

    for (const auto& store: graph.stores())
    {
        roots.push_back(store.index);
        roots.push_back(store.value);
    }

    auto stageRoots = roots;
    collectStatementRoots(graph, ShaderGraph::rootBlock, stageRoots);

    auto stage = StageEmitter {graph, backend};

    source += stage.declareArrays(stageRoots, "    ");
    source += stage.emitBlock(ShaderGraph::rootBlock, "    ");
    source += stage.defineFor(roots, "    ");

    for (const auto& store: graph.stores())
        source += "    buffer" + std::to_string(store.slot) + "["
                  + stage.printer.ref(store.index)
                  + "] = " + stage.printer.ref(store.value) + ";\n";

    source += "}\n";
    return source;
}

std::string emit(const ShaderGraph& graph, Backend backend)
{
    if (graph.isCompute())
        return emitCompute(graph, backend);

    auto source = std::string {};

    if (backend == Backend::Metal)
        source += "#include <metal_stdlib>\nusing namespace metal;\n\n";

    source += "struct VertexIn\n{\n";

    for (auto i = 0; i < graph.inputs().size(); ++i)
        source += "    " + std::string(typeName(graph.inputs()[i])) + " a"
                  + std::to_string(i) + attributeSemantic(backend, i) + ";\n";

    source += "};\n\nstruct VertexOut\n{\n";
    source += "    float4 position" + positionSemantic(backend) + ";\n";

    for (auto i = 0; i < graph.varyings().size(); ++i)
        source += "    " + std::string(typeName(graph.varyings()[i].type)) + " v"
                  + std::to_string(i) + varyingSemantic(backend, i) + ";\n";

    source += "};\n\n";

    auto hasUniforms = !graph.uniforms().empty();

    // One uniform block aggregates every uniform<>() call. Both backends expose
    // it as "uniforms.uN" (HLSL wraps the struct in a cbuffer) so the expression
    // printer stays backend-agnostic.
    if (hasUniforms)
    {
        auto names = Vector<std::string> {};

        for (auto i = 0; i < graph.uniforms().size(); ++i)
            names.add("u" + std::to_string(i));

        source += uniformBlock(backend, graph.uniforms(), names);
    }

    // HLSL textures and samplers are globals; on Metal they are fragment
    // function parameters added to the signature below. Texture and sampler
    // share an index, matching RenderPass::setFragmentTexture.
    if (backend == Backend::DirectX)
    {
        // The texture lands on t<slot>, but its sampler lands on a register
        // chosen by how the shader declared the texture should be sampled: the
        // root signature declares a static sampler for every (slot,
        // configuration) pair, so picking the register picks the sampler. See
        // TextureSampling for why samplers cannot come from a descriptor table.
        for (auto i = 0; i < graph.textureCount(); ++i)
        {
            auto samplerRegister =
                i * samplingConfigurations + samplingIndex(graph.textureSampling(i));

            source += "Texture2D texture" + std::to_string(i) + " : register(t"
                      + std::to_string(i) + ");\nSamplerState sampler"
                      + std::to_string(i) + " : register(s"
                      + std::to_string(samplerRegister) + ");\n";
        }

        if (graph.textureCount() > 0)
            source += "\n";
    }

    // On Metal each stage declares the uniform block as a function parameter,
    // and only when that stage's expressions read one; the HLSL cbuffer is a
    // global both functions already see. Slot 0 maps to buffer(uniformBase)
    // in both stages, matching what RenderPass::setVertexBytes /
    // setFragmentBytes bind. Uniforms live at buffer(uniformBase..) so a
    // vertex layout with multiple per-instance slots (0..N) never collides
    // with them.
    if (backend == Backend::Metal)
    {
        source += "vertex VertexOut vertexMain(VertexIn input [[stage_in]]";

        if (hasUniforms && vertexUsesUniforms(graph))
            source += ", constant Uniforms& uniforms [[buffer("
                      + std::to_string(RenderPass::uniformBase) + ")]]";

        source += ")\n{\n";
    }
    else
    {
        source += "VertexOut vertexMain(VertexIn input)\n{\n";
    }

    auto vertexRoots = std::vector<int> {graph.position()};

    for (auto i = 0; i < graph.varyings().size(); ++i)
        vertexRoots.push_back(graph.varyings()[i].sourceNode);

    // The vertex stage takes no statements: a mutable local and the control flow
    // driving it belong to the fragment expression, the way sampling does. See
    // ShaderBuilder::var.
    auto vertexStage = StageEmitter {graph, backend};

    source += vertexStage.declareArrays(vertexRoots, "    ");
    source += vertexStage.defineFor(vertexRoots, "    ");
    source += "    VertexOut output;\n";
    source +=
        "    output.position = " + vertexStage.printer.ref(graph.position()) + ";\n";

    for (auto i = 0; i < graph.varyings().size(); ++i)
        source += "    output.v" + std::to_string(i) + " = "
                  + vertexStage.printer.ref(graph.varyings()[i].sourceNode) + ";\n";

    source += "    return output;\n}\n\n";

    // The alpha test is a fragment root like the colour is: its subtree is
    // planned with the colour's, so a value both of them read (the texture
    // sample, typically) is computed once and shared.
    auto fragmentRoots = std::vector {graph.fragment()};

    if (graph.discard() >= 0)
        fragmentRoots.push_back(graph.discard());

    // What the statements read counts towards the stage's uniform declaration,
    // but not towards the colour's locals: a statement's own expressions are
    // named where that statement is emitted, above.
    auto stageRoots = fragmentRoots;
    collectStatementRoots(graph, ShaderGraph::rootBlock, stageRoots);

    auto fragmentReadsUniform = false;

    for (auto root: stageRoots)
        fragmentReadsUniform |= referencesUniform(graph, root);

    if (backend == Backend::Metal)
    {
        source += "fragment float4 fragmentMain(VertexOut input [[stage_in]]";

        if (hasUniforms && fragmentReadsUniform)
            source += ",\n    constant Uniforms& uniforms [[buffer("
                      + std::to_string(RenderPass::uniformBase) + ")]]";

        for (auto i = 0; i < graph.textureCount(); ++i)
            source += ",\n    texture2d<float> texture" + std::to_string(i)
                      + " [[texture(" + std::to_string(i)
                      + ")]],\n    sampler sampler" + std::to_string(i)
                      + " [[sampler(" + std::to_string(i) + ")]]";

        source += ")\n{\n";
    }
    else
    {
        source += "float4 fragmentMain(VertexOut input) : SV_Target\n{\n";
    }

    // The shader's statements run first - they are what the fragment expression
    // then reads a mutable local out of - and the colour is planned after them.
    auto fragmentStage = StageEmitter {graph, backend};

    source += fragmentStage.declareArrays(stageRoots, "    ");
    source += fragmentStage.emitBlock(ShaderGraph::rootBlock, "    ");
    source += fragmentStage.defineFor(fragmentRoots, "    ");

    if (graph.discard() >= 0)
    {
        auto kill = backend == Backend::Metal ? "discard_fragment();" : "discard;";

        source += "    if (" + fragmentStage.printer.ref(graph.discard()) + " < "
                  + floatLiteral(graph.discardThreshold()) + ")\n        " + kill
                  + "\n";
    }

    source += "    return " + fragmentStage.printer.ref(graph.fragment()) + ";\n}\n";

    return source;
}
} // namespace

std::string emitMetal(const ShaderGraph& graph)
{
    return emit(graph, Backend::Metal);
}

std::string emitHlsl(const ShaderGraph& graph)
{
    return emit(graph, Backend::DirectX);
}
} // namespace eacp::GPU
