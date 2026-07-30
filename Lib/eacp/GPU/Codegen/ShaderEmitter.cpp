#include "ShaderEmitter.h"

#include "../Frame/ComputePass.h"
#include "../Frame/RenderPass.h"
#include "ShaderGraph.h"
#include "UniformLayout.h"

#include <cassert>
#include <cstdio>

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
                // A matrix product, in the order it was written. MSL spells it
                // with the * operator; HLSL multiplies a matrix by anything
                // with mul(). Both read a vector on the left of one as a row
                // and one on the right as a column, so the order is the whole
                // of what tells the three products apart.
                auto left = ref(expr.args[0]);
                auto right = ref(expr.args[1]);

                if (backend == Backend::Metal)
                    return "(" + left + " * " + right + ")";

                return "mul(" + left + ", " + right + ")";
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
                // unsigned, so anything not already signed-integer goes through
                // int2 first: a negative coordinate then wraps to a large
                // unsigned one and reads as zero, which is what HLSL's Load does
                // with it directly. The level is 0 - GPU::Texture has no mips -
                // and D3D carries it in the coordinate's third component.
                auto name = "texture" + std::to_string(expr.index);
                auto given = ref(expr.args[0]);
                auto coordinates = graph.expr(expr.args[0]).type == ValueType::Int2
                                       ? given
                                       : "int2(" + given + ")";

                if (backend == Backend::Metal)
                    return name + ".read(uint2(" + coordinates + "))";

                return name + ".Load(int3(" + coordinates + ", 0))";
            }

            case ExprKind::ThreadId:
                // Both kernel scaffoldings declare the work-item id as gid: a
                // uint over the flat count in a 1D kernel, a uint2 over the
                // grid in a 2D one, where the node carries which component it
                // asked for.
                if (graph.dispatchRank() == DispatchRank::OneD)
                    return "gid";

                return expr.index == 0 ? "gid.x" : "gid.y";

            case ExprKind::BufferRead:
                return "buffer" + std::to_string(expr.index) + "["
                       + ref(expr.args[0]) + "]";

            case ExprKind::AtomicLoad:
            {
                // HLSL has nothing to spell: a UAV element of an
                // RWStructuredBuffer<uint> is already the thing an interlocked
                // operation acts on, and reading one is a subscript. MSL wraps
                // its atomic_uint, so the value has to be taken out of it.
                auto element = "buffer" + std::to_string(expr.index) + "["
                               + ref(expr.args[0]) + "]";

                if (backend == Backend::Metal)
                    return "atomic_load_explicit(&" + element
                           + ", memory_order_relaxed)";

                return element;
            }

            case ExprKind::ArrayRead:
                return "a" + std::to_string(expr.index) + "[" + ref(expr.args[0])
                       + "]";

            // The threadgroup indices ride the same scaffolding as gid: both
            // backends' entry points bind them to these names, a scalar in a
            // 1D kernel and a pair in a 2D one.
            case ExprKind::LocalId:
                if (graph.dispatchRank() == DispatchRank::OneD)
                    return "lid";

                return expr.index == 0 ? "lid.x" : "lid.y";

            case ExprKind::GroupId:
                if (graph.dispatchRank() == DispatchRank::OneD)
                    return "tgid";

                return expr.index == 0 ? "tgid.x" : "tgid.y";

            // The implicit bound the dispatch appended to the uniform block,
            // under the names the block declares it with.
            case ExprKind::GridExtent:
                if (graph.dispatchRank() == DispatchRank::OneD)
                    return "uniforms.count";

                return expr.index == 0 ? "uniforms.width" : "uniforms.height";

            case ExprKind::SharedRead:
                return "s" + std::to_string(expr.index) + "[" + ref(expr.args[0])
                       + "]";
        }

        return {};
    }

    const ShaderGraph& graph;
    Backend backend;
    const Vector<int>& locals; // node id -> local index, -1 = inline
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
        case ExprKind::AtomicLoad:
        case ExprKind::ArrayRead:
        case ExprKind::SharedRead:
            return true;

        case ExprKind::Input:
        case ExprKind::Varying:
        case ExprKind::Uniform:
        case ExprKind::Constant:
        case ExprKind::Swizzle:
        case ExprKind::VarRead:
        case ExprKind::ThreadId:
        case ExprKind::LocalId:
        case ExprKind::GroupId:
        case ExprKind::GridExtent:
            return false;
    }

    return false;
}

// Counts how many references each node receives across the stage's roots: one
// per root plus one per parent edge, visiting each node's children only once.
void countUses(const ShaderGraph& graph,
               int node,
               Vector<int>& uses,
               Vector<char>& seen)
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
                 const Vector<int>& uses,
                 const Vector<int>& locals,
                 Vector<char>& seen,
                 Vector<int>& order)
{
    if (node < 0 || seen[node] || locals[node] >= 0)
        return;

    seen[node] = 1;

    for (auto argument: graph.expr(node).args)
        orderLocals(graph, argument, uses, locals, seen, order);

    if (uses[node] > 1 && wantsLocal(graph.expr(node).kind))
        order.add(node);
}

// Which constant arrays a run of expressions subscripts, following the elements
// of one that is used in case an element subscripts another.
void collectArrays(const ShaderGraph& graph,
                   int node,
                   Vector<char>& used,
                   Vector<char>& seen)
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
void collectWrites(const ShaderGraph& graph, int block, Vector<char>& written);

void collectWrites(const ShaderGraph& graph,
                   const Statement& statement,
                   Vector<char>& written)
{
    switch (statement.kind)
    {
        case StatementKind::Declare:
        case StatementKind::Assign:
        case StatementKind::AtomicAdd:
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
        case StatementKind::Store:
        case StatementKind::TextureStore:
        case StatementKind::SharedStore:
        case StatementKind::Barrier:
            return;
    }
}

// Whether running a statement can change what threadgroup memory holds: a
// store to it, or the barrier that publishes what other threads stored -
// following nested bodies the way collectWrites does. What this feeds is the
// same rule variables get: a name computed from shared memory is given up the
// moment shared memory may have moved on.
bool touchesShared(const ShaderGraph& graph, int block);

bool touchesShared(const ShaderGraph& graph, const Statement& statement)
{
    switch (statement.kind)
    {
        case StatementKind::SharedStore:
        case StatementKind::Barrier:
            return true;

        case StatementKind::If:
            if (touchesShared(graph, statement.body))
                return true;

            return statement.elseBody >= 0
                   && touchesShared(graph, statement.elseBody);

        case StatementKind::Loop:
            return touchesShared(graph, statement.body);

        case StatementKind::Declare:
        case StatementKind::Assign:
        case StatementKind::Break:
        case StatementKind::Continue:
        case StatementKind::Store:
        case StatementKind::TextureStore:
        case StatementKind::AtomicAdd:
            return false;
    }

    return false;
}

bool touchesShared(const ShaderGraph& graph, int block)
{
    for (auto index: graph.block(block).statements)
        if (touchesShared(graph, graph.statement(index)))
            return true;

    return false;
}

void collectWrites(const ShaderGraph& graph, int block, Vector<char>& written)
{
    for (auto index: graph.block(block).statements)
        collectWrites(graph, graph.statement(index), written);
}

// A visited set a walk can have a fresh one of without paying for one. Marking
// is a stamp rather than a flag, so starting over is a counter increment
// instead of clearing a buffer the size of the graph.
//
// It exists because the walk below runs per open name per statement, and a
// buffer allocated and zeroed each time costs the whole graph however small the
// subtree walked turns out to be. On an ordinary shader that is invisible; on a
// large one it is the difference between a shader that compiles and an app that
// hangs.
struct VisitSet
{
    explicit VisitSet(int nodeCount) { stamps.resize(nodeCount, 0); }

    void restart() { ++generation; }

    bool visit(int node)
    {
        if (stamps[node] == generation)
            return false;

        stamps[node] = generation;
        return true;
    }

    Vector<int> stamps;

    // Ahead of the stamps a fresh buffer holds, so nothing counts as visited
    // until something visits it. Starting level with them makes every node of a
    // new set look already seen - which is not a walk that gives the wrong
    // answer slowly, it is one that gives it immediately.
    int generation = 1;
};

// Whether the value under node no longer stands for itself after a statement:
// it read a variable that statement wrote, or it read threadgroup memory and
// the statement may have moved what that holds.
bool readsStale(const ShaderGraph& graph,
                int node,
                const Vector<char>& written,
                bool sharedMoved,
                VisitSet& seen)
{
    if (node < 0 || !seen.visit(node))
        return false;

    const auto& expr = graph.expr(node);

    if (expr.kind == ExprKind::VarRead && written[expr.index] != 0)
        return true;

    if (sharedMoved && expr.kind == ExprKind::SharedRead)
        return true;

    for (auto argument: expr.args)
        if (readsStale(graph, argument, written, sharedMoved, seen))
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
        : printer {graphToUse, backend, locals}
        , visited(graphToUse.nodeCount())
    {
        locals.resize(graphToUse.nodeCount(), -1);
    }

    const ShaderGraph& graph() const { return printer.graph; }

    // The locals a standalone run of expressions needs - a stage's outputs,
    // which no statement follows - counted over just those expressions.
    std::string defineFor(const Vector<int>& roots, const std::string& indent)
    {
        auto open = Vector<int> {};
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
    std::string declareArrays(const Vector<int>& roots, const std::string& indent)
    {
        const auto& arrays = graph().arrays();

        if (arrays.empty())
            return {};

        auto used = Vector<char> {};
        used.resize(arrays.size(), 0);
        auto seen = Vector<char> {};
        seen.resize(graph().nodeCount(), 0);

        for (auto root: roots)
            collectArrays(graph(), root, used, seen);

        auto source = std::string {};

        for (auto slot = 0; slot < arrays.size(); ++slot)
        {
            if (used[slot] == 0)
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
        auto open = Vector<int> {};
        auto source = std::string {};

        for (auto index: graph().block(block).statements)
            source += emitStatement(graph().statement(index), indent, uses, open);

        retire(open);
        return source;
    }

    std::string emitStatement(const Statement& statement,
                              const std::string& indent,
                              const Vector<int>& uses,
                              Vector<int>& open)
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

            case StatementKind::Store:
            {
                source =
                    define({statement.index, statement.value}, indent, uses, open);

                auto element = "buffer" + std::to_string(statement.slot) + "["
                               + printer.ref(statement.index) + "]";
                auto stored = printer.ref(statement.value);

                // An atomic buffer's element is an atomic_uint on Metal and has
                // to be stored through rather than assigned. HLSL's UAV element
                // is an ordinary uint, so the plain assignment is already right
                // there.
                auto atomic =
                    graph().storageBuffers()[statement.slot] == BufferAccess::Atomic;

                if (atomic && printer.backend == Backend::Metal)
                    source += indent + "atomic_store_explicit(&" + element + ", "
                              + stored + ", memory_order_relaxed);\n";
                else
                    source += indent + element + " = " + stored + ";\n";

                break;
            }

            case StatementKind::AtomicAdd:
            {
                source =
                    define({statement.index, statement.value}, indent, uses, open);

                auto name = "v" + std::to_string(statement.slot);
                auto element = "buffer" + std::to_string(statement.bufferSlot) + "["
                               + printer.ref(statement.index) + "]";
                auto addend = printer.ref(statement.value);

                if (printer.backend == Backend::Metal)
                {
                    source += indent + "uint " + name
                              + " = atomic_fetch_add_explicit(&" + element + ", "
                              + addend + ", memory_order_relaxed);\n";
                    break;
                }

                // Two lines here rather than one: InterlockedAdd hands the old
                // value back through an out parameter, so the name has to exist
                // before the call that fills it.
                source += indent + "uint " + name + ";\n";
                source += indent + "InterlockedAdd(" + element + ", " + addend + ", "
                          + name + ");\n";
                break;
            }

            case StatementKind::SharedStore:
                source =
                    define({statement.index, statement.value}, indent, uses, open);
                source += indent + "s" + std::to_string(statement.slot) + "["
                          + printer.ref(statement.index)
                          + "] = " + printer.ref(statement.value) + ";\n";
                break;

            // The synchronisation point itself. The names it invalidates -
            // anything computed from shared memory - are given up by the
            // dropStale below, the same pass an assignment retires its
            // variable's readers through.
            case StatementKind::Barrier:
                source =
                    indent
                    + (printer.backend == Backend::Metal
                           ? "threadgroup_barrier(mem_flags::mem_threadgroup);\n"
                           : "GroupMemoryBarrierWithGroupSync();\n");
                break;

            // The one place the two languages spell a write differently: MSL
            // takes the colour first and the coordinate second, HLSL
            // subscripts the texture like an array.
            case StatementKind::TextureStore:
            {
                source = define({statement.index, statement.indexY, statement.value},
                                indent,
                                uses,
                                open);

                auto name = "texture" + std::to_string(statement.slot);
                auto coordinates = "uint2(" + printer.ref(statement.index) + ", "
                                   + printer.ref(statement.indexY) + ")";
                auto color = printer.ref(statement.value);

                if (printer.backend == Backend::Metal)
                    source += indent + name + ".write(" + color + ", " + coordinates
                              + ");\n";
                else
                    source +=
                        indent + name + "[" + coordinates + "] = " + color + ";\n";

                break;
            }

            case StatementKind::Loop:
                break;
        }

        // Afterwards either way, for the names this statement's own expressions
        // introduced: a value read out of the variable it then wrote.
        dropStale(statement, open);
        return source;
    }

private:
    Vector<int> countUsesOver(const Vector<int>& roots) const
    {
        auto count = graph().nodeCount();
        auto uses = Vector<int> {};
        auto seen = Vector<char> {};

        uses.resize(count, 0);
        seen.resize(count, 0);

        for (auto root: roots)
            countUses(graph(), root, uses, seen);

        return uses;
    }

    // How often the statements of one block reach each node - the block's own
    // statements only, since a nested body counts its own when it is emitted.
    // A loop's condition is left out deliberately: see the note above.
    Vector<int> blockUses(int block) const
    {
        auto roots = Vector<int> {};

        for (auto index: graph().block(block).statements)
        {
            const auto& statement = graph().statement(index);

            if (statement.kind != StatementKind::Loop)
            {
                roots.add(statement.value);
                roots.add(statement.index);
                roots.add(statement.indexY);
            }
        }

        return countUsesOver(roots);
    }

    std::string define(const Vector<int>& roots,
                       const std::string& indent,
                       const Vector<int>& uses,
                       Vector<int>& open)
    {
        auto count = graph().nodeCount();
        auto ordered = Vector<char> {};
        ordered.resize(count, 0);
        auto order = Vector<int> {};

        for (auto root: roots)
            orderLocals(graph(), root, uses, locals, ordered, order);

        auto source = std::string {};

        for (auto node: order)
        {
            locals[node] = localCount++;
            open.add(node);

            source += indent + std::string(typeName(graph().expr(node).type)) + " t"
                      + std::to_string(locals[node]) + " = " + printer.print(node)
                      + ";\n";
        }

        return source;
    }

    void retire(Vector<int>& open)
    {
        for (auto node: open)
            locals[node] = -1;

        open.clear();
    }

    void dropStale(const Statement& statement, Vector<int>& open)
    {
        if (open.empty())
            return;

        auto sharedMoved = touchesShared(graph(), statement);

        written.assign(graph().variables().size(), 0);
        collectWrites(graph(), statement, written);

        // A statement that leaves no variable holding something else and
        // moves no shared memory cannot have staled a name, and most do not:
        // a break, a continue, and an if whose bodies only compute. Asking
        // each open name about an empty set is the same walk for a
        // guaranteed no.
        if (!written.contains(1) && !sharedMoved)
            return;

        auto kept = Vector<int> {};

        for (auto node: open)
        {
            visited.restart();

            if (readsStale(graph(), node, written, sharedMoved, visited))
                locals[node] = -1;
            else
                kept.add(node);
        }

        open = std::move(kept);
    }

public:
    Vector<int> locals; // node id -> local index, -1 = inline
    ExprPrinter printer;
    int localCount = 0;

private:
    // Held by the emitter rather than by the walk, so that naming a stage costs
    // one buffer instead of one per name per statement.
    VisitSet visited;
    Vector<char> written;
};

// Whether the expression tree under node reads a uniform. A Varying read is the
// fragment-stage boundary: its vertex-stage source tree is walked separately as
// part of the vertex stage, so the walk stops there.
//
// The visited set is not an optimisation here, it is what makes the walk
// finite in practice. What this walks is a graph rather than a tree - the
// emitter's whole reason for existing is that a shared subtree is stored once -
// and a walk that revisits a shared node once per path through it is
// exponential in the sharing, not quadratic. It went unnoticed for as long as
// every shader was small enough that the exponent did not matter.
//
// A node reached twice is a node whose answer is already in the result: either
// the walk that reached it first found a uniform, in which case it returned
// true and this one is unreachable, or it did not, in which case there is none
// under there to find.
bool referencesUniform(const ShaderGraph& graph, int node, VisitSet& seen)
{
    if (node < 0 || !seen.visit(node))
        return false;

    const auto& expr = graph.expr(node);

    if (expr.kind == ExprKind::Uniform)
        return true;

    if (expr.kind == ExprKind::Varying)
        return false;

    for (auto argument: expr.args)
        if (referencesUniform(graph, argument, seen))
            return true;

    return false;
}

// Every expression a block's statements evaluate, gathered so a stage sees what
// its statements read and not only what its output expression does. Without
// this a uniform read only from inside a loop would go undeclared: the
// expression walk starts at the fragment colour and never reaches it.
void collectStatementRoots(const ShaderGraph& graph, int block, Vector<int>& roots)
{
    for (auto index: graph.block(block).statements)
    {
        const auto& statement = graph.statement(index);
        roots.add(statement.value);
        roots.add(statement.index);
        roots.add(statement.indexY);

        if (statement.body >= 0)
            collectStatementRoots(graph, statement.body, roots);

        if (statement.elseBody >= 0)
            collectStatementRoots(graph, statement.elseBody, roots);
    }
}

// One visited set across every root, not one per root: a stage's roots share
// most of their graph, and a node already known to hold no uniform holds none
// whichever root reached it.
bool anyReferencesUniform(const ShaderGraph& graph, const Vector<int>& roots)
{
    auto seen = VisitSet {graph.nodeCount()};

    for (auto root: roots)
        if (referencesUniform(graph, root, seen))
            return true;

    return false;
}

// Every expression the vertex stage evaluates: the clip position and each
// varying it hands the fragment stage.
Vector<int> vertexStageRoots(const ShaderGraph& graph)
{
    auto roots = Vector<int> {};
    roots.add(graph.position());

    for (const auto& varying: graph.varyings())
        roots.add(varying.sourceNode);

    return roots;
}

// Its fragment sibling: the colour, the alpha test when there is one, and what
// the statements evaluate. Wider than the roots the colour's locals are planned
// from, which is why emit() keeps both - a value a statement reads is declared
// by the stage but named where the statement is emitted.
Vector<int> fragmentStageRoots(const ShaderGraph& graph)
{
    auto roots = Vector<int> {graph.fragment()};

    if (graph.discard() >= 0)
        roots.add(graph.discard());

    collectStatementRoots(graph, ShaderGraph::rootBlock, roots);
    return roots;
}

// Which storage-buffer slots a run of expressions subscripts. A render stage
// declares only the buffers it reads - unlike a kernel, where every slot is a
// parameter of the one entry point - so the vertex and fragment functions each
// need their own answer.
void collectBufferSlots(const ShaderGraph& graph,
                        int node,
                        Vector<char>& used,
                        VisitSet& seen)
{
    if (node < 0 || !seen.visit(node))
        return;

    const auto& expr = graph.expr(node);

    if (expr.kind == ExprKind::BufferRead && expr.index < used.size())
        used[expr.index] = 1;

    for (auto argument: expr.args)
        collectBufferSlots(graph, argument, used, seen);
}

Vector<char> bufferSlotsUsedBy(const ShaderGraph& graph, const Vector<int>& roots)
{
    auto used = Vector<char> {};
    used.resize(graph.storageBuffers().size(), 0);

    auto seen = VisitSet {graph.nodeCount()};

    for (auto root: roots)
        collectBufferSlots(graph, root, used, seen);

    return used;
}

// The MSL parameters for the storage buffers a render stage reads, always read
// only: a vertex or fragment function has no writable buffer here, which is the
// whole of what separates this from the kernel signature above.
std::string bufferParameters(const ShaderGraph& graph, const Vector<int>& roots)
{
    auto used = bufferSlotsUsedBy(graph, roots);
    auto source = std::string {};

    for (auto i = 0; i < used.size(); ++i)
        if (used[i] != 0)
            source += ",\n    device const float* buffer" + std::to_string(i)
                      + " [[buffer(" + std::to_string(RenderPass::bufferBase + i)
                      + ")]]";

    return source;
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

// How each access spells the buffer it declares. An atomic one is the only kind
// whose *elements* differ - unsigned integers, wrapped in the type the language
// requires an interlocked operation to act through - so it cannot be a
// qualifier on the float declaration the other two share.
const char* metalBufferType(BufferAccess access)
{
    switch (access)
    {
        case BufferAccess::Read:
            return "device const float*";
        case BufferAccess::Write:
            return "device float*";
        case BufferAccess::Atomic:
            return "device atomic_uint*";
    }

    return "device const float*";
}

const char* hlslBufferType(BufferAccess access)
{
    switch (access)
    {
        case BufferAccess::Read:
            return "StructuredBuffer<float>";
        case BufferAccess::Write:
            return "RWStructuredBuffer<float>";
        case BufferAccess::Atomic:
            return "RWStructuredBuffer<uint>";
    }

    return "StructuredBuffer<float>";
}

// Compute kernel emission. The expression printer is the render one; only the
// scaffolding differs: storage buffers and the uniform block are MSL kernel
// parameters but HLSL globals, and the work-item id arrives as a builtin
// parameter on Metal and as SV_DispatchThreadID on D3D. The block always ends
// with the implicit grid extents the bounds guard reads - one count for a 1D
// kernel, a width and a height for a 2D one - and the kernel opens with the
// guard the rounded-up dispatch needs; ComputeProgram appends the matching CPU
// values.
std::string emitCompute(const ShaderGraph& graph, Backend backend)
{
    auto source = std::string {};
    auto is2D = graph.dispatchRank() == DispatchRank::TwoD;

    if (backend == Backend::Metal)
        source += "#include <metal_stdlib>\nusing namespace metal;\n\n";

    auto uniformTypes = graph.uniforms();
    auto uniformNames = Vector<std::string> {};

    for (auto i = 0; i < uniformTypes.size(); ++i)
        uniformNames.add("u" + std::to_string(i));

    if (is2D)
    {
        uniformTypes.add(ValueType::UInt);
        uniformNames.add("width");
        uniformTypes.add(ValueType::UInt);
        uniformNames.add("height");
    }
    else
    {
        uniformTypes.add(ValueType::UInt);
        uniformNames.add("count");
    }

    source += uniformBlock(backend, uniformTypes, uniformNames);

    const auto& buffers = graph.storageBuffers();

    assert(buffers.size() <= ComputePass::maxBufferSlots
           && "eacp: a kernel may bind ComputePass::maxBufferSlots storage "
              "buffers. A slot past that has no register the root signature "
              "declares, so it binds nowhere and reads zeroes.");

    if (backend == Backend::Metal)
    {
        source += "kernel void computeMain(";

        for (auto i = 0; i < buffers.size(); ++i)
        {
            source += std::string(metalBufferType(buffers[i])) + " buffer"
                      + std::to_string(i) + " [[buffer(" + std::to_string(i)
                      + ")]],\n    ";
        }

        // Textures are kernel parameters like the buffers, on an index space of
        // their own. A written one takes the write access qualifier and no
        // sampler: there is nothing to sample it with and nothing to read.
        for (auto i = 0; i < graph.textureCount(); ++i)
        {
            auto slot = std::to_string(i);

            if (graph.textureAccess(i) == TextureAccess::Write)
            {
                source += "texture2d<float, access::write> texture" + slot
                          + " [[texture(" + slot + ")]],\n    ";
                continue;
            }

            source += "texture2d<float> texture" + slot + " [[texture(" + slot
                      + ")]],\n    sampler sampler" + slot + " [[sampler(" + slot
                      + ")]],\n    ";
        }

        source += "constant Uniforms& uniforms [[buffer("
                  + std::to_string(ComputePass::uniformBase) + ")]],\n    ";
        source += std::string(is2D ? "uint2" : "uint")
                  + " gid [[thread_position_in_grid]]";

        auto indexType = std::string(is2D ? "uint2" : "uint");

        if (graph.usesLocalId())
            source +=
                ",\n    " + indexType + " lid [[thread_position_in_threadgroup]]";

        if (graph.usesGroupId())
            source +=
                ",\n    " + indexType + " tgid [[threadgroup_position_in_grid]]";

        source += ")\n{\n";

        // Threadgroup arrays are body-scope declarations on Metal, ahead of
        // everything that subscripts them.
        for (auto i = 0; i < graph.sharedArrays().size(); ++i)
        {
            const auto& shared = graph.sharedArrays()[i];

            source += "    threadgroup " + std::string(typeName(shared.elementType))
                      + " s" + std::to_string(i) + "["
                      + std::to_string(shared.elements) + "];\n";
        }
    }
    else
    {
        // SRV t<slot> / UAV u<slot> with one shared slot counter, matching the
        // flat Metal indices ComputePass binds both backends with.
        for (auto i = 0; i < buffers.size(); ++i)
        {
            auto slot = std::to_string(i);
            auto readOnly = buffers[i] == BufferAccess::Read;

            source += hlslBufferType(buffers[i]);
            source += " buffer";
            source += slot;
            source += readOnly ? " : register(t" : " : register(u";
            source += slot + ");\n";
        }

        if (buffers.size() > 0)
            source += "\n";

        // Textures are globals here, and their registers start above every
        // buffer slot's: a texture and a storage buffer share the t and u
        // spaces on this backend, while their slots are counted separately. See
        // ComputePass::textureRegisterBase.
        for (auto i = 0; i < graph.textureCount(); ++i)
        {
            auto slot = std::to_string(i);
            auto reg = std::to_string(ComputePass::textureRegisterBase + i);

            if (graph.textureAccess(i) == TextureAccess::Write)
            {
                source += "RWTexture2D<float4> texture" + slot + " : register(u"
                          + reg + ");\n";
                continue;
            }

            auto samplerRegister =
                i * samplingConfigurations + samplingIndex(graph.textureSampling(i));

            source += "Texture2D texture" + slot + " : register(t" + reg
                      + ");\nSamplerState sampler" + slot + " : register(s"
                      + std::to_string(samplerRegister) + ");\n";
        }

        if (graph.textureCount() > 0)
            source += "\n";

        // Threadgroup arrays are globals on HLSL, like the buffers above.
        for (auto i = 0; i < graph.sharedArrays().size(); ++i)
        {
            const auto& shared = graph.sharedArrays()[i];

            source += "groupshared " + std::string(typeName(shared.elementType))
                      + " s" + std::to_string(i) + "["
                      + std::to_string(shared.elements) + "];\n";
        }

        if (graph.sharedArrays().size() > 0)
            source += "\n";

        auto groupWidth =
            is2D ? ComputePass::threadGroupSize2D : ComputePass::threadGroupWidth;
        auto groupHeight = is2D ? ComputePass::threadGroupSize2D : 1;

        source += "[numthreads(" + std::to_string(groupWidth) + ", "
                  + std::to_string(groupHeight) + ", 1)]\n";
        source += "void computeMain(uint3 threadId : SV_DispatchThreadID";

        if (graph.usesLocalId())
            source += ", uint3 localThread : SV_GroupThreadID";

        if (graph.usesGroupId())
            source += ", uint3 groupIndex : SV_GroupID";

        source += ")\n{\n";
        source +=
            is2D ? "    uint2 gid = threadId.xy;\n" : "    uint gid = threadId.x;\n";

        if (graph.usesLocalId())
            source += is2D ? "    uint2 lid = localThread.xy;\n"
                           : "    uint lid = localThread.x;\n";

        if (graph.usesGroupId())
            source += is2D ? "    uint2 tgid = groupIndex.xy;\n"
                           : "    uint tgid = groupIndex.x;\n";
    }

    // The early-return bounds guard the rounded-up dispatch needs - except in
    // a kernel that barriers, where a return some threads take ahead of a
    // barrier the rest sit at is undefined on both backends. There every
    // thread runs the whole body, and the kernel bounds its own stores
    // against gridCount()/gridWidth()/gridHeight() instead.
    if (!graph.usesBarrier())
        source += is2D ? "    if (gid.x >= uniforms.width || gid.y >= "
                         "uniforms.height)\n        return;\n"
                       : "    if (gid >= uniforms.count)\n        return;\n";

    // Stores ride the statement stream like everything else, so the body is
    // one block walk: a write records where it was made, inside whatever
    // loop or branch was open, and the emitter has no end-of-kernel step.
    auto stageRoots = Vector<int> {};
    collectStatementRoots(graph, ShaderGraph::rootBlock, stageRoots);

    auto stage = StageEmitter {graph, backend};

    source += stage.declareArrays(stageRoots, "    ");
    source += stage.emitBlock(ShaderGraph::rootBlock, "    ");

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

        // Storage buffers are globals here like the textures, at registers
        // above every texture slot - the render signature's mirror of the way
        // a kernel's textures sit above its buffers. See
        // RenderPass::bufferRegisterBase.
        for (auto i = 0; i < graph.storageBuffers().size(); ++i)
            source += "StructuredBuffer<float> buffer" + std::to_string(i)
                      + " : register(t"
                      + std::to_string(RenderPass::bufferRegisterBase + i) + ");\n";

        if (graph.storageBuffers().size() > 0)
            source += "\n";
    }

    // On Metal each stage declares the uniform block as a function parameter,
    // and only when that stage's expressions read one; the HLSL cbuffer is a
    // global both functions already see. Slot 0 maps to buffer(uniformBase)
    // in both stages, matching what RenderPass::setVertexBytes /
    // setFragmentBytes bind. Uniforms live at buffer(uniformBase..) so a
    // vertex layout with multiple per-instance slots (0..N) never collides
    // with them.
    auto vertexRoots = vertexStageRoots(graph);

    if (backend == Backend::Metal)
    {
        source += "vertex VertexOut vertexMain(VertexIn input [[stage_in]]";

        if (hasUniforms && vertexReadsUniforms(graph))
            source += ", constant Uniforms& uniforms [[buffer("
                      + std::to_string(RenderPass::uniformBase) + ")]]";

        source += bufferParameters(graph, vertexRoots);
        source += ")\n{\n";
    }
    else
    {
        source += "VertexOut vertexMain(VertexIn input)\n{\n";
    }

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
    auto fragmentRoots = Vector<int> {graph.fragment()};

    if (graph.discard() >= 0)
        fragmentRoots.add(graph.discard());

    // What the statements read counts towards the stage's uniform declaration,
    // but not towards the colour's locals: a statement's own expressions are
    // named where that statement is emitted, above.
    auto stageRoots = fragmentStageRoots(graph);

    if (backend == Backend::Metal)
    {
        source += "fragment float4 fragmentMain(VertexOut input [[stage_in]]";

        if (hasUniforms && fragmentReadsUniforms(graph))
            source += ",\n    constant Uniforms& uniforms [[buffer("
                      + std::to_string(RenderPass::uniformBase) + ")]]";

        for (auto i = 0; i < graph.textureCount(); ++i)
            source += ",\n    texture2d<float> texture" + std::to_string(i)
                      + " [[texture(" + std::to_string(i)
                      + ")]],\n    sampler sampler" + std::to_string(i)
                      + " [[sampler(" + std::to_string(i) + ")]]";

        source += bufferParameters(graph, stageRoots);
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

bool vertexReadsUniforms(const ShaderGraph& graph)
{
    return anyReferencesUniform(graph, vertexStageRoots(graph));
}

bool fragmentReadsUniforms(const ShaderGraph& graph)
{
    return anyReferencesUniform(graph, fragmentStageRoots(graph));
}

std::string emitMetal(const ShaderGraph& graph)
{
    return emit(graph, Backend::Metal);
}

std::string emitHlsl(const ShaderGraph& graph)
{
    return emit(graph, Backend::DirectX);
}
} // namespace eacp::GPU
