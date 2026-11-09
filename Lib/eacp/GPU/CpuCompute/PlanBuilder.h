#pragma once

#include "Plan.h"

#include <eacp/Core/Utils/Format.h>

#include <cstdint>
#include <limits>

// Builds a Plan from a graph: statements and blocks (PlanStatements.cpp), node
// decoding (PlanNodes.cpp), schedules (PlanSchedule.cpp) and scratch layout
// (PlanLayout.cpp).

namespace eacp::GPU::CpuCompute
{
// How far a node's value is shared: by every lane of the dispatch, by every
// lane of a group, or by none. Ordered, so the join of two is the larger.
enum class PlanLevel : std::uint8_t
{
    Dispatch,
    Group,
    Lane
};

inline bool isIntegerFamily(ValueType type)
{
    return isUnsignedInteger(type) || isSignedInteger(type);
}

// Both float families: an f16 lane is held as the float it rounds to.
inline bool isFloatFamily(ValueType type)
{
    switch (type)
    {
        case ValueType::Float:
        case ValueType::Float2:
        case ValueType::Float3:
        case ValueType::Float4:
        case ValueType::Half:
        case ValueType::Half2:
        case ValueType::Half3:
        case ValueType::Half4:
            return true;
        default:
            return isMatrix(type);
    }
}

inline bool isScalarInteger(ValueType type)
{
    return componentCount(type) == 1 && isIntegerFamily(type);
}

// How many words a value takes in a lane: its components, a struct's
// every field's laid end to end.
inline int getFlatComponents(const ShaderGraph& graph, ValueType type, int structId)
{
    if (type == ValueType::Struct)
        return structId >= 0 ? graph.structType(structId).components.size() : 0;

    return componentCount(type);
}

// Fixed text is taken as it is; only a pattern given values is formatted.
template <typename... Args>
std::string formatReason(std::string_view pattern, const Args&... args)
{
    if constexpr (sizeof...(Args) == 0)
        return std::string {pattern};
    else
        return Strings::format(pattern, args...);
}

struct BitHelperName;

class PlanBuilder
{
public:
    static constexpr std::int64_t maxLanes = std::int64_t {1} << 20;
    static constexpr std::size_t maxWords = std::numeric_limits<std::int32_t>::max();

    // Where a node is evaluated: a step's id, or one of these.
    static constexpr int arraySite = -1;
    static constexpr int dispatchSite = -2;
    static constexpr int groupSite = -3;
    static constexpr int unhoisted = -4;

    static constexpr int axisCount = 3;

    PlanBuilder(Plan& planToFill,
                const ShaderGraph& graphToRead,
                Plan::Options optionsToUse,
                int mostGroupsPerBatch);

    void build();

    static Word getConstantWord(const Expr& expr);

private:
    struct PendingSchedule
    {
        Array<int, 3> roots {-1, -1, -1};
        int pinned = -1;
    };

    // jumpsOut: something in the block may leave the frame it runs in empty -
    // a jump, or a return; returns: a return, which no loop stops.
    struct BlockResult
    {
        int id = -1;
        bool jumpsOut = false;
        bool returns = false;
    };

    struct BlockWalk
    {
        int graphBlock = -1;
        int loopDepth = 0;
        int depth = 0;
        int pinned = -1;
        bool jumpsOut = false;
        bool returns = false;
    };

    struct StepDraft
    {
        BlockWalk& walk;
        int statementId = -1;
        Plan::Step step;
        PendingSchedule schedule;
    };

    struct WalkEntry
    {
        int node = -1;
        int next = 0;
    };

    using StepBuilder = bool (PlanBuilder::*)(const Statement&, StepDraft&);
    using NodeDecoder = void (PlanBuilder::*)(int, Plan::Node&, const Expr&);

    bool failed() const { return !plan.failure.empty(); }
    void failWith(std::string reason);
    void failStatementWith(const Statement&, std::string_view what);
    void rejectNodeWith(int id, std::string_view why);

    template <typename... Args>
    void fail(std::string_view pattern, const Args&... args)
    {
        failWith(formatReason(pattern, args...));
    }

    template <typename... Args>
    void failStatement(const Statement& statement,
                       std::string_view pattern,
                       const Args&... args)
    {
        failStatementWith(statement, formatReason(pattern, args...));
    }

    template <typename... Args>
    void rejectNode(int id, std::string_view pattern, const Args&... args)
    {
        rejectNodeWith(id, formatReason(pattern, args...));
    }

    // PlanStatements.cpp
    bool checkGraph();
    void recordStructSlot(int slot);
    int nodeComponents(const Expr& expr) const;
    int variableComponents(int slot) const;
    bool checkThreadGroupShape();
    bool packUniforms();
    bool enterBlock(int graphBlock);
    bool checkStatement(int statement);
    BlockResult buildBlock(int graphBlock, int loopDepth, int depth);
    bool buildStep(const Statement&, int statementId, int stepId, BlockWalk&);
    bool buildVariableStep(const Statement&, StepDraft&);
    bool buildIfStep(const Statement&, StepDraft&);
    bool buildLoopStep(const Statement&, StepDraft&);
    bool buildJumpStep(const Statement&, StepDraft&);
    bool buildBreakIfStep(const Statement&, StepDraft&);
    bool buildSwitchStep(const Statement&, StepDraft&);
    bool buildCaseStep(const Statement&, StepDraft&);
    bool buildReturnStep(const Statement&, StepDraft&);
    bool buildDiscardStep(const Statement&, StepDraft&);
    bool buildCallStep(const Statement&, StepDraft&);
    bool buildStoreStep(const Statement&, StepDraft&);
    bool buildVectorStoreStep(const Statement&, StepDraft&);
    bool buildTextureStoreStep(const Statement&, StepDraft&);
    bool buildSharedStoreStep(const Statement&, StepDraft&);
    bool buildBarrierStep(const Statement&, StepDraft&);
    bool buildReductionStep(const Statement&, StepDraft&);
    bool buildAtomicStep(const Statement&, StepDraft&);
    bool buildSimdMatrixFillStep(const Statement&, StepDraft&);
    bool buildSimdMatrixTransferStep(const Statement&, StepDraft&);
    bool buildSimdMatrixMultiplyAddStep(const Statement&, StepDraft&);
    bool checkSimdMatrixTransfer(const Statement&);
    bool checkSimdMatrixMultiplyAdd(const Statement&);
    bool checkNode(int node);
    bool checkVariable(int slot);
    int fieldComponent(int structId, int field) const;
    bool isField(int structId, int field) const;
    bool checkVariableValue(const Statement&);
    bool checkElementIndex(const Statement&);
    bool checkBreakIf(const Statement&,
                      int statementId,
                      int graphBlock,
                      int loopDepth);
    bool isValidCase(int statementId) const;
    bool hasDefaultClause(int cases) const;
    bool checkSwitchLabels(const Statement&, ValueType selector);
    bool checkSwitch(const Statement&);
    bool checkCondition(int node);
    bool checkSlot(int slot);
    bool isStructSlot(int slot) const;
    bool checkStructStore(const Statement&);
    bool checkStore(const Statement&, int width);
    bool checkIndexAndValue(const Statement&);
    bool checkSharedSlot(int slot);
    bool checkFragment(const Statement&, int fragment, std::string_view role);
    bool checkFloatFragment(const Statement&, int fragment);
    bool checkWholeSimdGroups(const Statement&);
    bool checkScalarInteger(const Statement&, int node, std::string_view role);
    bool checkSimdMatrixElement(const Statement&);
    bool checkSimdMatrixMemory(const Statement&);
    bool checkSimdMatrixStorage(const Statement&);
    ValueType atomicElement(const Statement&);
    bool checkComparand(const Statement&, ValueType element);
    bool checkAtomic(const Statement&);

    // PlanNodes.cpp
    static Word overrideWord(const PipelineConstant& value);
    bool resolveOverrideWords();
    void markHalfRounding(int id);
    void markReachable();
    Vector<int> collectReachableRoots() const;
    bool markNodeReachable(int id, Vector<int>& stack);
    bool markArrayReachable(int array, Vector<int>& stack);
    bool pushReachable(const Vector<int>& ids, Vector<int>& stack);
    bool requireArguments(int id, int count);
    bool requireBroadcastable(int id);
    bool requireScalarIndex(int id, const Expr&);
    bool requireUnaryFloatVector(int id, const Expr&);
    void decodeNode(int id);
    void rejectRenderInput(int id, Plan::Node&, const Expr&);
    void rejectTexture(int id, Plan::Node&, const Expr&);
    void decodeLeaf(int id, Plan::Node&, const Expr&);
    void decodeIdLeaf(int id, Plan::Node&, const Expr&);
    void decodeUniform(int id, Plan::Node&, const Expr&);
    void decodeVarRead(int id, Plan::Node&, const Expr&);
    void decodeMatrixColumn(int id, Plan::Node&, const Expr&);
    void decodeOverride(int id, Plan::Node&, const Expr&);
    void decodeBuiltIn(int id, Plan::Node&, const Expr&);
    void rejectFunctionCall(int id, Plan::Node&, const Expr&);
    void decodeSelect(int id, Plan::Node&, const Expr&);
    void decodeBufferRead(int id, Plan::Node&, const Expr&);
    void decodeStructRead(int id, Plan::Node&, const Expr&);
    void decodeArrayRead(int id, Plan::Node&, const Expr&);
    void decodeAtomicLoad(int id, Plan::Node&, const Expr&);
    void decodeSharedRead(int id, Plan::Node&, const Expr&);
    void decodeConstruct(int id, Plan::Node&, const Expr&);
    void decodeSwizzle(int id, Plan::Node&, const Expr&);
    void decodeUnary(int id, Plan::Node&, const Expr&);
    void decodeBinary(int id, Plan::Node&, const Expr&);
    void decodeShift(int id, Plan::Node&, const Expr&);
    void decodeCompare(int id, Plan::Node&, const Expr&);
    void decodeBoolCompare(int id, Plan::Node&, const Expr&);
    void decodeMul(int id, Plan::Node&, const Expr&);
    void decodeCall(int id, Plan::Node&, const Expr&);
    bool decodeNamedCall(int id, Plan::Node&, const Expr&);
    void decodeConversionCall(int id, Plan::Node&, const Expr&);
    void decodeMath(int id, Plan::Node&, const Expr&, MathFunction);
    void decodeBroadcast(int id, Plan::Node&, const Expr&, Op);
    void decodeAbs(int id, Plan::Node&, const Expr&);
    void decodeMinMax(int id, Plan::Node&, const Expr&);
    void decodeMatrixFunction(int id, Plan::Node&, const Expr&);
    void decodeBoolReduction(int id, Plan::Node&, const Expr&);
    void decodeBitcast(int id, Plan::Node&, const Expr&);
    void decodeHelper(int id, Plan::Node&, const Expr&);
    void decodePackedHelper(int id, Plan::Node&, const Expr&);
    void decodeBitHelper(int id, Plan::Node&, const Expr&, const BitHelperName&);
    void decodeGeometric(int id, Plan::Node&, const Expr&, Op);
    void decodeConversion(int id, Plan::Node&, const Expr&);

    // PlanLayout.cpp
    bool usesAtomics() const;
    bool needsGroupPerBatch() const;
    void chooseBatchWidth();
    void markRamps();
    std::uint32_t allocate(int components);
    std::uint32_t reuseOrAllocate(Vector<std::uint32_t>& freeScratch,
                                  int components);
    void layOutShared();
    void layOutFragments();
    void layOutNodes();
    void layOutVariables();
    void layOutFixedRows();
    void layOut();
    bool hasOwnScratch(int id) const;
    bool isPooled(int id) const;
    int getLoopEnd(int step) const;
    int calculateLoopExtent(int site, int definedAt) const;
    void notePooledUse(int id, int time, int site);
    void noteScheduled(int, const Plan::Range&);
    void noteLiveRanges();
    void noteArrayLiveRanges(const Plan::ArrayLayout&);
    void noteStepLiveRanges(int stepId);
    void colourPooledNodes();
    void colourDefinedNodes(Vector<int> byDefinition);

    // PlanSchedule.cpp
    bool isScheduleLeaf(int id, int site, int pinned) const;
    template <typename Emit>
    void visit(int root, int site, int pinned, Emit emit);
    template <typename Emit>
    void visitArrayElements(const Plan::ArrayLayout& layout, Emit emit);
    void describeArrays();
    int mapBlock(int block, int depth);
    void mapBlocks();
    int findCommonBlock(int a, int b) const;
    PlanLevel calculateNodeLevel(int id) const;
    PlanLevel joinArgumentLevels(int id, const Plan::Node&) const;
    void computeLevels();
    void countUse(int id, int site);
    void collectUses();
    int findSiteWithin(int site, int block) const;
    void chooseHoistSites();
    void appendToSchedule(int id) { plan.scheduleList.add(id); }
    void scheduleLevel(PlanLevel level, int site, Plan::Range& range);
    void scheduleArrays();
    void scheduleSteps();
    void buildSchedules();

    Plan& plan;
    const ShaderGraph& graph;
    Plan::Options options;
    int groupLimit = 1;
    Vector<PendingSchedule> pending;
    Vector<int> marks;
    Vector<char> arrayUsed;
    Vector<char> blockReached;
    Vector<WalkEntry> walk;
    Vector<PlanLevel> levels;
    Vector<int> hoistSites;
    Vector<Vector<int>> hoistedAt;
    Vector<int> stepBlocks;
    Vector<int> blockOwners;
    Vector<int> blockDepths;
    Vector<int> subtreeLasts;
    Vector<int> useCounts;
    Vector<int> firstUses;
    Vector<int> useBlocks;
    Vector<int> definitions;
    Vector<int> lastUses;
    Vector<int> lastDefinitions;

    // Each override's value, by id, as the word a constant of its type holds.
    Vector<Word> overrideWords;

    // The loop depth of the continuing block being built, or -1 outside one,
    // and the clause list of the switch being built, or -1.
    int continuingLoopDepth = -1;
    int casesBlock = -1;
    int stamp = 0;
    std::size_t cursor = 0;
};
} // namespace eacp::GPU::CpuCompute
