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

inline bool isFloatFamily(ValueType type)
{
    return !isIntegerFamily(type) && !isBoolean(type);
}

inline bool isScalarInteger(ValueType type)
{
    return componentCount(type) == 1 && isIntegerFamily(type);
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

// Plain ints, named for what they index.
using NodeId = int;
using StepId = int;
using BlockId = int;
using ScheduleTime = int;

// Where a node is evaluated: a step's id, or one of PlanBuilder's sites.
using Site = int;

class PlanBuilder
{
public:
    static constexpr std::int64_t maxLanes = std::int64_t {1} << 20;
    static constexpr std::size_t maxWords = std::numeric_limits<std::int32_t>::max();

    static constexpr Site arraySite = -1;
    static constexpr Site dispatchSite = -2;
    static constexpr Site groupSite = -3;
    static constexpr Site unhoisted = -4;

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

    struct BlockResult
    {
        int id = -1;
        bool jumpsOut = false;
    };

    struct BlockWalk
    {
        int loopDepth = 0;
        int depth = 0;
        int pinned = -1;
        bool jumpsOut = false;
    };

    struct StepDraft
    {
        BlockWalk& walk;
        Plan::Step step;
        PendingSchedule schedule;
    };

    struct WalkEntry
    {
        int node = -1;
        int next = 0;
    };

    using StepBuilder = bool (PlanBuilder::*)(const Statement&, StepDraft&);
    using NodeDecoder = void (PlanBuilder::*)(NodeId, Plan::Node&, const Expr&);

    bool failed() const { return !plan.failure.empty(); }
    void failWith(std::string reason);
    void failStatementWith(const Statement&, std::string_view what);
    void rejectNodeWith(NodeId id, std::string_view why);

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
    void rejectNode(NodeId id, std::string_view pattern, const Args&... args)
    {
        rejectNodeWith(id, formatReason(pattern, args...));
    }

    // PlanStatements.cpp
    bool checkGraph();
    bool checkThreadGroupShape();
    bool packUniforms();
    bool enterBlock(BlockId graphBlock);
    bool checkStatement(int statement);
    BlockResult buildBlock(BlockId graphBlock, int loopDepth, int depth);
    bool buildStep(const Statement&, StepId stepId, BlockWalk&);
    bool buildVariableStep(const Statement&, StepDraft&);
    bool buildIfStep(const Statement&, StepDraft&);
    bool buildLoopStep(const Statement&, StepDraft&);
    bool buildJumpStep(const Statement&, StepDraft&);
    bool buildStoreStep(const Statement&, StepDraft&);
    bool buildVectorStoreStep(const Statement&, StepDraft&);
    bool buildTextureStoreStep(const Statement&, StepDraft&);
    bool buildSharedStoreStep(const Statement&, StepDraft&);
    bool buildBarrierStep(const Statement&, StepDraft&);
    bool buildReductionStep(const Statement&, StepDraft&);
    bool buildAtomicAddStep(const Statement&, StepDraft&);
    bool buildSimdMatrixFillStep(const Statement&, StepDraft&);
    bool buildSimdMatrixTransferStep(const Statement&, StepDraft&);
    bool buildSimdMatrixMultiplyAddStep(const Statement&, StepDraft&);
    bool checkSimdMatrixTransfer(const Statement&);
    bool checkSimdMatrixMultiplyAdd(const Statement&);
    bool checkNode(NodeId node);
    bool checkVariable(int slot);
    bool checkVariableValue(const Statement&);
    bool checkCondition(NodeId node);
    bool checkSlot(int slot);
    bool checkStore(const Statement&, int width);
    bool checkIndexAndValue(const Statement&);
    bool checkSharedSlot(int slot);
    bool checkFragment(const Statement&, int fragment, std::string_view role);
    bool checkFloatFragment(const Statement&, int fragment);
    bool checkWholeSimdGroups(const Statement&);
    bool checkScalarInteger(const Statement&, NodeId node, std::string_view role);
    bool checkSimdMatrixElement(const Statement&);
    bool checkSimdMatrixMemory(const Statement&);
    bool checkSimdMatrixStorage(const Statement&);

    // PlanNodes.cpp
    void markReachable();
    Vector<int> collectReachableRoots() const;
    bool markNodeReachable(NodeId id, Vector<int>& stack);
    bool markArrayReachable(int array, Vector<int>& stack);
    bool pushReachable(const Vector<int>& ids, Vector<int>& stack);
    ValueType getArgumentType(const Expr&, int which) const;
    int getArgumentComponents(const Expr&, int which) const;
    bool requireArguments(NodeId id, int count);
    bool requireBroadcastable(NodeId id);
    bool requireScalarIndex(NodeId id, const Expr&);
    bool requireUnaryFloatVector(NodeId id, const Expr&);
    void decodeNode(NodeId id);
    void rejectRenderInput(NodeId id, Plan::Node&, const Expr&);
    void rejectTexture(NodeId id, Plan::Node&, const Expr&);
    void decodeLeaf(NodeId id, Plan::Node&, const Expr&);
    void decodeIdLeaf(NodeId id, Plan::Node&, const Expr&);
    void decodeUniform(NodeId id, Plan::Node&, const Expr&);
    void decodeVarRead(NodeId id, Plan::Node&, const Expr&);
    void decodeSelect(NodeId id, Plan::Node&, const Expr&);
    void decodeBufferRead(NodeId id, Plan::Node&, const Expr&);
    void decodeArrayRead(NodeId id, Plan::Node&, const Expr&);
    void decodeAtomicLoad(NodeId id, Plan::Node&, const Expr&);
    void decodeSharedRead(NodeId id, Plan::Node&, const Expr&);
    void decodeConstruct(NodeId id, Plan::Node&, const Expr&);
    void decodeSwizzle(NodeId id, Plan::Node&, const Expr&);
    void decodeUnary(NodeId id, Plan::Node&, const Expr&);
    void decodeBinary(NodeId id, Plan::Node&, const Expr&);
    void decodeShift(NodeId id, Plan::Node&, const Expr&);
    void decodeCompare(NodeId id, Plan::Node&, const Expr&);
    void decodeBoolCompare(NodeId id, Plan::Node&, const Expr&);
    void decodeMul(NodeId id, Plan::Node&, const Expr&);
    void decodeCall(NodeId id, Plan::Node&, const Expr&);
    bool decodeNamedCall(NodeId id, Plan::Node&, const Expr&);
    void decodeConversionCall(NodeId id, Plan::Node&, const Expr&);
    void decodeMath(NodeId id, Plan::Node&, const Expr&, MathFunction);
    void decodeBroadcast(NodeId id, Plan::Node&, const Expr&, Op);
    void decodeAbs(NodeId id, Plan::Node&, const Expr&);
    void decodeMinMax(NodeId id, Plan::Node&, const Expr&);
    void decodeMatrixFunction(NodeId id, Plan::Node&, const Expr&);
    void decodeBoolReduction(NodeId id, Plan::Node&, const Expr&);
    void decodeBitcast(NodeId id, Plan::Node&, const Expr&);
    void decodeHelper(NodeId id, Plan::Node&, const Expr&);
    void decodePackedHelper(NodeId id, Plan::Node&, const Expr&);
    void decodeGeometric(NodeId id, Plan::Node&, const Expr&, Op);
    bool hasGeometricWidths(const Expr&, Op, int width) const;
    void decodeConversion(NodeId id, Plan::Node&, const Expr&);

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
    bool hasOwnScratch(NodeId id) const;
    bool isPooled(NodeId id) const;
    int getLoopEnd(StepId step) const;
    int calculateLoopExtent(Site site, Site definedAt) const;
    void notePooledUse(NodeId id, ScheduleTime time, Site site);
    void noteScheduled(Site, const Plan::Range&);
    void noteLiveRanges();
    void noteArrayLiveRanges(const Plan::ArrayLayout&);
    void noteStepLiveRanges(StepId stepId);
    void colourPooledNodes();
    void colourDefinedNodes(Vector<int> byDefinition);

    // PlanSchedule.cpp
    bool isScheduleLeaf(NodeId id, Site site, NodeId pinned) const;
    template <typename Emit>
    void visit(NodeId root, Site site, NodeId pinned, Emit emit);
    template <typename Emit>
    void visitArrayElements(const Plan::ArrayLayout& layout, Emit emit);
    void describeArrays();
    int mapBlock(BlockId block, int depth);
    void mapBlocks();
    int findCommonBlock(BlockId a, BlockId b) const;
    PlanLevel calculateNodeLevel(NodeId id) const;
    PlanLevel joinArgumentLevels(NodeId id, const Plan::Node&) const;
    void computeLevels();
    void countUse(NodeId id, Site site);
    void collectUses();
    int findSiteWithin(Site site, BlockId block) const;
    void chooseHoistSites();
    void appendToSchedule(NodeId id) { plan.scheduleList.add(id); }
    void scheduleLevel(PlanLevel level, Site site, Plan::Range& range);
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
    int stamp = 0;
    std::size_t cursor = 0;
};
} // namespace eacp::GPU::CpuCompute
