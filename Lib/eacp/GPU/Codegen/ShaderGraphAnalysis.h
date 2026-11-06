#pragma once

#include "ShaderGraph.h"

// The walks over a graph the emitter plans a stage from: which nodes are worth
// a name, which statements make a name stale, and which expressions, uniforms,
// attributes and buffers each stage reaches.

namespace eacp::GPU::ShaderEmission
{
// A visited set a walk can have a fresh one of without paying for one. Marking
// is a stamp rather than a flag, so starting over is a counter increment
// instead of clearing a buffer the size of the graph.
//
// It exists because the staleness walk runs per open name per statement, and a
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

bool wantsLocal(ExprKind kind);

void countUses(const ShaderGraph& graph,
               int node,
               Vector<int>& uses,
               Vector<char>& seen);

// Which nodes a run of expressions evaluates more than once, in dependency
// (post) order so every definition precedes its uses. A node that already holds
// a name is left alone, and so is everything under it: it is already computed.
struct LocalOrdering
{
    void visit(int node);

    const ShaderGraph& graph;
    const Vector<int>& uses;
    const Vector<int>& locals;
    Vector<char>& seen;
    Vector<int>& order;
};

void collectArrays(const ShaderGraph& graph,
                   int node,
                   Vector<char>& used,
                   Vector<char>& seen);

void collectWrites(const ShaderGraph& graph,
                   const Statement& statement,
                   Vector<char>& written);

bool touchesShared(const ShaderGraph& graph, const Statement& statement);

void collectBufferWrites(const ShaderGraph& graph,
                         const Statement& statement,
                         Vector<char>& written);

// Whether the value under a node no longer stands for itself after a
// statement: it read a variable that statement wrote, an element of a storage
// buffer the statement stored to, or threadgroup memory the statement may have
// moved.
struct StalenessQuery
{
    bool readsStale(int node);

    const ShaderGraph& graph;
    const Vector<char>& variablesWritten;
    const Vector<char>& buffersWritten;
    bool sharedMoved = false;
    VisitSet& seen;
};

// Whether a loop's condition counts among the expressions a block reaches. The
// statement emitter leaves it out, since a loop header takes no name of its
// own; the declarations a stage needs count it in.
enum class LoopConditions
{
    Included,
    Excluded
};

void collectStatementExpressions(const ShaderGraph& graph,
                                 int block,
                                 LoopConditions loopConditions,
                                 Vector<int>& roots);

bool anyReferencesUniform(const ShaderGraph& graph, const Vector<int>& roots);
Vector<int> vertexStageRoots(const ShaderGraph& graph);
Vector<int> fragmentStageRoots(const ShaderGraph& graph);

// A fragment-stage read of a vertex attribute names an identifier no dialect
// gives that stage, so each is promoted to a varying after the declared ones.
struct PromotedAttributes
{
    Vector<int> varyingOf; // attribute slot -> varying index, -1 = not read
    Vector<int> carried; // implicit varying order -> attribute slot
};

PromotedAttributes promotedAttributes(const ShaderGraph& graph);

struct StageVarying
{
    ValueType type = ValueType::Float;
    int sourceNode = -1;
    int attribute = -1;
};

Vector<StageVarying> stageVaryings(const ShaderGraph& graph,
                                   const PromotedAttributes& promoted);

Vector<char> bufferSlotsUsedBy(const ShaderGraph& graph, const Vector<int>& roots);
} // namespace eacp::GPU::ShaderEmission
