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

class Plan
{
public:
    static constexpr int maxSlots = ComputePass::maxBufferSlots;
    static constexpr int uniformWordsPerSlot = 16;

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

    explicit Plan(const ShaderGraph& graph);

    bool isValid() const { return failure.empty(); }
    const std::string& reason() const { return failure; }

    DispatchRank rank() const { return dispatchRank; }
    ThreadGroupShape groupShape() const { return shape; }
    int lanes() const { return laneCount; }
    int laneStride() const { return stride; }
    bool guardsBounds() const { return boundsGuard; }

    int storageSlotCount() const { return slotCount; }
    BufferAccess access(int slot) const;
    ValueType element(int slot) const;
    bool referencesSlot(int slot) const;

    int uniformCount() const { return uniformTypes.size(); }
    ValueType uniformType(int slot) const;

    std::size_t footprintBytes() const;
    std::size_t totalWords() const { return wordCount; }

    const Node& node(int id) const { return nodes[id]; }
    int argument(const Node& node, int which) const
    {
        return arguments[node.argBegin + which];
    }

    const Vector<int>& schedule() const { return scheduleList; }
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
    std::uint32_t uniformWords() const { return uniformOffset; }

private:
    friend class PlanBuilder;

    std::string failure;
    DispatchRank dispatchRank = DispatchRank::OneD;
    ThreadGroupShape shape;
    int laneCount = 0;
    int stride = 0;
    bool boundsGuard = true;

    int slotCount = 0;
    std::array<BufferAccess, maxSlots> slotAccess {};
    std::array<ValueType, maxSlots> slotElement {};
    std::array<bool, maxSlots> slotReferenced {};
    Vector<ValueType> uniformTypes;

    Vector<Node> nodes;
    Vector<int> arguments;
    Vector<int> scheduleList;
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
    std::uint32_t uniformOffset = 0;
    std::size_t wordCount = 0;
};
} // namespace eacp::GPU::CpuCompute
