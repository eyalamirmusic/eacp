#pragma once

#include "ShaderGraph.h"

namespace eacp::GPU::ShaderEmission
{
// Restarting is a counter increment: the staleness walk runs per open name per
// statement, and clearing a graph-sized buffer each time hangs large shaders.
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
    int generation = 1;
};

bool isLocalWanted(ExprKind kind);

// dfdx, dfdy and fwidth; and anything taking the fragment's derivatives,
// those three and the samples whose level is derived from them.
bool isDerivativeCall(const Expr& expr);
bool takesDerivedLevel(const ShaderGraph& graph, const Expr& expr);

void countUses(const ShaderGraph& graph,
               int node,
               Vector<int>& uses,
               Vector<char>& seen);

// Post order, so every definition precedes its uses.
struct LocalOrdering
{
    void visit(int node);
    bool isAlreadyPlanned(int node) const;

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

bool doesTouchSharedMemory(const ShaderGraph& graph, const Statement& statement);

void collectBufferWrites(const ShaderGraph& graph,
                         const Statement& statement,
                         Vector<char>& written);

void collectCalledBufferWrites(const ShaderGraph& graph,
                               int function,
                               Vector<char>& written);

struct StalenessQuery
{
    bool doesReadStaleValue(int node);
    bool doesReadWhatMoved(const Expr& expr) const;
    bool doesReadWrittenVariable(const Expr& expr) const;
    bool doesReadMovedSharedMemory(const Expr& expr) const;
    bool doesReadWrittenBuffer(const Expr& expr) const;

    const ShaderGraph& graph;
    const Vector<char>& variablesWritten;
    const Vector<char>& buffersWritten;
    bool sharedMoved = false;
    VisitSet& seen;
};

enum class LoopConditions
{
    Included,
    Excluded
};

void collectStatementExpressions(const ShaderGraph& graph,
                                 int block,
                                 LoopConditions loopConditions,
                                 Vector<int>& roots);

} // namespace eacp::GPU::ShaderEmission
