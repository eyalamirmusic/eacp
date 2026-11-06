#pragma once

#include "ShaderExpressionPrinter.h"
#include "ShaderGraphAnalysis.h"

#include <functional>
#include <string>

namespace eacp::GPU::ShaderEmission
{
class StageEmitter;

// What a Return statement prints at `indent`, after the locals its roots share
// are named: a kernel's bare return, or a render function's outputs.
using ReturnPrinter =
    std::function<std::string(const StageEmitter&, const std::string& indent)>;

// Any operation evaluated more than once becomes a tN local. A name is given
// up once a statement writes something it read, and a loop condition takes none.
class StageEmitter
{
public:
    StageEmitter(const ShaderGraph& graphToUse,
                 Backend backend,
                 Vector<int> attributeVaryings = {});

    const ShaderGraph& getGraph() const { return printer.graph; }

    std::string defineLocalsFor(const Vector<int>& roots, std::string_view indent);
    std::string declareArrays(const Vector<int>& roots, std::string_view indent);
    std::string emitBlock(int block, std::string_view indentToUse);

    Vector<int> locals; // node id -> local index, -1 = inline
    ExprPrinter printer;
    int localCount = 0;

    // The outputs a Return evaluates where it stands, and how it prints them.
    Vector<int> returnRoots;
    ReturnPrinter printReturn = [](const StageEmitter&, const std::string& indent)
    { return indent + "return;\n"; };

private:
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

    struct SimdMatrixPatch
    {
        std::string memory;
        std::string offset;
        std::string stride;
    };

    struct AtomicCall
    {
        std::string element;
        std::string operand;
        std::string type;
        std::string name;
        std::string_view indent;
    };

    static StatementEmitFunction findStatementEmitter(StatementKind kind);

    std::string emitStatement(const Statement& statement,
                              const std::string& indent,
                              const Vector<int>& uses,
                              Vector<int>& open);
    std::string emitLoop(const Statement& statement,
                         const std::string& indent,
                         Vector<int>& open);
    std::string emitLoopWithContinuing(const Statement& statement,
                                       const std::string& indent);
    std::string printLoopBlock(int block, const std::string& indent);
    std::string emitVariableWrite(const Statement& statement, StatementScope& scope);
    std::string emitArrayWrite(const Statement& statement, StatementScope& scope);
    std::string emitIf(const Statement& statement, StatementScope& scope);
    std::string emitBreak(const Statement& statement, StatementScope& scope);
    std::string emitContinue(const Statement& statement, StatementScope& scope);
    std::string emitBreakIf(const Statement& statement, StatementScope& scope);
    std::string emitSwitch(const Statement& statement, StatementScope& scope);
    std::string printClause(const Statement& clause,
                            ValueType selector,
                            const std::string& indent);
    std::string printLoopJump(int code, const std::string& indent) const;
    std::string
        printJumpsOutOfSwitch(int flag, int jumps, const std::string& indent) const;
    std::string emitReturn(const Statement& statement, StatementScope& scope);
    bool returnReadsOnlyVariablesInScope();
    bool readsOnlyVariablesInScope(int node);
    std::string emitDiscard(const Statement& statement, StatementScope& scope);
    std::string emitStore(const Statement& statement, StatementScope& scope);
    std::string emitVectorStore(const Statement& statement, StatementScope& scope);
    std::string emitAtomic(const Statement& statement, StatementScope& scope);
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
    std::string emitStorageStore(const Statement& statement, StatementScope& scope);
    std::string emitNothing(const Statement& statement, StatementScope& scope);

    std::string defineLocalsInScope(const Vector<int>& roots, StatementScope& scope);
    std::string printBody(int block, const std::string& indent);

    std::string formatCompareExchange(const Statement& statement,
                                      const AtomicCall& call);

    std::string formatGroupReduction(const Statement& statement,
                                     const StatementScope& scope);
    std::string formatMetalGroupReduction(const Statement& statement,
                                          const StatementScope& scope);
    std::string formatTreeGroupReduction(const Statement& statement,
                                         const StatementScope& scope);

    bool isMetal() const;
    std::string formatSimdMatrixDeclaration(int slot) const;
    std::string formatSimdMatrixFill(const Statement& statement,
                                     const StatementScope& scope);
    std::string formatSimdMatrixLaneElement(const Statement& statement,
                                            const SimdMatrixPatch& patch,
                                            int which) const;
    std::string formatSimdMatrixTransfer(const Statement& statement,
                                         const StatementScope& scope);
    std::string formatMetalSimdMatrixTransfer(const Statement& statement,
                                              const SimdMatrixPatch& patch,
                                              const StatementScope& scope);
    std::string formatSimdMatrixMultiplyAdd(const Statement& statement,
                                            const StatementScope& scope);
    std::string stageSimdMatrixOperands(const Statement& statement,
                                        const StatementScope& scope);

    Vector<char> findUsedArrays(const Vector<int>& roots) const;
    std::string printArrayDeclaration(int slot, std::string_view indent) const;
    bool isRecordStillUnnamed(const Statement& statement) const;
    Vector<int> countUsesOver(const Vector<int>& roots) const;
    Vector<int> countBlockUses(int block) const;
    std::string defineLocals(const Vector<int>& roots,
                             const std::string& indent,
                             const Vector<int>& uses,
                             Vector<int>& open);
    std::string bindLocal(int node, const std::string& indent, Vector<int>& open);
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
    bool doesReadSlot(int node, int slot);
    bool hasNothingStale(bool sharedMoved) const;
    void retireLocals(Vector<int>& open);
    void dropStaleLocals(const Statement& statement, Vector<int>& open);

    VisitSet visited;
    Vector<char> written;
    Vector<char> buffersWritten;

    // The constructs a break or continue would land in, innermost last: a loop
    // (jumpsToLoop), or a switch, by the flag that carries a loop jump out of
    // it - or noJumpFlag where none of its clauses jumps.
    static constexpr int jumpsToLoop = -1;
    static constexpr int noJumpFlag = -2;
    Vector<int> jumpTargets;
    int controlCount = 0;

    // Per variable: whether its declaration was printed in a block still open.
    Vector<char> variablesInScope;
};
} // namespace eacp::GPU::ShaderEmission
