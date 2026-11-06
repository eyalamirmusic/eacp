#include "PlanBuilder.h"

#include <Miro/Reflection/ReflectEnum.h>

#include <algorithm>

namespace eacp::GPU::CpuCompute
{
namespace
{
bool hasEmptyAxis(const ThreadGroupShape& shape)
{
    return std::min({shape.x, shape.y, shape.z}) <= 0;
}

bool isStorable(ValueType type, int width)
{
    return componentCount(type) == width && width >= 1 && width <= 4
           && !isMatrix(type) && !isBoolean(type);
}

bool isReducible(ValueType type)
{
    return type == ValueType::Float || type == ValueType::UInt
           || type == ValueType::Int;
}
} // namespace

bool PlanBuilder::checkGraph()
{
    if (!graph.isCompute())
    {
        fail("not a compute graph: it records no store");
        return false;
    }

    if (graph.position() >= 0 || graph.fragment() >= 0)
    {
        fail("a render graph (position or fragment set) cannot run as a kernel");
        return false;
    }

    const auto& slots = graph.storageBuffers();

    if (slots.size() > Plan::maxSlots)
    {
        fail("the kernel declares {} storage buffers; at most {} are supported",
             slots.size(),
             Plan::maxSlots);
        return false;
    }

    plan.slotCount = slots.size();

    for (auto slot = 0; slot < slots.size(); ++slot)
    {
        plan.slotAccess[static_cast<std::size_t>(slot)] = slots[slot];
        plan.slotElement[static_cast<std::size_t>(slot)] =
            graph.storageElementType(slot);
    }

    return checkThreadGroupShape() && packUniforms();
}

bool PlanBuilder::checkThreadGroupShape()
{
    plan.shape = graph.threadGroupShape();
    plan.dispatchRank = graph.dispatchRank();

    if (hasEmptyAxis(plan.shape))
    {
        fail("the thread group shape has no threads");
        return false;
    }

    auto lanes =
        static_cast<std::int64_t>(plan.shape.x) * plan.shape.y * plan.shape.z;

    if (lanes > maxLanes)
    {
        fail(
            "the thread group shape has {} threads; the CPU executor runs at most {} per group",
            lanes,
            maxLanes);
        return false;
    }

    auto hasNegativeSize = [](const auto& shared) { return shared.elements < 0; };

    if (std::ranges::any_of(graph.sharedArrays(), hasNegativeSize))
    {
        fail("a shared array has a negative size");
        return false;
    }

    plan.laneCount = static_cast<int>(lanes);
    plan.boundsGuard = !graph.usesBarrier();
    return true;
}

bool PlanBuilder::packUniforms()
{
    plan.uniformTypes = graph.uniforms();

    for (auto type: plan.uniformTypes)
    {
        plan.uniformOffsets.add(plan.uniformWordCount);
        plan.uniformWordCount += byteSize(type) / static_cast<int>(sizeof(Word));
    }

    if (plan.uniformWordCount <= Plan::maxUniformWords)
        return true;

    fail("the kernel's uniforms take {} bytes; at most {} are supported",
         plan.uniformWordCount * sizeof(Word),
         Plan::maxUniformWords * sizeof(Word));
    return false;
}

bool PlanBuilder::enterBlock(BlockId graphBlock)
{
    if (graphBlock < 0 || graphBlock >= graph.blockCount())
    {
        fail("a statement names block {}, which does not exist", graphBlock);
        return false;
    }

    auto& reached = blockReached[graphBlock];

    if (reached != 0)
    {
        fail("block {} is reached twice: a body is shared or contains itself",
             graphBlock);
        return false;
    }

    reached = 1;
    return true;
}

bool PlanBuilder::checkStatement(int statement)
{
    if (statement >= 0 && statement < graph.statementCount())
        return true;

    fail("a block names statement {}, which does not exist", statement);
    return false;
}

PlanBuilder::BlockResult
    PlanBuilder::buildBlock(BlockId graphBlock, int loopDepth, int depth)
{
    if (!enterBlock(graphBlock))
        return {};

    auto walk = BlockWalk {loopDepth, depth};
    auto id = plan.blocks.size();
    plan.blocks.add(Plan::BlockRange {});
    plan.nesting = std::max(plan.nesting, depth);

    auto stepIds = Vector<int> {};

    for (auto statementId: graph.block(graphBlock).statements)
    {
        if (failed() || !checkStatement(statementId))
            return {id, walk.jumpsOut};

        auto stepId = plan.steps.size();
        plan.steps.add(Plan::Step {});
        pending.add(PendingSchedule {});
        stepIds.add(stepId);

        if (!buildStep(graph.statement(statementId), stepId, walk))
            return {id, walk.jumpsOut};
    }

    auto& range = plan.blocks[id];
    range.begin = plan.blockSteps.size();

    for (auto stepId: stepIds)
        plan.blockSteps.add(stepId);

    range.end = plan.blockSteps.size();
    return {id, walk.jumpsOut};
}

bool PlanBuilder::buildStep(const Statement& statement,
                            StepId stepId,
                            BlockWalk& walk)
{
    using Entry = std::pair<StatementKind, StepBuilder>;

    static constexpr auto builders = Array<Entry, 17> {
        Entry {StatementKind::Declare, &PlanBuilder::buildVariableStep},
        Entry {StatementKind::Assign, &PlanBuilder::buildVariableStep},
        Entry {StatementKind::If, &PlanBuilder::buildIfStep},
        Entry {StatementKind::Loop, &PlanBuilder::buildLoopStep},
        Entry {StatementKind::Break, &PlanBuilder::buildJumpStep},
        Entry {StatementKind::Continue, &PlanBuilder::buildJumpStep},
        Entry {StatementKind::Store, &PlanBuilder::buildStoreStep},
        Entry {StatementKind::VectorStore, &PlanBuilder::buildVectorStoreStep},
        Entry {StatementKind::TextureStore, &PlanBuilder::buildTextureStoreStep},
        Entry {StatementKind::SharedStore, &PlanBuilder::buildSharedStoreStep},
        Entry {StatementKind::Barrier, &PlanBuilder::buildBarrierStep},
        Entry {StatementKind::GroupReduce, &PlanBuilder::buildReductionStep},
        Entry {StatementKind::AtomicAdd, &PlanBuilder::buildAtomicAddStep},
        Entry {StatementKind::SimdMatrixFill, &PlanBuilder::buildSimdMatrixFillStep},
        Entry {StatementKind::SimdMatrixLoad,
               &PlanBuilder::buildSimdMatrixTransferStep},
        Entry {StatementKind::SimdMatrixStore,
               &PlanBuilder::buildSimdMatrixTransferStep},
        Entry {StatementKind::SimdMatrixMultiplyAdd,
               &PlanBuilder::buildSimdMatrixMultiplyAddStep}};

    auto draft = StepDraft {walk, {}, {}};
    draft.step.kind = statement.kind;
    draft.step.value = statement.value;
    draft.step.index = statement.index;
    draft.step.slot = statement.slot;

    auto found = std::ranges::find(builders, statement.kind, &Entry::first);
    auto builder = found->second;

    if (!(this->*builder)(statement, draft))
        return false;

    if (statement.kind != StatementKind::Store)
        walk.pinned = -1;

    plan.steps[stepId] = draft.step;
    pending[stepId] = draft.schedule;
    return true;
}

bool PlanBuilder::buildVariableStep(const Statement& statement, StepDraft& draft)
{
    if (!checkVariableValue(statement))
        return false;

    draft.schedule.roots[0] = statement.value;
    return true;
}

bool PlanBuilder::buildIfStep(const Statement& statement, StepDraft& draft)
{
    auto& walk = draft.walk;

    if (!checkCondition(statement.value))
        return false;

    auto& step = draft.step;
    auto body = buildBlock(statement.body, walk.loopDepth, walk.depth + 1);
    step.body = body.id;
    step.bodiesJumpOut = body.jumpsOut;

    if (statement.elseBody >= 0)
    {
        auto elseBody =
            buildBlock(statement.elseBody, walk.loopDepth, walk.depth + 1);
        step.elseBody = elseBody.id;
        step.bodiesJumpOut = step.bodiesJumpOut || elseBody.jumpsOut;
    }

    walk.jumpsOut = walk.jumpsOut || step.bodiesJumpOut;
    draft.schedule.roots[0] = statement.value;
    return true;
}

bool PlanBuilder::buildLoopStep(const Statement& statement, StepDraft& draft)
{
    auto& walk = draft.walk;

    if (!checkCondition(statement.value))
        return false;

    draft.step.body =
        buildBlock(statement.body, walk.loopDepth + 1, walk.depth + 1).id;
    draft.schedule.roots[0] = statement.value;
    return true;
}

bool PlanBuilder::buildJumpStep(const Statement& statement, StepDraft& draft)
{
    auto& walk = draft.walk;

    if (walk.loopDepth == 0)
    {
        fail("statement {} outside a loop", Miro::enumToString(statement.kind));
        return false;
    }

    walk.jumpsOut = true;
    return true;
}

bool PlanBuilder::buildStoreStep(const Statement& statement, StepDraft& draft)
{
    auto& walk = draft.walk;

    if (!checkStore(statement, 1))
        return false;

    draft.schedule.roots = Array<int, 3> {statement.index, statement.value, -1};

    if (statement.record < 0)
    {
        walk.pinned = -1;
        return true;
    }

    if (!checkNode(statement.record))
        return false;

    if (walk.pinned == statement.record)
    {
        draft.schedule.pinned = statement.record;
    }
    else
    {
        draft.schedule.roots[2] = statement.record;
        walk.pinned = statement.record;
    }

    if (statement.recordComponentsLeft <= 0)
        walk.pinned = -1;

    return true;
}

bool PlanBuilder::buildVectorStoreStep(const Statement& statement, StepDraft& draft)
{
    if (!checkNode(statement.value)
        || !checkStore(statement, componentCount(graph.expr(statement.value).type)))
        return false;

    draft.schedule.roots = Array<int, 3> {statement.index, statement.value, -1};
    return true;
}

bool PlanBuilder::buildTextureStoreStep(const Statement&, StepDraft&)
{
    fail("statement TextureStore: textures are not supported by the CPU executor");
    return false;
}

bool PlanBuilder::buildSharedStoreStep(const Statement& statement, StepDraft& draft)
{
    if (!checkSharedSlot(statement.slot) || !checkIndexAndValue(statement))
        return false;

    if (!checkScalarInteger(statement, statement.index, "an index"))
        return false;

    auto elementType = graph.sharedArrays()[statement.slot].elementType;
    auto valueType = graph.expr(statement.value).type;

    if (valueType != elementType)
    {
        fail("a SharedStore gives shared array {} of {} a value of type {}",
             statement.slot,
             typeName(elementType),
             typeName(valueType));
        return false;
    }

    draft.schedule.roots = Array<int, 3> {statement.index, statement.value, -1};
    return true;
}

bool PlanBuilder::buildBarrierStep(const Statement&, StepDraft&)
{
    return true;
}

bool PlanBuilder::buildReductionStep(const Statement& statement, StepDraft& draft)
{
    if (!checkVariableValue(statement))
        return false;

    auto type = graph.variables()[statement.slot];

    if (!isReducible(type))
    {
        failStatement(statement,
                      "folds a value of type {}; only Float, UInt and Int fold",
                      typeName(type));
        return false;
    }

    draft.step.reduction = statement.reduction;
    draft.step.scope = statement.scope;
    draft.step.type = type;
    draft.schedule.roots[0] = statement.value;
    return true;
}

bool PlanBuilder::buildAtomicAddStep(const Statement& statement, StepDraft& draft)
{
    if (!checkVariable(statement.slot) || !checkSlot(statement.bufferSlot))
        return false;

    if (!checkIndexAndValue(statement)
        || !checkScalarInteger(statement, statement.index, "an index"))
        return false;

    if (plan.slotAccess[static_cast<std::size_t>(statement.bufferSlot)]
        != BufferAccess::Atomic)
    {
        fail("an AtomicAdd names storage slot {}, which is not atomic",
             statement.bufferSlot);
        return false;
    }

    if (graph.variables()[statement.slot] != ValueType::UInt
        || graph.expr(statement.value).type != ValueType::UInt)
    {
        fail("an AtomicAdd adds or returns a value that is not a UInt");
        return false;
    }

    draft.step.buffer = statement.bufferSlot;
    draft.schedule.roots = Array<int, 3> {statement.index, statement.value, -1};
    return true;
}

bool PlanBuilder::buildSimdMatrixFillStep(const Statement& statement,
                                          StepDraft& draft)
{
    if (!checkWholeSimdGroups(statement)
        || !checkFloatFragment(statement, statement.slot))
        return false;

    if (!checkNode(statement.value))
        return false;

    if (graph.expr(statement.value).type != ValueType::Float)
    {
        fail(
            "a SimdMatrixFill fills a fragment with a value that is not a scalar Float");
        return false;
    }

    draft.schedule.roots[0] = statement.value;
    return true;
}

bool PlanBuilder::buildSimdMatrixTransferStep(const Statement& statement,
                                              StepDraft& draft)
{
    if (!checkSimdMatrixTransfer(statement))
        return false;

    auto& step = draft.step;
    step.buffer = statement.bufferSlot;
    step.stride = statement.stride;
    step.memory = statement.memory;
    step.element = graph.simdMatrixElement(statement.slot);
    draft.schedule.roots = Array<int, 3> {statement.index, statement.stride, -1};
    return true;
}

bool PlanBuilder::buildSimdMatrixMultiplyAddStep(const Statement& statement,
                                                 StepDraft& draft)
{
    if (!checkSimdMatrixMultiplyAdd(statement))
        return false;

    draft.step.left = statement.left;
    draft.step.right = statement.right;
    return true;
}

bool PlanBuilder::checkSimdMatrixTransfer(const Statement& statement)
{
    return checkWholeSimdGroups(statement) && checkSimdMatrixElement(statement)
           && checkSimdMatrixMemory(statement)
           && checkScalarInteger(statement, statement.index, "an offset")
           && checkScalarInteger(statement, statement.stride, "a row stride");
}

bool PlanBuilder::checkSimdMatrixMultiplyAdd(const Statement& statement)
{
    return checkWholeSimdGroups(statement)
           && checkFloatFragment(statement, statement.slot)
           && checkFragment(statement, statement.left, "a left")
           && checkFragment(statement, statement.right, "a right");
}

void PlanBuilder::failStatementWith(const Statement& statement,
                                    std::string_view what)
{
    fail("a {} {}", Miro::enumToString(statement.kind), what);
}

bool PlanBuilder::checkNode(NodeId node)
{
    if (node >= 0 && node < graph.nodeCount())
        return true;

    fail("a statement names expression node {}, which does not exist", node);
    return false;
}

bool PlanBuilder::checkIndexAndValue(const Statement& statement)
{
    return checkNode(statement.index) && checkNode(statement.value);
}

bool PlanBuilder::checkVariable(int slot)
{
    if (slot >= 0 && slot < graph.variables().size())
        return true;

    fail("a statement names variable {}, which does not exist", slot);
    return false;
}

bool PlanBuilder::checkVariableValue(const Statement& statement)
{
    if (!checkVariable(statement.slot) || !checkNode(statement.value))
        return false;

    auto variableType = graph.variables()[statement.slot];
    auto valueType = graph.expr(statement.value).type;

    if (valueType == variableType)
        return true;

    fail("statement {} gives variable {} of type {} a value of type {}",
         Miro::enumToString(statement.kind),
         statement.slot,
         typeName(variableType),
         typeName(valueType));
    return false;
}

bool PlanBuilder::checkCondition(NodeId node)
{
    if (!checkNode(node))
        return false;

    if (graph.expr(node).type == ValueType::Bool)
        return true;

    fail("an If or Loop condition is not a scalar Bool");
    return false;
}

bool PlanBuilder::checkSlot(int slot)
{
    if (slot >= 0 && slot < plan.slotCount)
    {
        plan.slotReferenced[static_cast<std::size_t>(slot)] = true;
        return true;
    }

    fail("storage slot {} does not exist", slot);
    return false;
}

bool PlanBuilder::checkStore(const Statement& statement, int width)
{
    if (!checkSlot(statement.slot) || !checkIndexAndValue(statement))
        return false;

    auto valueType = graph.expr(statement.value).type;

    if (!isStorable(valueType, width))
    {
        failStatement(statement, "stores a value of type {}", typeName(valueType));
        return false;
    }

    if (componentCount(graph.expr(statement.index).type) != 1)
    {
        failStatement(statement, "has an index that is not a scalar");
        return false;
    }

    auto slot = static_cast<std::size_t>(statement.slot);

    if (plan.slotAccess[slot] == BufferAccess::Read)
    {
        failStatement(
            statement, "writes storage slot {}, which is read-only", statement.slot);
        return false;
    }

    auto element = plan.slotElement[slot];

    if (isFloatFamily(valueType) == isFloatFamily(element))
        return true;

    failStatement(statement,
                  "stores a value of type {} into storage slot {} of {}",
                  typeName(valueType),
                  statement.slot,
                  typeName(element));
    return false;
}

bool PlanBuilder::checkSharedSlot(int slot)
{
    if (slot >= 0 && slot < graph.sharedArrays().size())
        return true;

    fail("shared array {} does not exist", slot);
    return false;
}

bool PlanBuilder::checkFragment(const Statement& statement,
                                int fragment,
                                std::string_view role)
{
    if (fragment >= 0 && fragment < graph.simdMatrixCount())
        return true;

    failStatement(
        statement, "names {} fragment {}, which does not exist", role, fragment);
    return false;
}

bool PlanBuilder::checkFloatFragment(const Statement& statement, int fragment)
{
    if (!checkFragment(statement, fragment, "the"))
        return false;

    if (graph.simdMatrixElement(fragment) == SimdMatrixElement::Float)
        return true;

    failStatement(
        statement,
        "writes or stores packed fragment {}; only a float fragment is filled, stored or accumulated into",
        fragment);
    return false;
}

// A fragment belongs to a whole SIMD group: Metal requires full ones, and the
// fallback's scratch holds threads / 32 SIMD groups' worth, so a partial one
// indexes past it. The emitter only asserts this.
bool PlanBuilder::checkWholeSimdGroups(const Statement& statement)
{
    auto threads = graph.threadGroupShape().threadCount();

    if (threads % simdGroupWidth == 0)
        return true;

    failStatement(
        statement,
        "needs a threadgroup of whole SIMD groups, a multiple of {} threads, and this one has {}",
        simdGroupWidth,
        threads);
    return false;
}

bool PlanBuilder::checkScalarInteger(const Statement& statement,
                                     int node,
                                     std::string_view role)
{
    if (!checkNode(node))
        return false;

    if (isScalarInteger(graph.expr(node).type))
        return true;

    failStatement(statement, "has {} that is not a scalar integer", role);
    return false;
}

// A load takes any fragment - a Half or BFloat16 one is widened as it is read,
// in SimdMatrix.cpp (Step::element says which) - and a store only a float one.
bool PlanBuilder::checkSimdMatrixElement(const Statement& statement)
{
    if (statement.kind == StatementKind::SimdMatrixStore)
        return checkFloatFragment(statement, statement.slot);

    return checkFragment(statement, statement.slot, "the");
}

bool PlanBuilder::checkSimdMatrixMemory(const Statement& statement)
{
    if (statement.memory != SimdMatrixMemory::Shared)
        return checkSimdMatrixStorage(statement);

    if (!checkSharedSlot(statement.bufferSlot))
        return false;

    if (graph.sharedArrays()[statement.bufferSlot].elementType == ValueType::Float)
        return true;

    failStatement(statement,
                  "reaches shared array {}, which does not hold Float",
                  statement.bufferSlot);
    return false;
}

bool PlanBuilder::checkSimdMatrixStorage(const Statement& statement)
{
    if (!checkSlot(statement.bufferSlot))
        return false;

    auto slot = static_cast<std::size_t>(statement.bufferSlot);

    if (plan.slotElement[slot] != ValueType::Float)
    {
        failStatement(statement,
                      "reaches storage slot {}, which does not hold Float",
                      statement.bufferSlot);
        return false;
    }

    auto storing = statement.kind == StatementKind::SimdMatrixStore;

    if (storing && plan.slotAccess[slot] == BufferAccess::Read)
    {
        failStatement(statement,
                      "writes storage slot {}, which is read-only",
                      statement.bufferSlot);
        return false;
    }

    return true;
}
} // namespace eacp::GPU::CpuCompute
