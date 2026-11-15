#include "ShaderGraphAnalysis.h"

#include <initializer_list>

namespace eacp::GPU::ShaderEmission
{
namespace
{
// Leaf reads and swizzles stay inline: naming them saves nothing.
constexpr auto kindsWorthALocal = Array<ExprKind, 18> {ExprKind::FunctionCall,
                                                       ExprKind::Construct,
                                                       ExprKind::Call,
                                                       ExprKind::Unary,
                                                       ExprKind::Binary,
                                                       ExprKind::Compare,
                                                       ExprKind::Select,
                                                       ExprKind::Mul,
                                                       ExprKind::Sample,
                                                       ExprKind::Fetch,
                                                       ExprKind::TextureFunction,
                                                       ExprKind::TextureQuery,
                                                       ExprKind::BufferRead,
                                                       ExprKind::BufferVectorRead,
                                                       ExprKind::AtomicLoad,
                                                       ExprKind::ArrayRead,
                                                       ExprKind::SharedRead,
                                                       ExprKind::MatrixColumn};

constexpr auto bufferElementReadKinds = Array<ExprKind, 3> {
    ExprKind::BufferRead, ExprKind::BufferVectorRead, ExprKind::AtomicLoad};

constexpr auto variableWritingStatements =
    Array<StatementKind, 4> {StatementKind::Declare,
                             StatementKind::Assign,
                             StatementKind::Atomic,
                             StatementKind::GroupReduce};

constexpr auto sharedMovingStatements = Array<StatementKind, 3> {
    StatementKind::SharedStore, StatementKind::Barrier, StatementKind::GroupReduce};

constexpr auto bufferSlotStoringStatements =
    Array<StatementKind, 2> {StatementKind::Store, StatementKind::VectorStore};

Vector<int> getNestedBodies(const Statement& statement)
{
    auto bodies = Vector<int> {};

    for (auto body: {statement.body, statement.elseBody, statement.continuing})
        if (body >= 0)
            bodies.add(body);

    return bodies;
}

void collectBlockWrites(const ShaderGraph& graph, int block, Vector<char>& written)
{
    for (auto index: graph.block(block).statements)
        collectWrites(graph, graph.statement(index), written);
}

bool blockTouchesShared(const ShaderGraph& graph, int block)
{
    for (auto index: graph.block(block).statements)
        if (doesTouchSharedMemory(graph, graph.statement(index)))
            return true;

    return false;
}

void collectBlockBufferWrites(const ShaderGraph& graph,
                              int block,
                              Vector<char>& written)
{
    for (auto index: graph.block(block).statements)
        collectBufferWrites(graph, graph.statement(index), written);
}

bool doesStoreSimdMatrixTo(const Statement& statement, SimdMatrixMemory memory)
{
    return statement.kind == StatementKind::SimdMatrixStore
           && statement.memory == memory;
}

bool isAtomicOn(const Statement& statement, MemorySpace memory)
{
    return statement.kind == StatementKind::Atomic && statement.memory == memory;
}

// An atomic's `slot` names the variable that receives the old value, and a
// fragment store's names the fragment.
bool doesWriteBufferSlotField(const Statement& statement)
{
    return isAtomicOn(statement, MemorySpace::Buffer)
           || doesStoreSimdMatrixTo(statement, SimdMatrixMemory::Buffer);
}

void addStatementExpressions(const Statement& statement,
                             LoopConditions loopConditions,
                             Vector<int>& roots)
{
    auto skipsCondition = loopConditions == LoopConditions::Excluded
                          && statement.kind == StatementKind::Loop;

    if (!skipsCondition)
        roots.add(statement.value);

    roots.add(statement.index);
    roots.add(statement.indexY);
    roots.add(statement.layer);
    roots.add(statement.stride);
    roots.add(statement.compare);
}

} // namespace

bool isLocalWanted(ExprKind kind)
{
    return kindsWorthALocal.contains(kind);
}

bool isDerivativeCall(const Expr& expr)
{
    return expr.text == "dfdx" || expr.text == "dfdy" || expr.text == "fwidth";
}

// A plain sample's level is derived unless it was given one; a layered or a
// comparison one has no level form at all.
bool takesDerivedLevel(const ShaderGraph& graph, const Expr& expr)
{
    switch (expr.kind)
    {
        case ExprKind::TextureFunction:
            return usesImplicitDerivatives(textureFunctionNamed(expr.text));
        case ExprKind::Sample:
            return expr.text == "compare" || isLayered(graph.textureKind(expr.index))
                   || expr.args.size() < 2;
        case ExprKind::Call:
            return isDerivativeCall(expr);
        default:
            return false;
    }
}

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

    const auto& expr = graph.expr(node);

    // A vector load's index prints once per component in some dialects.
    auto perArgument =
        expr.kind == ExprKind::BufferVectorRead ? componentCount(expr.type) : 1;

    for (auto argument: expr.args)
        for (auto i = 0; i < perArgument; ++i)
            countUses(graph, argument, uses, seen);
}

bool LocalOrdering::isAlreadyPlanned(int node) const
{
    return seen[node] != 0 || locals[node] >= 0;
}

void LocalOrdering::visit(int node)
{
    if (node < 0 || isAlreadyPlanned(node))
        return;

    seen[node] = 1;

    for (auto argument: graph.expr(node).args)
        visit(argument);

    if (uses[node] > 1 && isLocalWanted(graph.expr(node).kind))
        order.add(node);
}

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

void collectWrites(const ShaderGraph& graph,
                   const Statement& statement,
                   Vector<char>& written)
{
    if (variableWritingStatements.contains(statement.kind))
        written[statement.slot] = 1;

    if (statement.kind == StatementKind::Call && statement.slot >= 0)
        written[statement.slot] = 1;

    if (statement.exchanged >= 0)
        written[statement.exchanged] = 1;

    for (auto body: getNestedBodies(statement))
        collectBlockWrites(graph, body, written);
}

bool doesTouchSharedMemory(const ShaderGraph& graph, const Statement& statement)
{
    if (sharedMovingStatements.contains(statement.kind))
        return true;

    if (doesStoreSimdMatrixTo(statement, SimdMatrixMemory::Shared)
        || isAtomicOn(statement, MemorySpace::Shared))
        return true;

    for (auto body: getNestedBodies(statement))
        if (blockTouchesShared(graph, body))
            return true;

    return false;
}

void collectBufferWrites(const ShaderGraph& graph,
                         const Statement& statement,
                         Vector<char>& written)
{
    if (bufferSlotStoringStatements.contains(statement.kind))
        written[statement.slot] = 1;

    if (doesWriteBufferSlotField(statement))
        written[statement.bufferSlot] = 1;

    if (statement.kind == StatementKind::Call && statement.value >= 0)
        collectCalledBufferWrites(graph, graph.expr(statement.value).index, written);

    for (auto body: getNestedBodies(statement))
        collectBlockBufferWrites(graph, body, written);
}

// What a function writes is what a call of it writes, its callees' included -
// which its own statements reach, since a function writing storage is called
// as a statement.
void collectCalledBufferWrites(const ShaderGraph& graph,
                               int function,
                               Vector<char>& written)
{
    collectBlockBufferWrites(graph, graph.function(function).body, written);
}

bool StalenessQuery::doesReadWrittenVariable(const Expr& expr) const
{
    return expr.kind == ExprKind::VarRead && variablesWritten[expr.index] != 0;
}

bool StalenessQuery::doesReadMovedSharedMemory(const Expr& expr) const
{
    return sharedMoved && expr.kind == ExprKind::SharedRead;
}

bool StalenessQuery::doesReadWrittenBuffer(const Expr& expr) const
{
    return bufferElementReadKinds.contains(expr.kind)
           && buffersWritten[expr.index] != 0;
}

bool StalenessQuery::doesReadWhatMoved(const Expr& expr) const
{
    return doesReadWrittenVariable(expr) || doesReadMovedSharedMemory(expr)
           || doesReadWrittenBuffer(expr);
}

bool StalenessQuery::doesReadStaleValue(int node)
{
    if (node < 0 || !seen.visit(node))
        return false;

    const auto& expr = graph.expr(node);

    if (doesReadWhatMoved(expr))
        return true;

    for (auto argument: expr.args)
        if (doesReadStaleValue(argument))
            return true;

    return false;
}

void collectStatementExpressions(const ShaderGraph& graph,
                                 int block,
                                 LoopConditions loopConditions,
                                 Vector<int>& roots)
{
    for (auto index: graph.block(block).statements)
    {
        const auto& statement = graph.statement(index);

        addStatementExpressions(statement, loopConditions, roots);

        for (auto body: getNestedBodies(statement))
            collectStatementExpressions(graph, body, loopConditions, roots);
    }
}

} // namespace eacp::GPU::ShaderEmission
