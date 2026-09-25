#pragma once

#include "Lanes.h"

#include <eacp/GPU/Codegen/ShaderGraph.h>
#include <eacp/GPU/Frame/ComputePass.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

// A compute graph decoded once; immutable, and holds no pointer to the graph.

namespace eacp::GPU::CpuCompute
{
enum class Op : std::uint8_t
{
    Leaf,
    Construct,
    Swizzle,
    Select,
    CopyBits,
    NegF,
    NegI,
    BitNot,
    AddF,
    SubF,
    MulF,
    DivF,
    AddI,
    SubI,
    MulI,
    DivU,
    DivS,
    RemU,
    RemS,
    And,
    Or,
    Xor,
    EqMask,
    Shl,
    ShrU,
    ShrS,
    CmpF,
    CmpU,
    CmpS,
    MatVec,
    VecMat,
    MatMat,
    BufferRead,
    BufferVectorRead,
    ArrayRead,
    SharedRead,
    AtomicLoad,
    UnaryMath,
    AbsS,
    MinF,
    MaxF,
    MinU,
    MaxU,
    MinS,
    MaxS,
    Pow,
    Atan2,
    Step,
    Clamp,
    Mix,
    Smoothstep,
    Dot,
    Length,
    Distance,
    Cross,
    Normalize,
    Reflect,
    Refract,
    FaceForward,
    Transpose,
    Determinant,
    All,
    Any,
    FloatFromU,
    FloatFromS,
    FloatFromMask,
    IntFromF,
    UIntFromF,
    IntFromMask,
    Helper
};

// The eacp* shader helpers, run through Helpers.h.
enum class HelperFunction : std::uint8_t
{
    Erf,
    Erfc,
    SaturatingTanh,
    UnpackHalf2,
    PackHalf2,
    ReadHalf,
    UnpackBFloat16x2,
    PackBFloat16x2,
    ReadBFloat16,
    ReadInt8,
    ReadUInt8,
    UnpackInt8x4,
    UnpackUInt8x4,
    UnpackInt4x4,
    UnpackUInt4x4,
    PackInt8x4,
    PackUInt8x4
};

enum class Relation : std::uint8_t
{
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Equal,
    NotEqual
};

enum class MathFunction : std::uint8_t
{
    Sin,
    Cos,
    Tan,
    Asin,
    Acos,
    Atan,
    Sinh,
    Cosh,
    Tanh,
    Exp,
    Exp2,
    Log,
    Log2,
    Log10,
    Sqrt,
    Rsqrt,
    Floor,
    Ceil,
    Trunc,
    Round,
    Fract,
    Sign,
    Abs
};

// A kernel with no group-scope feature runs several consecutive x groups as
// one batch of about targetBatchLanes lanes; 1 keeps a group per batch. A
// batch whose workspace would pass maxBatchBytes is halved until it fits or
// is one group, since every per-lane row - array constants included - grows
// with it.
struct PlanOptions
{
    static constexpr int defaultBatchLanes = 1024;
    static constexpr std::size_t defaultBatchBytes = 192 * 1024;

    int targetBatchLanes = defaultBatchLanes;
    std::size_t maxBatchBytes = defaultBatchBytes;
};

class Plan
{
public:
    static constexpr int maxSlots = ComputePass::maxBufferSlots;

    // Metal's setBytes limit: the uniforms tightly packed, a word per scalar.
    static constexpr int maxUniformWords = 1024;

    struct Node
    {
        Op op = Op::Leaf;
        std::uint8_t sub = 0;
        std::uint8_t components = 1;
        std::uint8_t order = 0;
        std::array<std::uint8_t, 4> swizzle {};
        int argBegin = 0;
        int argCount = 0;
        int immediate = -1;
        std::uint32_t scratch = 0;
        bool used = false;

        // A buffer read whose index is lane 0's plus lane * rampScale on every
        // real lane, wrapping, with the batch spanning less than 2^32 elements.
        bool ramp = false;
        Word rampScale = 0;

        // An unsigned / or % whose scalar divisor is the same on every lane.
        bool invariantDivisor = false;
    };

    struct Step
    {
        StatementKind kind = StatementKind::Assign;
        int value = -1;
        int index = -1;
        int slot = -1;
        int buffer = -1;
        int body = -1;
        int elseBody = -1;
        int scheduleBegin = 0;
        int scheduleEnd = 0;
        bool bodiesJumpOut = false;
        GroupReduction reduction = GroupReduction::Sum;
        ReductionScope scope = ReductionScope::Group;
        ValueType type = ValueType::Float;
        int stride = -1;
        int left = -1;
        int right = -1;
        SimdMatrixMemory memory = SimdMatrixMemory::Shared;
        SimdMatrixElement element = SimdMatrixElement::Float;

        // A store whose index is a ramp (as Node::ramp) that never gives two
        // (lane, component) pairs the same element.
        bool ramp = false;
        Word rampScale = 0;
    };

    struct BlockRange
    {
        int begin = 0;
        int end = 0;
    };

    struct Range
    {
        int begin = 0;
        int end = 0;
    };

    struct ArrayLayout
    {
        int elementBegin = 0;
        int elementCount = 0;
        int components = 1;
        std::uint32_t storage = 0;
        Range schedule;
        bool used = false;
    };

    struct Variable
    {
        int components = 1;
        std::uint32_t storage = 0;
    };

    // Element e, component c of a shared array is the word at
    // storage + e * components + c: one copy per group, not per lane.
    struct SharedLayout
    {
        int elements = 0;
        int components = 1;
        std::uint32_t storage = 0;
    };

    struct LeafNode
    {
        int node = -1;
        int index = 0;
    };

    struct ConstantNode
    {
        int node = -1;
        Word word = 0;
    };

    using Options = PlanOptions;

    explicit Plan(const ShaderGraph& graph, Options options = {});

    bool isValid() const { return failure.empty(); }
    const std::string& reason() const { return failure; }

    // Unique to each plan built, and shared by its copies, which have its
    // layout: what a Workspace or a PreparedDispatch is checked against.
    std::uint64_t serial() const { return planSerial; }

    DispatchRank rank() const { return dispatchRank; }
    ThreadGroupShape groupShape() const { return shape; }

    // One group's lanes, and a batch's: groupsPerBatch() groups back to back.
    int lanes() const { return laneCount; }
    int groupsPerBatch() const { return groupsInBatch; }
    int batchLanes() const { return laneCount * groupsInBatch; }
    int laneStride() const { return stride; }
    bool guardsBounds() const { return boundsGuard; }

    int storageSlotCount() const { return slotCount; }
    BufferAccess access(int slot) const;
    ValueType element(int slot) const;
    bool referencesSlot(int slot) const;

    int uniformCount() const { return uniformTypes.size(); }
    ValueType uniformType(int slot) const;

    // Uniform slot s is the byteSize words at uniformOffset(s) of a block of
    // uniformBlockWords(), packed in slot order.
    int uniformOffset(int slot) const { return uniformOffsets[slot]; }
    int uniformBlockWords() const { return uniformWordCount; }

    std::size_t footprintBytes() const;
    std::size_t totalWords() const { return wordCount; }

    const Node& node(int id) const { return nodes[id]; }
    int argument(const Node& node, int which) const
    {
        return arguments[node.argBegin + which];
    }

    const Vector<int>& schedule() const { return scheduleList; }

    // The nodes every lane of a dispatch, or of a group, computes alike: run
    // once after the uniforms are read, and once per group after its ids.
    const Range& dispatchSchedule() const { return dispatchRange; }
    const Range& groupSchedule() const { return groupRange; }

    int stepCount() const { return steps.size(); }
    const Step& step(int id) const { return steps[id]; }
    const BlockRange& block(int id) const { return blocks[id]; }
    int blockStep(int position) const { return blockSteps[position]; }
    int rootBlock() const { return 0; }

    const Vector<ArrayLayout>& arrays() const { return arrayLayouts; }
    int arrayElement(const ArrayLayout& array, int which) const
    {
        return arrayElements[array.elementBegin + which];
    }

    const Vector<Variable>& variables() const { return variableLayouts; }
    const Vector<ConstantNode>& constants() const { return constantNodes; }
    const Vector<LeafNode>& uniformNodes() const { return uniformLeaves; }
    const Vector<LeafNode>& extentNodes() const { return extentLeaves; }
    const Vector<LeafNode>& threadIdNodes() const { return threadIdLeaves; }
    const Vector<LeafNode>& groupIdNodes() const { return groupIdLeaves; }
    const Vector<int>& simdGroupIndexNodes() const { return simdGroupLeaves; }

    const Vector<SharedLayout>& sharedArrays() const { return sharedLayouts; }
    std::uint32_t sharedWords() const { return sharedOffset; }
    int sharedWordCount() const { return sharedCount; }

    std::uint32_t reductionScratch() const { return reductionOffset; }

    // Fragment f of SIMD group g is a dense row-major 8x8 at
    // fragment(f, g): held whole per SIMD group, not spread over its lanes.
    static constexpr int fragmentElements = simdMatrixSize * simdMatrixSize;

    int simdGroupCount() const
    {
        return (laneCount + simdGroupWidth - 1) / simdGroupWidth;
    }

    std::uint32_t fragment(int which, int simdGroup) const
    {
        return fragmentOffset
               + static_cast<std::uint32_t>((which * simdGroupCount() + simdGroup)
                                            * fragmentElements);
    }

    std::uint32_t fragmentWords() const { return fragmentOffset; }
    int fragmentWordCount() const { return fragmentCount; }

    int maskFrameCount() const { return 1 + 2 * nesting; }
    std::uint32_t maskFrame(int frame) const
    {
        return maskOffset + static_cast<std::uint32_t>(frame * stride);
    }

    std::uint32_t localCoordinates(int axis) const
    {
        return localOffset + static_cast<std::uint32_t>(axis * stride);
    }

    std::uint32_t realLanes() const { return realLaneOffset; }

    // Per lane of a batch: which of its groups the lane is in, and its x
    // offset from the batch's first thread (groupInBatch * shape.x + local x).
    std::uint32_t groupInBatch() const { return groupInBatchOffset; }
    std::uint32_t batchX() const { return batchXOffset; }

private:
    friend class PlanBuilder;

    Plan() = default;

    bool fitsBatchBudget(const Options& options) const;

    std::string failure;
    std::uint64_t planSerial = 0;
    DispatchRank dispatchRank = DispatchRank::OneD;
    ThreadGroupShape shape;
    int laneCount = 0;
    int groupsInBatch = 1;
    int stride = 0;
    bool boundsGuard = true;

    int slotCount = 0;
    std::array<BufferAccess, maxSlots> slotAccess {};
    std::array<ValueType, maxSlots> slotElement {};
    std::array<bool, maxSlots> slotReferenced {};
    Vector<ValueType> uniformTypes;
    Vector<int> uniformOffsets;
    int uniformWordCount = 0;

    Vector<Node> nodes;
    Vector<int> arguments;
    Vector<int> scheduleList;
    Range dispatchRange;
    Range groupRange;
    Vector<Step> steps;
    Vector<BlockRange> blocks;
    Vector<int> blockSteps;
    Vector<ArrayLayout> arrayLayouts;
    Vector<int> arrayElements;
    Vector<Variable> variableLayouts;
    Vector<ConstantNode> constantNodes;
    Vector<LeafNode> uniformLeaves;
    Vector<LeafNode> extentLeaves;
    Vector<LeafNode> threadIdLeaves;
    Vector<LeafNode> groupIdLeaves;
    Vector<int> simdGroupLeaves;
    Vector<SharedLayout> sharedLayouts;

    int nesting = 0;
    int sharedCount = 0;
    std::uint32_t sharedOffset = 0;
    std::uint32_t reductionOffset = 0;
    std::uint32_t fragmentOffset = 0;
    int fragmentCount = 0;
    std::uint32_t maskOffset = 0;
    std::uint32_t localOffset = 0;
    std::uint32_t realLaneOffset = 0;
    std::uint32_t groupInBatchOffset = 0;
    std::uint32_t batchXOffset = 0;
    std::size_t wordCount = 0;
};
} // namespace eacp::GPU::CpuCompute
