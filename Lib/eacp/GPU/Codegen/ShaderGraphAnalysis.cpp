#include "ShaderGraphAnalysis.h"

#include <span>

namespace eacp::GPU::ShaderEmission
{
namespace
{
// Operation nodes are worth naming when evaluated more than once; leaf reads
// and swizzles stay inline - naming them saves nothing and hurts readability.
constexpr ExprKind kindsWorthALocal[] = {ExprKind::Construct,
                                         ExprKind::Call,
                                         ExprKind::Unary,
                                         ExprKind::Binary,
                                         ExprKind::Compare,
                                         ExprKind::Select,
                                         ExprKind::Mul,
                                         ExprKind::Sample,
                                         ExprKind::Fetch,
                                         ExprKind::BufferRead,
                                         ExprKind::BufferVectorRead,
                                         ExprKind::AtomicLoad,
                                         ExprKind::ArrayRead,
                                         ExprKind::SharedRead};

// Whichever way the buffer was declared: an output a kernel reads back and an
// atomic counter it loads are both elements a store can have changed.
constexpr ExprKind bufferElementReadKinds[] = {
    ExprKind::BufferRead, ExprKind::BufferVectorRead, ExprKind::AtomicLoad};

constexpr ExprKind bufferSlotReadKinds[] = {ExprKind::BufferRead,
                                            ExprKind::BufferVectorRead};

constexpr StatementKind variableWritingStatements[] = {StatementKind::Declare,
                                                       StatementKind::Assign,
                                                       StatementKind::AtomicAdd,
                                                       StatementKind::GroupReduce};

constexpr StatementKind sharedMovingStatements[] = {
    StatementKind::SharedStore, StatementKind::Barrier, StatementKind::GroupReduce};

constexpr StatementKind bufferSlotStoringStatements[] = {StatementKind::Store,
                                                         StatementKind::VectorStore};

template <typename Kind>
bool isKindAmong(Kind kind, std::span<const Kind> kinds)
{
    for (auto candidate: kinds)
        if (candidate == kind)
            return true;

    return false;
}

// The blocks an if or a loop runs, in the order they appear: what such a
// statement writes is whatever its bodies write.
Vector<int> getNestedBodies(const Statement& statement)
{
    auto bodies = Vector<int> {};

    if (statement.kind == StatementKind::If)
    {
        bodies.add(statement.body);

        if (statement.elseBody >= 0)
            bodies.add(statement.elseBody);
    }

    if (statement.kind == StatementKind::Loop)
        bodies.add(statement.body);

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
        if (touchesShared(graph, graph.statement(index)))
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

// A fragment stored back into a threadgroup tile moves that tile, so any name
// read out of it beforehand is given up; one stored into a buffer moves no
// shared memory at all.
bool storesSimdMatrixTo(const Statement& statement, SimdMatrixMemory memory)
{
    return statement.kind == StatementKind::SimdMatrixStore
           && statement.memory == memory;
}

// The one statement whose buffer is not in `slot` is the atomic add: that field
// names the variable the value from before the add lands in. Nor is a fragment
// store's, `slot` there naming the fragment, and it writes a buffer only when
// that is where its patch is.
bool writesBufferSlotField(const Statement& statement)
{
    return statement.kind == StatementKind::AtomicAdd
           || storesSimdMatrixTo(statement, SimdMatrixMemory::Buffer);
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
    roots.add(statement.stride);
}

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

// A Varying is the stage boundary; an array element is followed, the array
// being declared inside the stage that subscripts it.
void collectAttributeReads(const ShaderGraph& graph,
                           int node,
                           Vector<char>& reached,
                           VisitSet& seen)
{
    if (node < 0 || !seen.visit(node))
        return;

    const auto& expr = graph.expr(node);

    if (expr.kind == ExprKind::Varying)
        return;

    if (expr.kind == ExprKind::Input)
        reached[node] = 1;

    if (expr.kind == ExprKind::ArrayRead)
        for (auto element: graph.arrays()[expr.index].elements)
            collectAttributeReads(graph, element, reached, seen);

    for (auto argument: expr.args)
        collectAttributeReads(graph, argument, reached, seen);
}

// -1 when no declared varying carries `source`.
int varyingCarrying(const ShaderGraph& graph, int source)
{
    for (auto i = 0; i < graph.varyings().size(); ++i)
        if (graph.varyings()[i].sourceNode == source)
            return i;

    return -1;
}

Vector<char> collectFragmentAttributeReads(const ShaderGraph& graph)
{
    auto reached = Vector<char> {};
    reached.resize(graph.nodeCount(), 0);

    auto seen = VisitSet {graph.nodeCount()};

    for (auto root: fragmentStageRoots(graph))
        collectAttributeReads(graph, root, reached, seen);

    return reached;
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

    if (isKindAmong<ExprKind>(expr.kind, bufferSlotReadKinds)
        && expr.index < used.size())
        used[expr.index] = 1;

    for (auto argument: expr.args)
        collectBufferSlots(graph, argument, used, seen);
}
} // namespace

// The one thing named whatever its kind is a record write's value, which its
// element stores all have to be handed rather than evaluate one at a time.
bool wantsLocal(ExprKind kind)
{
    return isKindAmong<ExprKind>(kind, kindsWorthALocal);
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

    const auto& expr = graph.expr(node);

    // A vector load's index is printed once per component by the backends that
    // expand it into subscripts, so it earns a name there the way any
    // subexpression evaluated more than once does - and Metal, which prints it
    // once, reads the same name and is no worse for it.
    auto perArgument =
        expr.kind == ExprKind::BufferVectorRead ? componentCount(expr.type) : 1;

    for (auto argument: expr.args)
        for (auto i = 0; i < perArgument; ++i)
            countUses(graph, argument, uses, seen);
}

void LocalOrdering::visit(int node)
{
    if (node < 0 || seen[node] || locals[node] >= 0)
        return;

    seen[node] = 1;

    for (auto argument: graph.expr(node).args)
        visit(argument);

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
void collectWrites(const ShaderGraph& graph,
                   const Statement& statement,
                   Vector<char>& written)
{
    if (isKindAmong<StatementKind>(statement.kind, variableWritingStatements))
        written[statement.slot] = 1;

    for (auto body: getNestedBodies(statement))
        collectBlockWrites(graph, body, written);
}

// Whether running a statement can change what threadgroup memory holds: a
// store to it, or the barrier that publishes what other threads stored -
// following nested bodies the way collectWrites does. What this feeds is the
// same rule variables get: a name computed from shared memory is given up the
// moment shared memory may have moved on.
bool touchesShared(const ShaderGraph& graph, const Statement& statement)
{
    if (isKindAmong<StatementKind>(statement.kind, sharedMovingStatements))
        return true;

    if (storesSimdMatrixTo(statement, SimdMatrixMemory::Shared))
        return true;

    for (auto body: getNestedBodies(statement))
        if (blockTouchesShared(graph, body))
            return true;

    return false;
}

// Its storage-buffer sibling: which buffer slots running a statement can leave
// holding something else. What it feeds is the rule variables and shared memory
// already get - a name computed from an element is given up the moment that
// buffer may have moved on - and it is what makes an output a kernel reads back
// answer with what the kernel stored rather than with what was there before.
void collectBufferWrites(const ShaderGraph& graph,
                         const Statement& statement,
                         Vector<char>& written)
{
    if (isKindAmong<StatementKind>(statement.kind, bufferSlotStoringStatements))
        written[statement.slot] = 1;

    if (writesBufferSlotField(statement))
        written[statement.bufferSlot] = 1;

    for (auto body: getNestedBodies(statement))
        collectBlockBufferWrites(graph, body, written);
}

bool StalenessQuery::readsStale(int node)
{
    if (node < 0 || !seen.visit(node))
        return false;

    const auto& expr = graph.expr(node);

    if (expr.kind == ExprKind::VarRead && variablesWritten[expr.index] != 0)
        return true;

    if (sharedMoved && expr.kind == ExprKind::SharedRead)
        return true;

    if (isKindAmong<ExprKind>(expr.kind, bufferElementReadKinds)
        && buffersWritten[expr.index] != 0)
        return true;

    for (auto argument: expr.args)
        if (readsStale(argument))
            return true;

    return false;
}

// Every expression the statements of a block reach, its nested bodies included.
// Gathered so a stage sees what its statements read and not only what its
// output expression does: without it a uniform read only from inside a loop
// would go undeclared.
void collectStatementExpressions(const ShaderGraph& graph,
                                 int block,
                                 LoopConditions loopConditions,
                                 Vector<int>& roots)
{
    for (auto index: graph.block(block).statements)
    {
        const auto& statement = graph.statement(index);

        addStatementExpressions(statement, loopConditions, roots);

        if (statement.body >= 0)
            collectStatementExpressions(
                graph, statement.body, loopConditions, roots);

        if (statement.elseBody >= 0)
            collectStatementExpressions(
                graph, statement.elseBody, loopConditions, roots);
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

    collectStatementExpressions(
        graph, ShaderGraph::rootBlock, LoopConditions::Included, roots);
    return roots;
}

PromotedAttributes promotedAttributes(const ShaderGraph& graph)
{
    auto promoted = PromotedAttributes {};
    promoted.varyingOf.resize(graph.inputs().size(), -1);

    auto reached = collectFragmentAttributeReads(graph);

    // Ascending node order, so the implicit varyings follow declaration order.
    for (auto node = 0; node < graph.nodeCount(); ++node)
    {
        if (reached[node] == 0)
            continue;

        auto slot = graph.expr(node).index;
        auto carrier = varyingCarrying(graph, node);

        if (carrier < 0)
        {
            carrier = graph.varyings().size() + promoted.carried.size();
            promoted.carried.add(slot);
        }

        promoted.varyingOf[slot] = carrier;
    }

    return promoted;
}

Vector<StageVarying> stageVaryings(const ShaderGraph& graph,
                                   const PromotedAttributes& promoted)
{
    auto slots = Vector<StageVarying> {};

    for (const auto& varying: graph.varyings())
        slots.add(StageVarying {varying.type, varying.sourceNode, -1});

    for (auto attribute: promoted.carried)
        slots.add(StageVarying {graph.inputs()[attribute], -1, attribute});

    return slots;
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
} // namespace eacp::GPU::ShaderEmission
