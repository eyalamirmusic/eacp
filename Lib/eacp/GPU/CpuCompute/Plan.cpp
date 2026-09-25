#include "Plan.h"

#include <eacp/Core/Utils/Logging.h>

#include <cstdint>
#include <string_view>

namespace eacp::GPU::CpuCompute
{
namespace
{
constexpr int planLaneAlignment = 16;
constexpr std::int64_t planMaxLanes = std::int64_t {1} << 20;
constexpr std::size_t planMaxWords = INT32_MAX;

bool isFloatFamily(ValueType type)
{
    switch (type)
    {
        case ValueType::Float:
        case ValueType::Float2:
        case ValueType::Float3:
        case ValueType::Float4:
        case ValueType::Float2x2:
        case ValueType::Float3x3:
        case ValueType::Float4x4:
            return true;
        default:
            return false;
    }
}

bool isIntegerFamily(ValueType type)
{
    return isUnsignedInteger(type) || isSignedInteger(type);
}

const char* planKindName(ExprKind kind)
{
    switch (kind)
    {
        case ExprKind::Input:
            return "Input";
        case ExprKind::Varying:
            return "Varying";
        case ExprKind::Uniform:
            return "Uniform";
        case ExprKind::Constant:
            return "Constant";
        case ExprKind::Construct:
            return "Construct";
        case ExprKind::Swizzle:
            return "Swizzle";
        case ExprKind::Call:
            return "Call";
        case ExprKind::Unary:
            return "Unary";
        case ExprKind::Binary:
            return "Binary";
        case ExprKind::Compare:
            return "Compare";
        case ExprKind::Select:
            return "Select";
        case ExprKind::VarRead:
            return "VarRead";
        case ExprKind::Mul:
            return "Mul";
        case ExprKind::Sample:
            return "Sample";
        case ExprKind::Fetch:
            return "Fetch";
        case ExprKind::ThreadId:
            return "ThreadId";
        case ExprKind::BufferRead:
            return "BufferRead";
        case ExprKind::BufferVectorRead:
            return "BufferVectorRead";
        case ExprKind::AtomicLoad:
            return "AtomicLoad";
        case ExprKind::ArrayRead:
            return "ArrayRead";
        case ExprKind::LocalId:
            return "LocalId";
        case ExprKind::GroupId:
            return "GroupId";
        case ExprKind::GridExtent:
            return "GridExtent";
        case ExprKind::SharedRead:
            return "SharedRead";
        case ExprKind::SimdGroupIndex:
            return "SimdGroupIndex";
    }

    return "unknown";
}

const char* planStatementName(StatementKind kind)
{
    switch (kind)
    {
        case StatementKind::Declare:
            return "Declare";
        case StatementKind::Assign:
            return "Assign";
        case StatementKind::If:
            return "If";
        case StatementKind::Loop:
            return "Loop";
        case StatementKind::Break:
            return "Break";
        case StatementKind::Continue:
            return "Continue";
        case StatementKind::Store:
            return "Store";
        case StatementKind::VectorStore:
            return "VectorStore";
        case StatementKind::TextureStore:
            return "TextureStore";
        case StatementKind::SharedStore:
            return "SharedStore";
        case StatementKind::Barrier:
            return "Barrier";
        case StatementKind::GroupReduce:
            return "GroupReduce";
        case StatementKind::SimdMatrixFill:
            return "SimdMatrixFill";
        case StatementKind::SimdMatrixLoad:
            return "SimdMatrixLoad";
        case StatementKind::SimdMatrixStore:
            return "SimdMatrixStore";
        case StatementKind::SimdMatrixMultiplyAdd:
            return "SimdMatrixMultiplyAdd";
        case StatementKind::AtomicAdd:
            return "AtomicAdd";
    }

    return "unknown";
}

struct MathName
{
    std::string_view name;
    MathFunction function;
};

constexpr MathName planMathNames[] = {
    {"sin", MathFunction::Sin},     {"cos", MathFunction::Cos},
    {"tan", MathFunction::Tan},     {"asin", MathFunction::Asin},
    {"acos", MathFunction::Acos},   {"atan", MathFunction::Atan},
    {"sinh", MathFunction::Sinh},   {"cosh", MathFunction::Cosh},
    {"tanh", MathFunction::Tanh},   {"exp", MathFunction::Exp},
    {"exp2", MathFunction::Exp2},   {"log", MathFunction::Log},
    {"log2", MathFunction::Log2},   {"log10", MathFunction::Log10},
    {"sqrt", MathFunction::Sqrt},   {"rsqrt", MathFunction::Rsqrt},
    {"floor", MathFunction::Floor}, {"ceil", MathFunction::Ceil},
    {"trunc", MathFunction::Trunc}, {"round", MathFunction::Round},
    {"fract", MathFunction::Fract}, {"sign", MathFunction::Sign}};

struct GeometricName
{
    std::string_view name;
    Op op;
    int arguments;
};

constexpr GeometricName planGeometricNames[] = {{"dot", Op::Dot, 2},
                                                {"length", Op::Length, 1},
                                                {"distance", Op::Distance, 2},
                                                {"normalize", Op::Normalize, 1},
                                                {"cross", Op::Cross, 2},
                                                {"reflect", Op::Reflect, 2},
                                                {"refract", Op::Refract, 3},
                                                {"faceforward", Op::FaceForward, 3}};

struct BroadcastName
{
    std::string_view name;
    Op op;
    int arguments;
};

constexpr BroadcastName planBroadcastNames[] = {{"pow", Op::Pow, 2},
                                                {"atan2", Op::Atan2, 2},
                                                {"step", Op::Step, 2},
                                                {"clamp", Op::Clamp, 3},
                                                {"mix", Op::Mix, 3},
                                                {"smoothstep", Op::Smoothstep, 3}};

// The helpers applied per component, at any float width.
struct ComponentHelperName
{
    std::string_view name;
    HelperFunction function;
};

constexpr ComponentHelperName planComponentHelperNames[] = {
    {"eacpErf", HelperFunction::Erf},
    {"eacpErfc", HelperFunction::Erfc},
    {"eacpSaturatingTanh", HelperFunction::SaturatingTanh}};

// The packed-data helpers, each with the one signature the shader defines.
struct PackedHelperName
{
    std::string_view name;
    HelperFunction function;
    ValueType result;
    int arguments;
    ValueType first;
    ValueType second;
};

constexpr PackedHelperName planPackedHelperNames[] = {
    {"eacpUnpackHalf2",
     HelperFunction::UnpackHalf2,
     ValueType::Float2,
     1,
     ValueType::UInt,
     ValueType::UInt},
    {"eacpPackHalf2",
     HelperFunction::PackHalf2,
     ValueType::UInt,
     1,
     ValueType::Float2,
     ValueType::UInt},
    {"eacpReadHalf",
     HelperFunction::ReadHalf,
     ValueType::Float,
     2,
     ValueType::UInt,
     ValueType::UInt},
    {"eacpUnpackBFloat16x2",
     HelperFunction::UnpackBFloat16x2,
     ValueType::Float2,
     1,
     ValueType::UInt,
     ValueType::UInt},
    {"eacpPackBFloat16x2",
     HelperFunction::PackBFloat16x2,
     ValueType::UInt,
     1,
     ValueType::Float2,
     ValueType::UInt},
    {"eacpReadBFloat16",
     HelperFunction::ReadBFloat16,
     ValueType::Float,
     2,
     ValueType::UInt,
     ValueType::UInt},
    {"eacpReadInt8",
     HelperFunction::ReadInt8,
     ValueType::Float,
     2,
     ValueType::UInt,
     ValueType::UInt},
    {"eacpReadUInt8",
     HelperFunction::ReadUInt8,
     ValueType::Float,
     2,
     ValueType::UInt,
     ValueType::UInt},
    {"eacpUnpackInt8x4",
     HelperFunction::UnpackInt8x4,
     ValueType::Float4,
     1,
     ValueType::UInt,
     ValueType::UInt},
    {"eacpUnpackUInt8x4",
     HelperFunction::UnpackUInt8x4,
     ValueType::Float4,
     1,
     ValueType::UInt,
     ValueType::UInt},
    {"eacpUnpackInt4x4",
     HelperFunction::UnpackInt4x4,
     ValueType::Float4,
     1,
     ValueType::UInt,
     ValueType::UInt},
    {"eacpUnpackUInt4x4",
     HelperFunction::UnpackUInt4x4,
     ValueType::Float4,
     1,
     ValueType::UInt,
     ValueType::UInt},
    {"eacpPackInt8x4",
     HelperFunction::PackInt8x4,
     ValueType::UInt,
     1,
     ValueType::Int4,
     ValueType::UInt},
    {"eacpPackUInt8x4",
     HelperFunction::PackUInt8x4,
     ValueType::UInt,
     1,
     ValueType::UInt4,
     ValueType::UInt}};

bool isConversionName(std::string_view name)
{
    for (auto raw = 0; raw <= static_cast<int>(ValueType::Bool4); ++raw)
    {
        auto type = static_cast<ValueType>(raw);

        if (!isMatrix(type) && !isBoolean(type) && name == typeName(type))
            return true;
    }

    return false;
}

bool isThreadIdShape(int index, int components)
{
    if (index == allComponents)
        return components <= 3;

    return index >= 0 && index < 3;
}

std::uint32_t planRoundUp(int value, int multiple)
{
    return static_cast<std::uint32_t>((value + multiple - 1) / multiple * multiple);
}
} // namespace

class PlanBuilder
{
public:
    PlanBuilder(Plan& planToFill, const ShaderGraph& graphToRead)
        : plan(planToFill)
        , graph(graphToRead)
    {
    }

    void build()
    {
        if (!checkGraph())
            return;

        plan.nodes.resize(graph.nodeCount());
        marks.resize(graph.nodeCount(), 0);
        arrayUsed.resize(graph.arrays().size(), 0);
        blockReached.resize(graph.blockCount(), 0);

        buildBlock(ShaderGraph::rootBlock, 0, 0);

        if (failed())
            return;

        markReachable();

        if (failed())
            return;

        for (auto id = 0; id < graph.nodeCount() && !failed(); ++id)
            if (plan.nodes[id].used)
                decodeNode(id);

        if (failed())
            return;

        layOut();

        if (failed())
            return;

        buildSchedules();
    }

private:
    struct PendingSchedule
    {
        int roots[3] = {-1, -1, -1};
        int pinned = -1;
    };

    bool failed() const { return !plan.failure.empty(); }

    void fail(std::string reason)
    {
        if (!failed())
            plan.failure = std::move(reason);
    }

    bool checkGraph()
    {
        if (!graph.isCompute())
        {
            fail("not a compute graph: it records no store");
            return false;
        }

        if (graph.position() >= 0 || graph.fragment() >= 0)
        {
            fail("a render graph (position or fragment set) cannot run as a "
                 "kernel");
            return false;
        }

        const auto& slots = graph.storageBuffers();

        if (slots.size() > Plan::maxSlots)
        {
            fail("the kernel declares " + std::to_string(slots.size())
                 + " storage buffers; at most " + std::to_string(Plan::maxSlots)
                 + " are supported");
            return false;
        }

        plan.slotCount = slots.size();

        for (auto slot = 0; slot < slots.size(); ++slot)
        {
            plan.slotAccess[static_cast<std::size_t>(slot)] = slots[slot];
            plan.slotElement[static_cast<std::size_t>(slot)] =
                graph.storageElementType(slot);
        }

        plan.shape = graph.threadGroupShape();
        plan.dispatchRank = graph.dispatchRank();

        if (plan.shape.x <= 0 || plan.shape.y <= 0 || plan.shape.z <= 0)
        {
            fail("the thread group shape has no threads");
            return false;
        }

        auto lanes =
            static_cast<std::int64_t>(plan.shape.x) * plan.shape.y * plan.shape.z;

        if (lanes > planMaxLanes)
        {
            fail("the thread group shape has " + std::to_string(lanes)
                 + " threads; the CPU executor runs at most "
                 + std::to_string(planMaxLanes) + " per group");
            return false;
        }

        for (const auto& shared: graph.sharedArrays())
        {
            if (shared.elements < 0)
            {
                fail("a shared array has a negative size");
                return false;
            }
        }

        plan.laneCount = static_cast<int>(lanes);
        plan.stride =
            static_cast<int>(planRoundUp(plan.laneCount, planLaneAlignment));
        plan.boundsGuard = !graph.usesBarrier();
        plan.uniformTypes = graph.uniforms();
        return true;
    }

    struct BlockResult
    {
        int id = -1;
        bool jumpsOut = false;
    };

    bool enterBlock(int graphBlock)
    {
        if (graphBlock < 0 || graphBlock >= graph.blockCount())
        {
            fail("a statement names block " + std::to_string(graphBlock)
                 + ", which does not exist");
            return false;
        }

        auto& reached = blockReached[graphBlock];

        if (reached != 0)
        {
            fail("block " + std::to_string(graphBlock)
                 + " is reached twice: a body is shared or contains itself");
            return false;
        }

        reached = 1;
        return true;
    }

    bool checkStatement(int statement)
    {
        if (statement >= 0 && statement < graph.statementCount())
            return true;

        fail("a block names statement " + std::to_string(statement)
             + ", which does not exist");
        return false;
    }

    BlockResult buildBlock(int graphBlock, int loopDepth, int depth)
    {
        auto result = BlockResult {};

        if (!enterBlock(graphBlock))
            return result;

        result.id = plan.blocks.size();
        plan.blocks.add(Plan::BlockRange {});

        if (depth > plan.nesting)
            plan.nesting = depth;

        auto stepIds = Vector<int> {};
        auto pinned = -1;

        for (auto statementId: graph.block(graphBlock).statements)
        {
            if (failed() || !checkStatement(statementId))
                return result;

            const auto& statement = graph.statement(statementId);
            auto stepId = plan.steps.size();
            plan.steps.add(Plan::Step {});
            pending.add(PendingSchedule {});
            stepIds.add(stepId);

            auto step = Plan::Step {};
            step.kind = statement.kind;
            step.value = statement.value;
            step.index = statement.index;
            step.slot = statement.slot;

            auto schedule = PendingSchedule {};

            switch (statement.kind)
            {
                case StatementKind::Declare:
                case StatementKind::Assign:
                    if (!checkVariable(statement.slot) || !checkNode(statement.value)
                        || !checkVariableValue(statement))
                        return result;

                    schedule.roots[0] = statement.value;
                    pinned = -1;
                    break;

                case StatementKind::If:
                {
                    if (!checkCondition(statement.value))
                        return result;

                    auto body = buildBlock(statement.body, loopDepth, depth + 1);
                    step.body = body.id;
                    step.bodiesJumpOut = body.jumpsOut;

                    if (statement.elseBody >= 0)
                    {
                        auto elseBody =
                            buildBlock(statement.elseBody, loopDepth, depth + 1);
                        step.elseBody = elseBody.id;
                        step.bodiesJumpOut = step.bodiesJumpOut || elseBody.jumpsOut;
                    }

                    result.jumpsOut = result.jumpsOut || step.bodiesJumpOut;
                    schedule.roots[0] = statement.value;
                    pinned = -1;
                    break;
                }

                case StatementKind::Loop:
                {
                    if (!checkCondition(statement.value))
                        return result;

                    auto body = buildBlock(statement.body, loopDepth + 1, depth + 1);
                    step.body = body.id;
                    schedule.roots[0] = statement.value;
                    pinned = -1;
                    break;
                }

                case StatementKind::Break:
                case StatementKind::Continue:
                    if (loopDepth == 0)
                    {
                        fail(std::string("statement ")
                             + planStatementName(statement.kind)
                             + " outside a loop");
                        return result;
                    }

                    result.jumpsOut = true;
                    pinned = -1;
                    break;

                case StatementKind::Store:
                    if (!checkStore(statement, 1))
                        return result;

                    schedule.roots[0] = statement.index;
                    schedule.roots[1] = statement.value;

                    if (statement.record >= 0)
                    {
                        if (!checkNode(statement.record))
                            return result;

                        if (pinned == statement.record)
                        {
                            schedule.pinned = statement.record;
                        }
                        else
                        {
                            schedule.roots[2] = statement.record;
                            pinned = statement.record;
                        }

                        if (statement.recordComponentsLeft <= 0)
                            pinned = -1;
                    }
                    else
                    {
                        pinned = -1;
                    }

                    break;

                case StatementKind::VectorStore:
                {
                    if (!checkNode(statement.value))
                        return result;

                    auto width = componentCount(graph.expr(statement.value).type);

                    if (!checkStore(statement, width))
                        return result;

                    schedule.roots[0] = statement.index;
                    schedule.roots[1] = statement.value;
                    pinned = -1;
                    break;
                }

                case StatementKind::TextureStore:
                    fail("statement TextureStore: textures are not supported by "
                         "the CPU executor");
                    return result;

                case StatementKind::SharedStore:
                    if (!checkSharedStore(statement))
                        return result;

                    schedule.roots[0] = statement.index;
                    schedule.roots[1] = statement.value;
                    pinned = -1;
                    break;

                case StatementKind::Barrier:
                    pinned = -1;
                    break;

                case StatementKind::GroupReduce:
                    if (!checkReduction(statement))
                        return result;

                    step.reduction = statement.reduction;
                    step.scope = statement.scope;
                    step.type = graph.variables()[statement.slot];
                    schedule.roots[0] = statement.value;
                    pinned = -1;
                    break;

                case StatementKind::AtomicAdd:
                    if (!checkAtomicAdd(statement))
                        return result;

                    step.buffer = statement.bufferSlot;
                    schedule.roots[0] = statement.index;
                    schedule.roots[1] = statement.value;
                    pinned = -1;
                    break;

                case StatementKind::SimdMatrixFill:
                    if (!checkSimdMatrixFill(statement))
                        return result;

                    schedule.roots[0] = statement.value;
                    pinned = -1;
                    break;

                case StatementKind::SimdMatrixLoad:
                case StatementKind::SimdMatrixStore:
                    if (!checkSimdMatrixTransfer(statement))
                        return result;

                    step.buffer = statement.bufferSlot;
                    step.stride = statement.stride;
                    step.memory = statement.memory;
                    step.element = graph.simdMatrixElement(statement.slot);
                    schedule.roots[0] = statement.index;
                    schedule.roots[1] = statement.stride;
                    pinned = -1;
                    break;

                case StatementKind::SimdMatrixMultiplyAdd:
                    if (!checkSimdMatrixMultiplyAdd(statement))
                        return result;

                    step.left = statement.left;
                    step.right = statement.right;
                    pinned = -1;
                    break;
            }

            plan.steps[stepId] = step;
            pending[stepId] = schedule;
        }

        auto& range = plan.blocks[result.id];
        range.begin = plan.blockSteps.size();

        for (auto stepId: stepIds)
            plan.blockSteps.add(stepId);

        range.end = plan.blockSteps.size();
        return result;
    }

    bool checkNode(int node)
    {
        if (node >= 0 && node < graph.nodeCount())
            return true;

        fail("a statement names expression node " + std::to_string(node)
             + ", which does not exist");
        return false;
    }

    bool checkVariable(int slot)
    {
        if (slot >= 0 && slot < graph.variables().size())
            return true;

        fail("a statement names variable " + std::to_string(slot)
             + ", which does not exist");
        return false;
    }

    bool checkVariableValue(const Statement& statement)
    {
        auto variableType = graph.variables()[statement.slot];
        auto valueType = graph.expr(statement.value).type;

        if (valueType == variableType)
            return true;

        fail(std::string("statement ") + planStatementName(statement.kind)
             + " gives variable " + std::to_string(statement.slot) + " of type "
             + typeName(variableType) + " a value of type " + typeName(valueType));
        return false;
    }

    bool checkCondition(int node)
    {
        if (!checkNode(node))
            return false;

        if (graph.expr(node).type == ValueType::Bool)
            return true;

        fail("an If or Loop condition is not a scalar Bool");
        return false;
    }

    bool checkSlot(int slot)
    {
        if (slot >= 0 && slot < plan.slotCount)
        {
            plan.slotReferenced[static_cast<std::size_t>(slot)] = true;
            return true;
        }

        fail("storage slot " + std::to_string(slot) + " does not exist");
        return false;
    }

    bool checkStore(const Statement& statement, int width)
    {
        if (!checkSlot(statement.slot) || !checkNode(statement.index)
            || !checkNode(statement.value))
            return false;

        auto valueType = graph.expr(statement.value).type;

        if (componentCount(valueType) != width || width < 1 || width > 4
            || isMatrix(valueType) || isBoolean(valueType))
        {
            fail(std::string("a ") + planStatementName(statement.kind)
                 + " stores a value of type " + typeName(valueType));
            return false;
        }

        if (componentCount(graph.expr(statement.index).type) != 1)
        {
            fail(std::string("a ") + planStatementName(statement.kind)
                 + " has an index that is not a scalar");
            return false;
        }

        auto slot = static_cast<std::size_t>(statement.slot);

        if (plan.slotAccess[slot] == BufferAccess::Read)
        {
            fail(std::string("a ") + planStatementName(statement.kind)
                 + " writes storage slot " + std::to_string(statement.slot)
                 + ", which is read-only");
            return false;
        }

        auto element = plan.slotElement[slot];

        if (isFloatFamily(valueType) != isFloatFamily(element))
        {
            fail(std::string("a ") + planStatementName(statement.kind)
                 + " stores a value of type " + typeName(valueType)
                 + " into storage slot " + std::to_string(statement.slot) + " of "
                 + typeName(element));
            return false;
        }

        return true;
    }

    bool checkIndex(const Statement& statement)
    {
        auto type = graph.expr(statement.index).type;

        if (componentCount(type) == 1 && isIntegerFamily(type))
            return true;

        fail(std::string("a ") + planStatementName(statement.kind)
             + " has an index that is not a scalar integer");
        return false;
    }

    bool checkSharedSlot(int slot)
    {
        if (slot >= 0 && slot < graph.sharedArrays().size())
            return true;

        fail("shared array " + std::to_string(slot) + " does not exist");
        return false;
    }

    bool checkSharedStore(const Statement& statement)
    {
        if (!checkSharedSlot(statement.slot) || !checkNode(statement.index)
            || !checkNode(statement.value) || !checkIndex(statement))
            return false;

        auto elementType = graph.sharedArrays()[statement.slot].elementType;
        auto valueType = graph.expr(statement.value).type;

        if (valueType == elementType)
            return true;

        fail("a SharedStore gives shared array " + std::to_string(statement.slot)
             + " of " + typeName(elementType) + " a value of type "
             + typeName(valueType));
        return false;
    }

    static bool isReducible(ValueType type)
    {
        return type == ValueType::Float || type == ValueType::UInt
               || type == ValueType::Int;
    }

    bool checkReduction(const Statement& statement)
    {
        if (!checkVariable(statement.slot) || !checkNode(statement.value)
            || !checkVariableValue(statement))
            return false;

        auto type = graph.variables()[statement.slot];

        if (isReducible(type))
            return true;

        fail(std::string("a GroupReduce folds a value of type ") + typeName(type)
             + "; only Float, UInt and Int fold");
        return false;
    }

    bool checkAtomicAdd(const Statement& statement)
    {
        if (!checkVariable(statement.slot) || !checkSlot(statement.bufferSlot)
            || !checkNode(statement.index) || !checkNode(statement.value)
            || !checkIndex(statement))
            return false;

        if (plan.slotAccess[static_cast<std::size_t>(statement.bufferSlot)]
            != BufferAccess::Atomic)
        {
            fail("an AtomicAdd names storage slot "
                 + std::to_string(statement.bufferSlot) + ", which is not atomic");
            return false;
        }

        if (graph.variables()[statement.slot] != ValueType::UInt
            || graph.expr(statement.value).type != ValueType::UInt)
        {
            fail("an AtomicAdd adds or returns a value that is not a UInt");
            return false;
        }

        return true;
    }

    bool checkFragment(const Statement& statement, int fragment, const char* role)
    {
        if (fragment >= 0 && fragment < graph.simdMatrixCount())
            return true;

        fail(std::string("a ") + planStatementName(statement.kind) + " names " + role
             + " fragment " + std::to_string(fragment) + ", which does not exist");
        return false;
    }

    bool checkFloatFragment(const Statement& statement, int fragment)
    {
        if (!checkFragment(statement, fragment, "the"))
            return false;

        if (graph.simdMatrixElement(fragment) == SimdMatrixElement::Float)
            return true;

        fail(std::string("a ") + planStatementName(statement.kind)
             + " writes or stores packed fragment " + std::to_string(fragment)
             + "; only a float fragment is filled, stored or accumulated into");
        return false;
    }

    // A fragment belongs to a whole SIMD group: Metal requires full ones, and
    // the fallback's scratch holds threads / 32 SIMD groups' worth, so a
    // partial one indexes past it. The emitter only asserts this.
    bool checkWholeSimdGroups(const Statement& statement)
    {
        auto threads = graph.threadGroupShape().threadCount();

        if (threads % simdGroupWidth == 0)
            return true;

        fail(std::string("a ") + planStatementName(statement.kind)
             + " needs a threadgroup of whole SIMD groups, a multiple of "
             + std::to_string(simdGroupWidth) + " threads, and this one has "
             + std::to_string(threads));
        return false;
    }

    bool checkScalarInteger(const Statement& statement, int node, const char* role)
    {
        if (!checkNode(node))
            return false;

        auto type = graph.expr(node).type;

        if (componentCount(type) == 1 && isIntegerFamily(type))
            return true;

        fail(std::string("a ") + planStatementName(statement.kind) + " has " + role
             + " that is not a scalar integer");
        return false;
    }

    bool checkSimdMatrixFill(const Statement& statement)
    {
        if (!checkWholeSimdGroups(statement)
            || !checkFloatFragment(statement, statement.slot)
            || !checkNode(statement.value))
            return false;

        if (graph.expr(statement.value).type == ValueType::Float)
            return true;

        fail("a SimdMatrixFill fills a fragment with a value that is not a "
             "scalar Float");
        return false;
    }

    // A load takes any fragment - a Half or BFloat16 one is widened as it is
    // read, in SimdMatrix.cpp (Step::element says which) - and a store only a
    // float one.
    bool checkSimdMatrixElement(const Statement& statement)
    {
        if (statement.kind == StatementKind::SimdMatrixStore)
            return checkFloatFragment(statement, statement.slot);

        return checkFragment(statement, statement.slot, "the");
    }

    bool checkSimdMatrixMemory(const Statement& statement)
    {
        auto storing = statement.kind == StatementKind::SimdMatrixStore;

        if (statement.memory == SimdMatrixMemory::Shared)
        {
            if (!checkSharedSlot(statement.bufferSlot))
                return false;

            if (graph.sharedArrays()[statement.bufferSlot].elementType
                == ValueType::Float)
                return true;

            fail(std::string("a ") + planStatementName(statement.kind)
                 + " reaches shared array " + std::to_string(statement.bufferSlot)
                 + ", which does not hold Float");
            return false;
        }

        if (!checkSlot(statement.bufferSlot))
            return false;

        auto slot = static_cast<std::size_t>(statement.bufferSlot);

        if (plan.slotElement[slot] != ValueType::Float)
        {
            fail(std::string("a ") + planStatementName(statement.kind)
                 + " reaches storage slot " + std::to_string(statement.bufferSlot)
                 + ", which does not hold Float");
            return false;
        }

        if (storing && plan.slotAccess[slot] == BufferAccess::Read)
        {
            fail("a SimdMatrixStore writes storage slot "
                 + std::to_string(statement.bufferSlot) + ", which is read-only");
            return false;
        }

        return true;
    }

    bool checkSimdMatrixTransfer(const Statement& statement)
    {
        return checkWholeSimdGroups(statement) && checkSimdMatrixElement(statement)
               && checkSimdMatrixMemory(statement)
               && checkScalarInteger(statement, statement.index, "an offset")
               && checkScalarInteger(statement, statement.stride, "a row stride");
    }

    bool checkSimdMatrixMultiplyAdd(const Statement& statement)
    {
        return checkWholeSimdGroups(statement)
               && checkFloatFragment(statement, statement.slot)
               && checkFragment(statement, statement.left, "a left")
               && checkFragment(statement, statement.right, "a right");
    }

    void markReachable()
    {
        auto stack = Vector<int> {};

        for (const auto& schedule: pending)
            for (auto root: schedule.roots)
                if (root >= 0)
                    stack.add(root);

        while (!stack.empty() && !failed())
        {
            auto id = stack.back();
            stack.pop_back();

            if (plan.nodes[id].used)
                continue;

            plan.nodes[id].used = true;
            const auto& expr = graph.expr(id);

            for (auto argument: expr.args)
            {
                if (!checkNode(argument))
                    return;

                stack.add(argument);
            }

            if (expr.kind == ExprKind::ArrayRead)
            {
                if (expr.index < 0 || expr.index >= graph.arrays().size())
                {
                    fail("an ArrayRead names array " + std::to_string(expr.index)
                         + ", which does not exist");
                    return;
                }

                if (arrayUsed[expr.index] == 0)
                {
                    arrayUsed[expr.index] = 1;

                    for (auto element: graph.arrays()[expr.index].elements)
                    {
                        if (!checkNode(element))
                            return;

                        stack.add(element);
                    }
                }
            }
        }
    }

    ValueType argumentType(const Expr& expr, int which) const
    {
        return graph.expr(expr.args[which]).type;
    }

    int argumentComponents(const Expr& expr, int which) const
    {
        return componentCount(argumentType(expr, which));
    }

    void rejectNode(int id, const std::string& why)
    {
        const auto& expr = graph.expr(id);
        auto name = std::string(planKindName(expr.kind));

        if (expr.kind == ExprKind::Call)
            name += " \"" + expr.text + "\"";

        fail("expression " + name + " (node " + std::to_string(id) + "): " + why);
    }

    bool requireArguments(int id, int count)
    {
        if (graph.expr(id).args.size() == count)
            return true;

        rejectNode(id, "expected " + std::to_string(count) + " arguments");
        return false;
    }

    bool requireBroadcastable(int id)
    {
        const auto& expr = graph.expr(id);
        auto width = componentCount(expr.type);

        for (auto which = 0; which < expr.args.size(); ++which)
        {
            auto components = argumentComponents(expr, which);

            if (components != width && components != 1)
            {
                rejectNode(id, "an argument's width does not match the result");
                return false;
            }
        }

        return true;
    }

    void setArguments(Plan::Node& node, const Expr& expr)
    {
        node.argBegin = plan.arguments.size();
        node.argCount = expr.args.size();

        for (auto argument: expr.args)
            plan.arguments.add(argument);
    }

    void decodeNode(int id)
    {
        const auto& expr = graph.expr(id);
        auto& node = plan.nodes[id];
        node.components = static_cast<std::uint8_t>(componentCount(expr.type));
        setArguments(node, expr);

        switch (expr.kind)
        {
            case ExprKind::Input:
            case ExprKind::Varying:
                rejectNode(id, "a render graph input cannot run as a kernel");
                return;

            case ExprKind::Sample:
            case ExprKind::Fetch:
                rejectNode(id, "textures are not supported by the CPU executor");
                return;

            case ExprKind::AtomicLoad:
                decodeAtomicLoad(id, node, expr);
                return;

            case ExprKind::LocalId:
                if (!isThreadIdShape(expr.index, node.components))
                    rejectNode(id, "names no axis of the local id");

                return;

            case ExprKind::GroupId:
                if (!isThreadIdShape(expr.index, node.components))
                {
                    rejectNode(id, "names no axis of the group id");
                    return;
                }

                plan.groupIdLeaves.add({id, expr.index});
                return;

            case ExprKind::SharedRead:
                decodeSharedRead(id, node, expr);
                return;

            case ExprKind::SimdGroupIndex:
                plan.simdGroupLeaves.add(id);
                return;

            case ExprKind::Uniform:
                if (expr.index < 0 || expr.index >= plan.uniformTypes.size())
                {
                    rejectNode(id, "names a uniform slot that does not exist");
                    return;
                }

                plan.uniformLeaves.add({id, expr.index});
                return;

            case ExprKind::Constant:
                plan.constantNodes.add({id, constantWord(expr)});
                return;

            case ExprKind::GridExtent:
                plan.extentLeaves.add({id, expr.index});
                return;

            case ExprKind::ThreadId:
                if (!isThreadIdShape(expr.index, node.components))
                {
                    rejectNode(id, "names no axis of the thread id");
                    return;
                }

                plan.threadIdLeaves.add({id, expr.index});
                return;

            case ExprKind::VarRead:
                if (expr.index < 0 || expr.index >= graph.variables().size())
                    rejectNode(id, "names a variable that does not exist");

                return;

            case ExprKind::Construct:
                decodeConstruct(id, node, expr);
                return;

            case ExprKind::Swizzle:
                decodeSwizzle(id, node, expr);
                return;

            case ExprKind::Call:
                decodeCall(id, node, expr);
                return;

            case ExprKind::Unary:
                decodeUnary(id, node, expr);
                return;

            case ExprKind::Binary:
                decodeBinary(id, node, expr);
                return;

            case ExprKind::Compare:
                decodeCompare(id, node, expr);
                return;

            case ExprKind::Select:
                if (!requireArguments(id, 3))
                    return;

                if (argumentType(expr, 0) != ValueType::Bool)
                {
                    rejectNode(id, "the condition is not a scalar Bool");
                    return;
                }

                if (requireBroadcastable(id))
                    node.op = Op::Select;

                return;

            case ExprKind::Mul:
                decodeMul(id, node, expr);
                return;

            case ExprKind::BufferRead:
            case ExprKind::BufferVectorRead:
                if (!requireArguments(id, 1) || !checkSlot(expr.index))
                    return;

                if (isMatrix(expr.type) || isBoolean(expr.type))
                {
                    rejectNode(id, "reads a type no buffer holds");
                    return;
                }

                node.op = expr.kind == ExprKind::BufferRead ? Op::BufferRead
                                                            : Op::BufferVectorRead;
                node.immediate = expr.index;
                return;

            case ExprKind::ArrayRead:
            {
                if (!requireArguments(id, 1))
                    return;

                const auto& array = graph.arrays()[expr.index];

                if (array.elementType != expr.type)
                {
                    rejectNode(id, "the element type does not match the array");
                    return;
                }

                node.op = Op::ArrayRead;
                node.immediate = expr.index;
                return;
            }
        }
    }

    bool requireScalarIndex(int id, const Expr& expr)
    {
        auto type = argumentType(expr, 0);

        if (componentCount(type) == 1 && isIntegerFamily(type))
            return true;

        rejectNode(id, "the index is not a scalar integer");
        return false;
    }

    void decodeAtomicLoad(int id, Plan::Node& node, const Expr& expr)
    {
        if (!requireArguments(id, 1) || !checkSlot(expr.index)
            || !requireScalarIndex(id, expr))
            return;

        if (plan.slotAccess[static_cast<std::size_t>(expr.index)]
                != BufferAccess::Atomic
            || expr.type != ValueType::UInt)
        {
            rejectNode(id, "reads a slot that is not atomic");
            return;
        }

        node.op = Op::AtomicLoad;
        node.immediate = expr.index;
    }

    void decodeSharedRead(int id, Plan::Node& node, const Expr& expr)
    {
        if (!requireArguments(id, 1) || !requireScalarIndex(id, expr))
            return;

        if (expr.index < 0 || expr.index >= graph.sharedArrays().size())
        {
            rejectNode(id, "names a shared array that does not exist");
            return;
        }

        if (graph.sharedArrays()[expr.index].elementType != expr.type)
        {
            rejectNode(id, "the element type does not match the shared array");
            return;
        }

        node.op = Op::SharedRead;
        node.immediate = expr.index;
    }

    static Word constantWord(const Expr& expr)
    {
        switch (expr.type)
        {
            case ValueType::Float:
                return Lanes::toWord(expr.value);
            case ValueType::Bool:
                return Lanes::maskOf(expr.index != 0);
            default:
                return static_cast<Word>(expr.index);
        }
    }

    void decodeConstruct(int id, Plan::Node& node, const Expr& expr)
    {
        auto total = 0;

        for (auto which = 0; which < expr.args.size(); ++which)
            total += argumentComponents(expr, which);

        if (total != node.components || expr.args.empty())
        {
            rejectNode(id,
                       "its arguments hold " + std::to_string(total)
                           + " components where the result has "
                           + std::to_string(node.components));
            return;
        }

        node.op = Op::Construct;
    }

    void decodeSwizzle(int id, Plan::Node& node, const Expr& expr)
    {
        if (!requireArguments(id, 1))
            return;

        auto width = argumentComponents(expr, 0);
        const auto& letters = expr.text;

        if (letters.empty() || letters.size() > 4
            || static_cast<int>(letters.size()) != node.components
            || isMatrix(argumentType(expr, 0)))
        {
            rejectNode(id, "malformed component list \"" + letters + "\"");
            return;
        }

        for (auto at = std::size_t {0}; at < letters.size(); ++at)
        {
            auto component = std::string_view("xyzw").find(letters[at]);

            if (component == std::string_view::npos
                || static_cast<int>(component) >= width)
            {
                rejectNode(id, "malformed component list \"" + letters + "\"");
                return;
            }

            node.swizzle[at] = static_cast<std::uint8_t>(component);
        }

        node.op = Op::Swizzle;
    }

    void decodeUnary(int id, Plan::Node& node, const Expr& expr)
    {
        if (!requireArguments(id, 1))
            return;

        auto type = expr.type;

        if (expr.op == '-' && isFloatFamily(type))
            node.op = Op::NegF;
        else if (expr.op == '-' && isSignedInteger(type))
            node.op = Op::NegI;
        else if ((expr.op == '!' && isBoolean(type))
                 || (expr.op == '~' && isIntegerFamily(type)))
            node.op = Op::BitNot;
        else
            rejectNode(id,
                       std::string("unsupported operator '") + expr.op + "' on "
                           + typeName(type));
    }

    void decodeBinary(int id, Plan::Node& node, const Expr& expr)
    {
        if (!requireArguments(id, 2) || !requireBroadcastable(id))
            return;

        auto type = expr.type;
        auto isSigned = isSignedInteger(type);

        if (!expr.text.empty())
        {
            if (!isIntegerFamily(type) || (expr.text != "<<" && expr.text != ">>"))
            {
                rejectNode(id,
                           "unsupported operator \"" + expr.text + "\" on "
                               + typeName(type));
                return;
            }

            node.op = expr.text == "<<" ? Op::Shl : isSigned ? Op::ShrS : Op::ShrU;
            return;
        }

        if (isFloatFamily(type))
        {
            switch (expr.op)
            {
                case '+':
                    node.op = Op::AddF;
                    return;
                case '-':
                    node.op = Op::SubF;
                    return;
                case '*':
                    node.op = Op::MulF;
                    return;
                case '/':
                    node.op = Op::DivF;
                    return;
                default:
                    break;
            }
        }
        else if (isIntegerFamily(type))
        {
            switch (expr.op)
            {
                case '+':
                    node.op = Op::AddI;
                    return;
                case '-':
                    node.op = Op::SubI;
                    return;
                case '*':
                    node.op = Op::MulI;
                    return;
                case '/':
                    node.op = isSigned ? Op::DivS : Op::DivU;
                    return;
                case '%':
                    node.op = isSigned ? Op::RemS : Op::RemU;
                    return;
                case '&':
                    node.op = Op::And;
                    return;
                case '|':
                    node.op = Op::Or;
                    return;
                case '^':
                    node.op = Op::Xor;
                    return;
                default:
                    break;
            }
        }

        rejectNode(id,
                   std::string("unsupported operator '") + expr.op + "' on "
                       + typeName(type));
    }

    void decodeCompare(int id, Plan::Node& node, const Expr& expr)
    {
        if (!requireArguments(id, 2) || !requireBroadcastable(id))
            return;

        auto operandType = argumentType(expr, 0);
        const auto& text = expr.text;

        if (!isBoolean(expr.type))
        {
            rejectNode(id, "the result is not a Bool");
            return;
        }

        if (isBoolean(operandType))
        {
            if (text == "==")
                node.op = Op::EqMask;
            else if (text == "!=")
                node.op = Op::Xor;
            else if (text == "&&")
                node.op = Op::And;
            else if (text == "||")
                node.op = Op::Or;
            else
                rejectNode(id, "unsupported relation \"" + text + "\" on a Bool");

            return;
        }

        auto relation = relationFor(text);

        if (relation < 0 || isMatrix(operandType))
        {
            rejectNode(id,
                       "unsupported relation \"" + text + "\" on "
                           + typeName(operandType));
            return;
        }

        node.sub = static_cast<std::uint8_t>(relation);

        if (isFloatFamily(operandType))
            node.op = Op::CmpF;
        else if (isSignedInteger(operandType))
            node.op = Op::CmpS;
        else
            node.op = Op::CmpU;
    }

    static int relationFor(const std::string& text)
    {
        if (text == "<")
            return static_cast<int>(Relation::Less);
        if (text == "<=")
            return static_cast<int>(Relation::LessEqual);
        if (text == ">")
            return static_cast<int>(Relation::Greater);
        if (text == ">=")
            return static_cast<int>(Relation::GreaterEqual);
        if (text == "==")
            return static_cast<int>(Relation::Equal);
        if (text == "!=")
            return static_cast<int>(Relation::NotEqual);

        return -1;
    }

    void decodeMul(int id, Plan::Node& node, const Expr& expr)
    {
        if (!requireArguments(id, 2))
            return;

        auto left = argumentType(expr, 0);
        auto right = argumentType(expr, 1);
        auto leftOrder = matrixOrder(left);
        auto rightOrder = matrixOrder(right);

        if (leftOrder > 0 && rightOrder == 0 && componentCount(right) == leftOrder
            && isFloatFamily(right))
        {
            node.op = Op::MatVec;
            node.order = static_cast<std::uint8_t>(leftOrder);
        }
        else if (leftOrder == 0 && rightOrder > 0
                 && componentCount(left) == rightOrder && isFloatFamily(left))
        {
            node.op = Op::VecMat;
            node.order = static_cast<std::uint8_t>(rightOrder);
        }
        else if (leftOrder > 0 && leftOrder == rightOrder)
        {
            node.op = Op::MatMat;
            node.order = static_cast<std::uint8_t>(leftOrder);
        }
        else
        {
            rejectNode(id,
                       std::string("unsupported product of ") + typeName(left)
                           + " and " + typeName(right));
        }
    }

    void decodeCall(int id, Plan::Node& node, const Expr& expr)
    {
        const auto& name = expr.text;
        auto type = expr.type;

        if (name == "dfdx" || name == "dfdy" || name == "fwidth")
        {
            rejectNode(id, "derivatives exist only in a fragment shader");
            return;
        }

        if (name.starts_with("eacp"))
        {
            decodeHelper(id, node, expr);
            return;
        }

        if (expr.args.empty())
        {
            rejectNode(id, "a call with no arguments");
            return;
        }

        auto operand = argumentType(expr, 0);

        for (const auto& math: planMathNames)
        {
            if (name != math.name)
                continue;

            if (!requireArguments(id, 1) || !isFloatFamily(type) || isMatrix(type)
                || operand != type)
            {
                rejectNode(id, "defined on the float vectors only");
                return;
            }

            node.op = Op::UnaryMath;
            node.sub = static_cast<std::uint8_t>(math.function);
            return;
        }

        if (name == "abs")
        {
            if (!requireArguments(id, 1) || operand != type || isMatrix(type))
                rejectNode(id, "the argument and the result differ");
            else if (isFloatFamily(type))
            {
                node.op = Op::UnaryMath;
                node.sub = static_cast<std::uint8_t>(MathFunction::Abs);
            }
            else if (isSignedInteger(type))
                node.op = Op::AbsS;
            else
                rejectNode(id, std::string("unsupported on ") + typeName(type));

            return;
        }

        if (name == "min" || name == "max")
        {
            if (!requireArguments(id, 2) || !requireBroadcastable(id))
                return;

            auto isMin = name == "min";

            if (isFloatFamily(type) && !isMatrix(type))
                node.op = isMin ? Op::MinF : Op::MaxF;
            else if (isSignedInteger(type))
                node.op = isMin ? Op::MinS : Op::MaxS;
            else if (isUnsignedInteger(type))
                node.op = isMin ? Op::MinU : Op::MaxU;
            else
                rejectNode(id, std::string("unsupported on ") + typeName(type));

            return;
        }

        for (const auto& broadcast: planBroadcastNames)
        {
            if (name != broadcast.name)
                continue;

            if (!requireArguments(id, broadcast.arguments)
                || !requireBroadcastable(id))
                return;

            if (!isFloatFamily(type) || isMatrix(type))
            {
                rejectNode(id, "defined on the float vectors only");
                return;
            }

            node.op = broadcast.op;
            return;
        }

        for (const auto& geometric: planGeometricNames)
        {
            if (name != geometric.name)
                continue;

            if (!requireArguments(id, geometric.arguments))
                return;

            decodeGeometric(id, node, expr, geometric.op);
            return;
        }

        if (name == "transpose" || name == "determinant")
        {
            if (!requireArguments(id, 1) || !isMatrix(operand))
            {
                rejectNode(id, "takes a matrix");
                return;
            }

            node.op = name == "transpose" ? Op::Transpose : Op::Determinant;
            node.order = static_cast<std::uint8_t>(matrixOrder(operand));
            return;
        }

        if (name == "all" || name == "any")
        {
            if (!requireArguments(id, 1) || !isBoolean(operand)
                || type != ValueType::Bool)
            {
                rejectNode(id, "takes a Bool vector");
                return;
            }

            node.op = name == "all" ? Op::All : Op::Any;
            node.order = static_cast<std::uint8_t>(componentCount(operand));
            return;
        }

        if (name.starts_with("as_type<"))
        {
            if (!requireArguments(id, 1)
                || componentCount(operand) != node.components || isBoolean(operand)
                || isBoolean(type))
            {
                rejectNode(id, "the bitcast changes the width");
                return;
            }

            node.op = Op::CopyBits;
            return;
        }

        if (isConversionName(name))
        {
            decodeConversion(id, node, expr, operand);
            return;
        }

        rejectNode(id, "unknown builtin");
    }

    void decodeHelper(int id, Plan::Node& node, const Expr& expr)
    {
        const auto& name = expr.text;
        auto type = expr.type;

        for (const auto& helper: planComponentHelperNames)
        {
            if (name != helper.name)
                continue;

            if (!requireArguments(id, 1) || !isFloatFamily(type) || isMatrix(type)
                || argumentType(expr, 0) != type)
            {
                rejectNode(id, "defined on the float vectors only");
                return;
            }

            node.op = Op::Helper;
            node.sub = static_cast<std::uint8_t>(helper.function);
            return;
        }

        for (const auto& helper: planPackedHelperNames)
        {
            if (name != helper.name)
                continue;

            if (!requireArguments(id, helper.arguments))
                return;

            auto matches =
                type == helper.result && argumentType(expr, 0) == helper.first
                && (helper.arguments == 1 || argumentType(expr, 1) == helper.second);

            if (!matches)
            {
                rejectNode(id,
                           "its argument or result types differ from the helper's");
                return;
            }

            node.op = Op::Helper;
            node.sub = static_cast<std::uint8_t>(helper.function);
            return;
        }

        rejectNode(id, "unknown helper");
    }

    void decodeGeometric(int id, Plan::Node& node, const Expr& expr, Op op)
    {
        auto operand = argumentType(expr, 0);
        auto width = componentCount(operand);

        if (!isFloatFamily(operand) || isMatrix(operand) || width < 2)
        {
            rejectNode(id, "takes float vectors");
            return;
        }

        for (auto which = 1; which < expr.args.size(); ++which)
        {
            auto isEta = op == Op::Refract && which == 2;
            auto expected = isEta ? 1 : width;

            if (argumentComponents(expr, which) != expected
                || !isFloatFamily(argumentType(expr, which)))
            {
                rejectNode(id, "the arguments' widths differ");
                return;
            }
        }

        if (op == Op::Cross && width != 3)
        {
            rejectNode(id, "takes Float3s");
            return;
        }

        node.op = op;
        node.order = static_cast<std::uint8_t>(width);
    }

    void decodeConversion(int id,
                          Plan::Node& node,
                          const Expr& expr,
                          ValueType source)
    {
        auto target = expr.type;

        if (!requireArguments(id, 1) || expr.text != typeName(target)
            || componentCount(source) != node.components || isMatrix(source))
        {
            rejectNode(id, "the conversion changes the width");
            return;
        }

        if (isFloatFamily(target))
        {
            if (isUnsignedInteger(source))
                node.op = Op::FloatFromU;
            else if (isSignedInteger(source))
                node.op = Op::FloatFromS;
            else if (isBoolean(source))
                node.op = Op::FloatFromMask;
            else
                node.op = Op::CopyBits;

            return;
        }

        if (isFloatFamily(source))
            node.op = isSignedInteger(target) ? Op::IntFromF : Op::UIntFromF;
        else if (isBoolean(source))
            node.op = Op::IntFromMask;
        else
            node.op = Op::CopyBits;
    }

    std::uint32_t allocate(int components)
    {
        auto offset = cursor;
        cursor += static_cast<std::size_t>(components)
                  * static_cast<std::size_t>(plan.stride);
        return static_cast<std::uint32_t>(offset);
    }

    static bool ownsScratch(ExprKind kind)
    {
        return kind != ExprKind::VarRead && kind != ExprKind::LocalId;
    }

    void layOutShared()
    {
        plan.sharedOffset = static_cast<std::uint32_t>(cursor);

        for (const auto& shared: graph.sharedArrays())
        {
            auto layout = Plan::SharedLayout {};
            layout.elements = shared.elements;
            layout.components = componentCount(shared.elementType);
            layout.storage = static_cast<std::uint32_t>(cursor);
            cursor += static_cast<std::size_t>(layout.elements)
                      * static_cast<std::size_t>(layout.components);
            plan.sharedLayouts.add(layout);
        }

        auto words = cursor - plan.sharedOffset;
        plan.sharedCount = words <= planMaxWords ? static_cast<int>(words) : 0;
    }

    void layOutFragments()
    {
        plan.fragmentOffset = static_cast<std::uint32_t>(cursor);

        auto words = static_cast<std::size_t>(graph.simdMatrixCount())
                     * static_cast<std::size_t>(plan.simdGroupCount())
                     * static_cast<std::size_t>(Plan::fragmentElements);

        cursor += words;
        plan.fragmentCount = words <= planMaxWords ? static_cast<int>(words) : 0;
    }

    void layOut()
    {
        for (auto id = 0; id < graph.nodeCount(); ++id)
        {
            auto& node = plan.nodes[id];

            if (node.used && ownsScratch(graph.expr(id).kind))
                node.scratch = allocate(node.components);
        }

        for (auto type: graph.variables())
        {
            auto variable = Plan::Variable {};
            variable.components = componentCount(type);
            variable.storage = allocate(variable.components);
            plan.variableLayouts.add(variable);
        }

        for (auto id = 0; id < graph.nodeCount(); ++id)
        {
            const auto& expr = graph.expr(id);

            if (plan.nodes[id].used && expr.kind == ExprKind::VarRead)
                plan.nodes[id].scratch = plan.variableLayouts[expr.index].storage;
        }

        for (auto slot = 0; slot < graph.arrays().size(); ++slot)
        {
            const auto& array = graph.arrays()[slot];
            auto layout = Plan::ArrayLayout {};
            layout.components = componentCount(array.elementType);
            layout.elementBegin = plan.arrayElements.size();
            layout.elementCount = array.elements.size();

            for (auto element: array.elements)
                plan.arrayElements.add(element);

            layout.used = arrayUsed[slot] != 0;

            if (layout.used)
                layout.storage = allocate(layout.components * layout.elementCount);

            plan.arrayLayouts.add(layout);
        }

        layOutShared();
        layOutFragments();

        if (graph.usesGroupReduction())
            plan.reductionOffset = allocate(1);

        plan.maskOffset = allocate(plan.maskFrameCount());
        plan.localOffset = allocate(3);
        plan.realLaneOffset = allocate(1);

        for (auto id = 0; id < graph.nodeCount(); ++id)
        {
            const auto& expr = graph.expr(id);

            if (plan.nodes[id].used && expr.kind == ExprKind::LocalId)
                plan.nodes[id].scratch = plan.localCoordinates(
                    expr.index == allComponents ? 0 : expr.index);
        }

        plan.uniformOffset = static_cast<std::uint32_t>(cursor);
        cursor += static_cast<std::size_t>(Plan::uniformWordsPerSlot)
                  * static_cast<std::size_t>(plan.uniformTypes.size());
        plan.wordCount = cursor;

        if (cursor > planMaxWords)
            fail("the kernel needs " + std::to_string(cursor)
                 + " words of scratch; the CPU executor addresses at most "
                 + std::to_string(planMaxWords));
    }

    bool isScheduleLeaf(int id, int pinned) const
    {
        return id == pinned || plan.nodes[id].op == Op::Leaf;
    }

    void visit(int root, int pinned)
    {
        if (root < 0 || marks[root] == stamp || isScheduleLeaf(root, pinned))
            return;

        marks[root] = stamp;
        walk.clear();
        walk.add({root, 0});

        while (!walk.empty())
        {
            auto& top = walk.back();
            const auto& node = plan.nodes[top.node];

            if (top.next < node.argCount)
            {
                auto child = plan.argument(node, top.next++);

                if (marks[child] != stamp && !isScheduleLeaf(child, pinned))
                {
                    marks[child] = stamp;
                    walk.add({child, 0});
                }

                continue;
            }

            plan.scheduleList.add(top.node);
            walk.pop_back();
        }
    }

    void buildSchedules()
    {
        for (auto stepId = 0; stepId < plan.steps.size(); ++stepId)
        {
            const auto& roots = pending[stepId];
            ++stamp;

            auto& step = plan.steps[stepId];
            step.scheduleBegin = plan.scheduleList.size();

            for (auto root: roots.roots)
                visit(root, roots.pinned);

            step.scheduleEnd = plan.scheduleList.size();
        }

        ++stamp;

        for (auto slot = 0; slot < plan.arrayLayouts.size(); ++slot)
        {
            auto& layout = plan.arrayLayouts[slot];
            layout.schedule.begin = plan.scheduleList.size();

            if (arrayUsed[slot] != 0)
                for (auto element: graph.arrays()[slot].elements)
                    visit(element, -1);

            layout.schedule.end = plan.scheduleList.size();
        }
    }

    struct WalkEntry
    {
        int node = -1;
        int next = 0;
    };

    Plan& plan;
    const ShaderGraph& graph;
    Vector<PendingSchedule> pending;
    Vector<int> marks;
    Vector<char> arrayUsed;
    Vector<char> blockReached;
    Vector<WalkEntry> walk;
    int stamp = 0;
    std::size_t cursor = 0;
};

Plan::Plan(const ShaderGraph& graph)
{
    auto builder = PlanBuilder {*this, graph};
    builder.build();

    if (!isValid())
        LOG("eacp: the CPU executor cannot run this kernel: ", failure);
}

BufferAccess Plan::access(int slot) const
{
    return slot >= 0 && slot < slotCount ? slotAccess[static_cast<std::size_t>(slot)]
                                         : BufferAccess::Read;
}

ValueType Plan::element(int slot) const
{
    return slot >= 0 && slot < slotCount
               ? slotElement[static_cast<std::size_t>(slot)]
               : ValueType::Float;
}

bool Plan::referencesSlot(int slot) const
{
    return slot >= 0 && slot < slotCount
           && slotReferenced[static_cast<std::size_t>(slot)];
}

ValueType Plan::uniformType(int slot) const
{
    return slot >= 0 && slot < uniformTypes.size() ? uniformTypes[slot]
                                                   : ValueType::Float;
}

std::size_t Plan::footprintBytes() const
{
    return wordCount * sizeof(Word) + 64;
}
} // namespace eacp::GPU::CpuCompute
