#include "ShaderEmitter.h"

#include "../Frame/ComputePass.h"
#include "../Frame/RenderPass.h"
#include "ShaderGraph.h"
#include "UniformLayout.h"

#include <cassert>
#include <cstdio>

// MSL and HLSL differ only in the binding syntax and the stage scaffolding
// captured by the helpers below; the expression printer is fully shared.

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

// print() spells out a node's own expression, for inline nodes and local
// definitions alike; ref() is what children and outputs go through, so a node
// the stage plan named as a local collapses to its tN name.
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
                // The uint, int and bool spellings are shared by MSL and HLSL.
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

                // A matrix construction passes columns, but HLSL fills a matrix
                // from rows, so transpose() restores the column-major value.
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
                // constant must print (-(-1.0)), never the decrement (--1.0).
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
                return "(" + ref(expr.args[0]) + " ? " + ref(expr.args[1]) + " : "
                       + ref(expr.args[2]) + ")";

            case ExprKind::VarRead:
                return "v" + std::to_string(expr.index);

            case ExprKind::Mul:
            {
                // In the order it was written: both languages read a vector on
                // the left as a row and one on the right as a column.
                auto left = ref(expr.args[0]);
                auto right = ref(expr.args[1]);

                if (backend == Backend::Metal)
                    return "(" + left + " * " + right + ")";

                return "mul(" + left + ", " + right + ")";
            }

            case ExprKind::Sample:
            {
                // Through the sampler declared at the same index as the
                // texture. A second argument is the mip level the shader
                // picked, which Metal and HLSL spell differently.
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
                // Metal takes texel coordinates unsigned, so a negative one
                // wraps to a large unsigned value and reads as zero, matching
                // HLSL's Load. D3D's mip level rides in the third component.
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
                // Both kernel scaffoldings declare the work-item id as gid.
                if (graph.dispatchRank() == DispatchRank::OneD)
                    return "gid";

                return expr.index == 0 ? "gid.x" : "gid.y";

            case ExprKind::BufferRead:
                return "buffer" + std::to_string(expr.index) + "["
                       + ref(expr.args[0]) + "]";

            case ExprKind::AtomicLoad:
            {
                // Reading an HLSL UAV element is a plain subscript; MSL wraps
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

            // The threadgroup indices ride the same scaffolding as gid.
            case ExprKind::LocalId:
                if (graph.dispatchRank() == DispatchRank::OneD)
                    return "lid";

                return expr.index == 0 ? "lid.x" : "lid.y";

            case ExprKind::GroupId:
                if (graph.dispatchRank() == DispatchRank::OneD)
                    return "tgid";

                return expr.index == 0 ? "tgid.x" : "tgid.y";

            // The implicit bound the dispatch appended to the uniform block.
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
// and swizzles stay inline.
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
// a name is left alone, and so is everything under it.
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

// Which variables running a statement can leave holding something else,
// following the bodies of an if or a loop.
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
// store to it, or the barrier that publishes what other threads stored. A name
// computed from shared memory is given up the moment this says yes.
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

// Marking is a stamp rather than a flag, so restarting is a counter increment
// instead of clearing a buffer the size of the graph - which matters because
// the walk below runs once per open name per statement.
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

    // Ahead of the zeroes a fresh stamp buffer holds, so nothing counts as
    // visited until something visits it.
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

// Emits one stage: its statements, then the expressions its outputs are. Any
// operation evaluated more than once becomes a tN local; a name is given up
// when a statement writes a variable it read, and a loop condition takes none.
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
    // function before any name has been handed out, in slot order so an array
    // whose elements read another one finds it already there.
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
        // up. An assignment needs no such pass first: its right-hand side is
        // what the variable held before it.
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
                // to be stored through; HLSL's UAV element is an ordinary uint.
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

                // InterlockedAdd hands the old value back through an out
                // parameter, so the name must exist before the call fills it.
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

            // The names it invalidates - anything computed from shared memory -
            // are given up by the dropStale below.
            case StatementKind::Barrier:
                source =
                    indent
                    + (printer.backend == Backend::Metal
                           ? "threadgroup_barrier(mem_flags::mem_threadgroup);\n"
                           : "GroupMemoryBarrierWithGroupSync();\n");
                break;

            // MSL takes the colour first and the coordinate second; HLSL
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

        // Again afterwards, for the names this statement's own expressions
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

    // The block's own statements only; a nested body counts its own when it is
    // emitted, and a loop's condition is deliberately left out.
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

        // A statement that leaves no variable holding something else and moves
        // no shared memory cannot have staled a name.
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
    // Held here so naming a stage costs one buffer, not one per name.
    VisitSet visited;
    Vector<char> written;
};

// Whether the expression tree under node reads a uniform, stopping at a Varying
// read - the vertex stage walks its source tree. The visited set is not an
// optimisation: over a shared graph, revisiting is exponential in the sharing.
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

// Every expression a block's statements evaluate, so a stage sees what its
// statements read and not only what its output expression does - otherwise a
// uniform read only from inside a loop would go undeclared.
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

// One visited set across every root: a node already known to hold no uniform
// holds none whichever root reached it.
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
// from, which is why emit() keeps both.
Vector<int> fragmentStageRoots(const ShaderGraph& graph)
{
    auto roots = Vector<int> {graph.fragment()};

    if (graph.discard() >= 0)
        roots.add(graph.discard());

    collectStatementRoots(graph, ShaderGraph::rootBlock, roots);
    return roots;
}

// A render stage declares only the buffers it reads, unlike a kernel where
// every slot is a parameter, so each function needs its own answer.
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

// Always read-only: a vertex or fragment function has no writable buffer.
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

// The CPU block is packed with MSL struct alignment (UniformLayout.h), so
// explicit pad scalars are emitted wherever HLSL cbuffer packing - which only
// forbids straddling a 16-byte register - would place a field lower.
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

// An atomic buffer is the only kind whose *elements* differ, so it cannot be a
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

// Compute kernel emission, sharing the render expression printer. The uniform
// block always ends with the implicit grid extents the bounds guard reads, for
// which ComputeProgram appends the matching CPU values.
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
        // their own. A written one takes no sampler.
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

        // Textures are globals here, their registers starting above every
        // buffer slot's because the two share the t and u spaces on this
        // backend. See ComputePass::textureRegisterBase.
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

    // The bounds guard the rounded-up dispatch needs - except in a kernel that
    // barriers, where a return some threads take ahead of a barrier the rest
    // sit at is undefined on both backends, so it bounds its own stores.
    if (!graph.usesBarrier())
        source += is2D ? "    if (gid.x >= uniforms.width || gid.y >= "
                         "uniforms.height)\n        return;\n"
                       : "    if (gid >= uniforms.count)\n        return;\n";

    // Stores ride the statement stream, so the body is one block walk: a write
    // is emitted where it was made, inside whatever loop or branch was open.
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

    // Both backends expose the block as "uniforms.uN" - HLSL wrapping the
    // struct in a cbuffer - so the expression printer stays backend-agnostic.
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
        // The root signature declares a static sampler for every (slot,
        // configuration) pair, so picking the register picks the sampler.
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
        // above every texture slot. See RenderPass::bufferRegisterBase.
        for (auto i = 0; i < graph.storageBuffers().size(); ++i)
            source += "StructuredBuffer<float> buffer" + std::to_string(i)
                      + " : register(t"
                      + std::to_string(RenderPass::bufferRegisterBase + i) + ");\n";

        if (graph.storageBuffers().size() > 0)
            source += "\n";
    }

    // On Metal each stage declares the uniform block as a function parameter,
    // and only when its expressions read one, at buffer(uniformBase) so the
    // per-instance vertex slots (0..N) never collide with it.
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

    // The vertex stage takes no statements: a mutable local and the control
    // flow driving it belong to the fragment expression, the way sampling does.
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

    // The alpha test is a fragment root like the colour, so a value both read
    // is computed once and shared.
    auto fragmentRoots = Vector<int> {graph.fragment()};

    if (graph.discard() >= 0)
        fragmentRoots.add(graph.discard());

    // What the statements read counts towards the stage's uniform declaration,
    // but not towards the colour's locals.
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

    // The statements run first, the fragment expression reading a mutable local
    // out of them, so the colour is planned after them.
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
