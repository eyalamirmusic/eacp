#pragma once

#include "ShaderExpressionPrinter.h"
#include "ShaderGraphAnalysis.h"

namespace eacp::GPU::ShaderEmission
{
// Emits one stage: its statements, then the expressions its outputs are.
//
// Any operation evaluated more than once becomes a tN local, so a shared
// subtree is computed - and printed - once instead of being inlined at every
// use. Control flow is what bounds that sharing, and the two rules it imposes
// are the whole of what makes this different from printing an expression tree:
//
// A name is given up the moment a statement writes a variable the value behind
// it read - which is what stops `d` computed before an `if` from standing for
// the same thing after a body that moved what it was computed from. A name
// neither body moves stays usable inside them both.
//
// A loop condition takes no name at all. It is printed into the while header,
// so binding it to a local ahead of the loop would test a value that never
// changes again; the names the body can invalidate are given up there too,
// since the header is re-evaluated after the body has run.
//
// A record write is what the rules answer to rather than bound by: its N
// element stores are one write, so its value takes a name whatever its use
// count and keeps it until the last of them has run.
class StageEmitter
{
public:
    StageEmitter(const ShaderGraph& graphToUse,
                 Backend backend,
                 Vector<int> attributeVaryings = {});

    const ShaderGraph& graph() const { return printer.graph; }

    std::string defineFor(const Vector<int>& roots, const std::string& indent);
    std::string declareArrays(const Vector<int>& roots, const std::string& indent);
    std::string emitBlock(int block, const std::string& indent);

    Vector<int> locals; // node id -> local index, -1 = inline
    ExprPrinter printer;
    int localCount = 0;

private:
    // Where a statement is being emitted: its indentation, the use counts of
    // the block it is in, and the names that block has handed out so far.
    struct StatementScope
    {
        const std::string& indent;
        const Vector<int>& uses;
        Vector<int>& open;
    };

    using StatementEmitFunction = std::string (StageEmitter::*)(const Statement&,
                                                                StatementScope&);

    struct StatementEmitEntry
    {
        StatementKind kind;
        StatementEmitFunction emit;
    };

    // The patch a SIMD-group matrix load or store walks, as the emitted source
    // names it.
    struct SimdMatrixPatch
    {
        std::string memory;
        std::string offset;
        std::string stride;
    };

    static StatementEmitFunction findStatementEmitter(StatementKind kind);

    std::string emitStatement(const Statement& statement,
                              const std::string& indent,
                              const Vector<int>& uses,
                              Vector<int>& open);
    std::string emitLoop(const Statement& statement,
                         const std::string& indent,
                         Vector<int>& open);
    std::string emitVariableWrite(const Statement& statement, StatementScope& scope);
    std::string emitIf(const Statement& statement, StatementScope& scope);
    std::string emitBreak(const Statement& statement, StatementScope& scope);
    std::string emitContinue(const Statement& statement, StatementScope& scope);
    std::string emitStore(const Statement& statement, StatementScope& scope);
    std::string emitVectorStore(const Statement& statement, StatementScope& scope);
    std::string emitAtomicAdd(const Statement& statement, StatementScope& scope);
    std::string emitSharedStore(const Statement& statement, StatementScope& scope);
    std::string emitBarrier(const Statement& statement, StatementScope& scope);
    std::string emitGroupReduce(const Statement& statement, StatementScope& scope);
    std::string emitSimdMatrixFill(const Statement& statement,
                                   StatementScope& scope);
    std::string emitSimdMatrixTransfer(const Statement& statement,
                                       StatementScope& scope);
    std::string emitSimdMatrixMultiplyAdd(const Statement& statement,
                                          StatementScope& scope);
    std::string emitTextureStore(const Statement& statement, StatementScope& scope);
    std::string emitNothing(const Statement& statement, StatementScope& scope);

    std::string defineInScope(const Vector<int>& roots, StatementScope& scope);
    std::string printBody(int block, const std::string& indent);

    std::string groupReduction(const Statement& statement,
                               const std::string& indent);
    std::string metalGroupReduction(const Statement& statement,
                                    const std::string& indent);
    std::string treeGroupReduction(const Statement& statement,
                                   const std::string& indent);

    bool metal() const;
    std::string simdMatrixDeclaration(int slot) const;
    std::string simdMatrixFill(const Statement& statement,
                               const std::string& indent);
    std::string simdMatrixLaneElement(const Statement& statement,
                                      const SimdMatrixPatch& patch,
                                      int which) const;
    std::string simdMatrixTransfer(const Statement& statement,
                                   const std::string& indent);
    std::string metalSimdMatrixTransfer(const Statement& statement,
                                        const SimdMatrixPatch& patch,
                                        const std::string& indent);
    std::string simdMatrixMultiplyAdd(const Statement& statement,
                                      const std::string& indent);
    std::string stageSimdMatrixOperands(const Statement& statement,
                                        const std::string& indent);

    Vector<int> countUsesOver(const Vector<int>& roots) const;
    Vector<int> blockUses(int block) const;
    std::string define(const Vector<int>& roots,
                       const std::string& indent,
                       const Vector<int>& uses,
                       Vector<int>& open);
    std::string bind(int node, const std::string& indent, Vector<int>& open);
    std::string holdTheRecord(const Statement& statement,
                              const std::string& indent,
                              Vector<int>& open);
    std::string holdTheVector(const Statement& statement,
                              const std::string& indent,
                              Vector<int>& open);
    std::string holdOperand(int node,
                            int slot,
                            const std::string& indent,
                            Vector<int>& open);
    bool readsSlot(int node, int slot);
    bool hasNothingStale(bool sharedMoved) const;
    void retire(Vector<int>& open);
    void dropStale(const Statement& statement, Vector<int>& open);

    // Held by the emitter rather than by the walk, so that naming a stage costs
    // one buffer instead of one per name per statement.
    VisitSet visited;
    Vector<char> written;
    Vector<char> buffersWritten;
};
} // namespace eacp::GPU::ShaderEmission
