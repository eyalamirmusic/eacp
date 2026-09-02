#include "ShaderGraph.h"

#include <bit>

namespace eacp::GPU
{
namespace
{
// Whether a node evaluates to something other than a function of its arguments:
// a mutable local, or a resource the kernel may have written since. Two such
// nodes spelled identically are not the same value, so neither they nor
// anything built over them may be shared.
bool dependsOnMutableState(ExprKind kind)
{
    switch (kind)
    {
        case ExprKind::VarRead:
        case ExprKind::BufferRead:
        case ExprKind::AtomicLoad:
        case ExprKind::Sample:
        case ExprKind::Fetch:
        case ExprKind::SharedRead:
            return true;

        case ExprKind::Input:
        case ExprKind::Varying:
        case ExprKind::Uniform:
        case ExprKind::Constant:
        case ExprKind::Construct:
        case ExprKind::Swizzle:
        case ExprKind::Call:
        case ExprKind::Unary:
        case ExprKind::Binary:
        case ExprKind::Compare:
        case ExprKind::Select:
        case ExprKind::Mul:
        case ExprKind::ThreadId:
        case ExprKind::ArrayRead:
        case ExprKind::LocalId:
        case ExprKind::GroupId:
        case ExprKind::GridExtent:
            return false;
    }

    return false;
}
} // namespace

bool ShaderGraph::isPure(int node) const
{
    return node >= 0 && node < pureFlags.size() && pureFlags[node] != 0;
}

bool ShaderGraph::purityOf(const Expr& node) const
{
    if (dependsOnMutableState(node.kind))
        return false;

    for (auto argument: node.args)
        if (!isPure(argument))
            return false;

    return true;
}

// Constants and pure binaries are shared by structure rather than by the call
// that built them, so a base index two separate calls arrive at - the write's
// `gid * 4u` and the read's - is one node and prints under one name. Only these
// two kinds: every other add() registers a slot in a parallel vector before it
// gets here, and returning an existing node would leave that registration
// stranded.
int ShaderGraph::findShared(const Expr& node) const
{
    if (node.kind == ExprKind::Constant)
    {
        auto found = constantCache.find(constantKeyFor(node));
        return found != constantCache.end() ? found->second : -1;
    }

    if (node.kind == ExprKind::Binary)
    {
        auto found = binaryCache.find(binaryKeyFor(node));
        return found != binaryCache.end() ? found->second : -1;
    }

    return -1;
}

ShaderGraph::ConstantKey ShaderGraph::constantKeyFor(const Expr& node)
{
    return {node.type, node.index, std::bit_cast<std::uint32_t>(node.value)};
}

ShaderGraph::BinaryKey ShaderGraph::binaryKeyFor(const Expr& node)
{
    return {node.type, node.op, node.text, node.args[0], node.args[1]};
}

int ShaderGraph::add(Expr node)
{
    auto pure = purityOf(node);

    if (pure)
    {
        auto shared = findShared(node);

        if (shared >= 0)
            return shared;
    }

    auto id = nodes.size();

    if (pure)
    {
        if (node.kind == ExprKind::Constant)
            constantCache.emplace(constantKeyFor(node), id);
        else if (node.kind == ExprKind::Binary)
            binaryCache.emplace(binaryKeyFor(node), id);
    }

    pureFlags.add(pure ? (char) 1 : (char) 0);
    nodes.add(std::move(node));
    return id;
}

int ShaderGraph::addInput(ValueType type)
{
    auto node = Expr {};
    node.kind = ExprKind::Input;
    node.type = type;
    node.index = inputTypes.size();
    inputTypes.add(type);
    inputRates.add(StepRate::PerVertex);
    inputSlots.add(0);
    return add(std::move(node));
}

int ShaderGraph::addInstanceInput(ValueType type)
{
    return addInstanceInput(type, 1);
}

int ShaderGraph::addInstanceInput(ValueType type, int bufferIndex)
{
    auto node = Expr {};
    node.kind = ExprKind::Input;
    node.type = type;
    node.index = inputTypes.size();
    inputTypes.add(type);
    inputRates.add(StepRate::PerInstance);
    inputSlots.add(bufferIndex);
    return add(std::move(node));
}

int ShaderGraph::addVarying(ValueType type, int sourceNode)
{
    auto node = Expr {};
    node.kind = ExprKind::Varying;
    node.type = type;
    node.index = varyingSlots.size();
    varyingSlots.add({type, sourceNode});
    return add(std::move(node));
}

int ShaderGraph::addUniform(ValueType type)
{
    auto node = Expr {};
    node.kind = ExprKind::Uniform;
    node.type = type;
    node.index = uniformTypes.size();
    uniformTypes.add(type);
    return add(std::move(node));
}

int ShaderGraph::addConstant(float value)
{
    auto node = Expr {};
    node.kind = ExprKind::Constant;
    node.type = ValueType::Float;
    node.value = value;
    return add(std::move(node));
}

int ShaderGraph::addUIntConstant(unsigned value)
{
    auto node = Expr {};
    node.kind = ExprKind::Constant;
    node.type = ValueType::UInt;
    node.index = (int) value;
    return add(std::move(node));
}

int ShaderGraph::addIntConstant(int value)
{
    auto node = Expr {};
    node.kind = ExprKind::Constant;
    node.type = ValueType::Int;
    node.index = value;
    return add(std::move(node));
}

int ShaderGraph::addBoolConstant(bool value)
{
    auto node = Expr {};
    node.kind = ExprKind::Constant;
    node.type = ValueType::Bool;
    node.index = value ? 1 : 0;
    return add(std::move(node));
}

int ShaderGraph::addConstruct(ValueType type, Vector<int> args)
{
    auto node = Expr {};
    node.kind = ExprKind::Construct;
    node.type = type;
    node.args = std::move(args);
    return add(std::move(node));
}

int ShaderGraph::addSwizzle(ValueType type, int child, std::string components)
{
    auto node = Expr {};
    node.kind = ExprKind::Swizzle;
    node.type = type;
    node.args.add(child);
    node.text = std::move(components);
    return add(std::move(node));
}

int ShaderGraph::addCall(ValueType type, std::string name, int argument)
{
    auto node = Expr {};
    node.kind = ExprKind::Call;
    node.type = type;
    node.args.add(argument);
    node.text = std::move(name);
    return add(std::move(node));
}

int ShaderGraph::addCall(ValueType type, std::string name, Vector<int> args)
{
    auto node = Expr {};
    node.kind = ExprKind::Call;
    node.type = type;
    node.args = std::move(args);
    node.text = std::move(name);
    return add(std::move(node));
}

int ShaderGraph::addUnary(ValueType type, char op, int child)
{
    auto node = Expr {};
    node.kind = ExprKind::Unary;
    node.type = type;
    node.op = op;
    node.args.add(child);
    return add(std::move(node));
}

int ShaderGraph::addBinary(ValueType type, char op, int lhs, int rhs)
{
    auto node = Expr {};
    node.kind = ExprKind::Binary;
    node.type = type;
    node.op = op;
    node.args.add(lhs);
    node.args.add(rhs);
    return add(std::move(node));
}

int ShaderGraph::addBinary(ValueType type, std::string op, int lhs, int rhs)
{
    auto node = Expr {};
    node.kind = ExprKind::Binary;
    node.type = type;
    node.text = std::move(op);
    node.args.add(lhs);
    node.args.add(rhs);
    return add(std::move(node));
}

int ShaderGraph::addCompare(std::string op, int lhs, int rhs)
{
    return addCompare(ValueType::Bool, std::move(op), lhs, rhs);
}

int ShaderGraph::addCompare(ValueType type, std::string op, int lhs, int rhs)
{
    auto node = Expr {};
    node.kind = ExprKind::Compare;
    node.type = type;
    node.text = std::move(op);
    node.args.add(lhs);
    node.args.add(rhs);
    return add(std::move(node));
}

int ShaderGraph::addSelect(ValueType type,
                           int condition,
                           int whenTrue,
                           int whenFalse)
{
    auto node = Expr {};
    node.kind = ExprKind::Select;
    node.type = type;
    node.args.add(condition);
    node.args.add(whenTrue);
    node.args.add(whenFalse);
    return add(std::move(node));
}

int ShaderGraph::addMul(ValueType type, int left, int right)
{
    auto node = Expr {};
    node.kind = ExprKind::Mul;
    node.type = type;
    node.args.add(left);
    node.args.add(right);
    return add(std::move(node));
}

int ShaderGraph::addTexture(TextureSampling sampling)
{
    textureSamplings.add(sampling);
    textureAccesses.add(TextureAccess::Sample);
    textureKinds.add(TextureKind::Texture2D);
    return textureSamplings.size() - 1;
}

int ShaderGraph::addCubeTexture(TextureSampling sampling)
{
    textureSamplings.add(sampling);
    textureAccesses.add(TextureAccess::Sample);
    textureKinds.add(TextureKind::Cube);
    return textureSamplings.size() - 1;
}

int ShaderGraph::addWritableTexture()
{
    // The sampling is recorded to keep the lists parallel and is never read: a
    // written texture has no sampler on either backend. Spelled out rather than
    // braced - `add({})` is Vector's initializer-list overload with an empty
    // list, which adds nothing at all. The kind is 2D for the same reason: a
    // kernel writes an image, and there is no cube form of that to record.
    textureSamplings.add(TextureSampling {});
    textureAccesses.add(TextureAccess::Write);
    textureKinds.add(TextureKind::Texture2D);
    return textureSamplings.size() - 1;
}

void ShaderGraph::addTextureStore(int slot, int x, int y, int value)
{
    textureStoreList.add({slot, x, y, value});

    auto statement = Statement {StatementKind::TextureStore};
    statement.slot = slot;
    statement.index = x;
    statement.indexY = y;
    statement.value = value;
    addStatement(statement);
}

int ShaderGraph::addSample(int textureSlot, int uv)
{
    auto node = Expr {};
    node.kind = ExprKind::Sample;
    node.type = ValueType::Float4;
    node.index = textureSlot;
    node.args.add(uv);
    return add(std::move(node));
}

int ShaderGraph::addSample(int textureSlot, int uv, int level)
{
    auto node = Expr {};
    node.kind = ExprKind::Sample;
    node.type = ValueType::Float4;
    node.index = textureSlot;
    node.args.add(uv);
    node.args.add(level);
    return add(std::move(node));
}

int ShaderGraph::addFetch(int textureSlot, int coordinates)
{
    auto node = Expr {};
    node.kind = ExprKind::Fetch;
    node.type = ValueType::Float4;
    node.index = textureSlot;
    node.args.add(coordinates);
    return add(std::move(node));
}

int ShaderGraph::addArray(ValueType elementType, Vector<int> elements)
{
    arrayConstants.add({elementType, std::move(elements)});
    return arrayConstants.size() - 1;
}

int ShaderGraph::addArrayRead(int slot, int index)
{
    auto node = Expr {};
    node.kind = ExprKind::ArrayRead;
    node.type = arrayConstants[slot].elementType;
    node.index = slot;
    node.args.add(index);
    return add(std::move(node));
}

int ShaderGraph::addIndexNode(ExprKind kind, DispatchRank forRank, int component)
{
    assert((!rankFixed || rank == forRank)
           && "eacp: a kernel takes either the 1D indices (threadId, localId, "
              "groupId, gridCount) or the 2D ones, never both - the dispatch "
              "has one grid shape");

    rank = forRank;
    rankFixed = true;

    auto node = Expr {};
    node.kind = kind;
    node.type = ValueType::UInt;
    node.index = component;
    return add(std::move(node));
}

int ShaderGraph::addThreadId()
{
    return addIndexNode(ExprKind::ThreadId, DispatchRank::OneD, 0);
}

int ShaderGraph::addThreadPosition(int component)
{
    return addIndexNode(ExprKind::ThreadId, DispatchRank::TwoD, component);
}

int ShaderGraph::addLocalId()
{
    localIdUsed = true;
    return addIndexNode(ExprKind::LocalId, DispatchRank::OneD, 0);
}

int ShaderGraph::addLocalPosition(int component)
{
    localIdUsed = true;
    return addIndexNode(ExprKind::LocalId, DispatchRank::TwoD, component);
}

int ShaderGraph::addGroupId()
{
    groupIdUsed = true;
    return addIndexNode(ExprKind::GroupId, DispatchRank::OneD, 0);
}

int ShaderGraph::addGroupPosition(int component)
{
    groupIdUsed = true;
    return addIndexNode(ExprKind::GroupId, DispatchRank::TwoD, component);
}

int ShaderGraph::addGridExtent(DispatchRank forRank, int component)
{
    return addIndexNode(ExprKind::GridExtent, forRank, component);
}

int ShaderGraph::addSharedArray(ValueType elementType, int elements)
{
    sharedArrayList.add({elementType, elements});
    return sharedArrayList.size() - 1;
}

int ShaderGraph::addSharedRead(int slot, int index)
{
    auto node = Expr {};
    node.kind = ExprKind::SharedRead;
    node.type = sharedArrayList[slot].elementType;
    node.index = slot;
    node.args.add(index);
    return add(std::move(node));
}

void ShaderGraph::addSharedStore(int slot, int index, int value)
{
    auto statement = Statement {StatementKind::SharedStore};
    statement.slot = slot;
    statement.index = index;
    statement.value = value;
    addStatement(statement);
}

void ShaderGraph::addBarrier()
{
    barrierUsed = true;
    addStatement(Statement {StatementKind::Barrier});
}

int ShaderGraph::addStorageBuffer(BufferAccess access)
{
    storageSlots.add(access);
    return storageSlots.size() - 1;
}

int ShaderGraph::addBufferRead(int slot, int index)
{
    auto node = Expr {};
    node.kind = ExprKind::BufferRead;
    node.type = ValueType::Float;
    node.index = slot;
    node.args.add(index);
    return add(std::move(node));
}

void ShaderGraph::addStore(int slot, int index, int value)
{
    storeList.add({slot, index, value});

    auto statement = Statement {StatementKind::Store};
    statement.slot = slot;
    statement.index = index;
    statement.value = value;
    addStatement(statement);
}

int ShaderGraph::addAtomicAdd(int bufferSlot, int index, int value)
{
    atomicUsed = true;

    auto slot = variableTypes.size();
    variableTypes.add(ValueType::UInt);

    auto operation = Statement {StatementKind::AtomicAdd};
    operation.slot = slot;
    operation.bufferSlot = bufferSlot;
    operation.index = index;
    operation.value = value;
    addStatement(operation);

    return slot;
}

int ShaderGraph::addAtomicLoad(int bufferSlot, int index)
{
    auto node = Expr {};
    node.kind = ExprKind::AtomicLoad;
    node.type = ValueType::UInt;
    node.index = bufferSlot;
    node.args.add(index);
    return add(std::move(node));
}

int ShaderGraph::addStatement(Statement newStatement)
{
    statementList.add(newStatement);
    auto index = statementList.size() - 1;
    blocks[openBlocks.back()].statements.add(index);
    return index;
}

int ShaderGraph::addVariable(ValueType type, int initialValue)
{
    auto slot = variableTypes.size();
    variableTypes.add(type);

    auto declaration = Statement {StatementKind::Declare};
    declaration.slot = slot;
    declaration.value = initialValue;
    addStatement(declaration);

    return slot;
}

int ShaderGraph::addVarRead(int slot)
{
    auto node = Expr {};
    node.kind = ExprKind::VarRead;
    node.type = variableTypes[slot];
    node.index = slot;
    return add(std::move(node));
}

void ShaderGraph::assign(int slot, int value)
{
    auto assignment = Statement {StatementKind::Assign};
    assignment.slot = slot;
    assignment.value = value;
    addStatement(assignment);
}

int ShaderGraph::pushBlock()
{
    blocks.add(Block {});
    auto index = blocks.size() - 1;
    openBlocks.add(index);
    return index;
}

void ShaderGraph::popBlock()
{
    openBlocks.pop_back();
}

void ShaderGraph::addIf(int condition, int body, int elseBody)
{
    auto branch = Statement {StatementKind::If};
    branch.value = condition;
    branch.body = body;
    branch.elseBody = elseBody;
    addStatement(branch);
}

void ShaderGraph::addLoop(int condition, int body)
{
    auto loop = Statement {StatementKind::Loop};
    loop.value = condition;
    loop.body = body;
    addStatement(loop);
}

void ShaderGraph::addBreak()
{
    addStatement(Statement {StatementKind::Break});
}

void ShaderGraph::addContinue()
{
    addStatement(Statement {StatementKind::Continue});
}
} // namespace eacp::GPU
