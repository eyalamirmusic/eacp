#include "ShaderGraph.h"

#include "../Frame/ComputePass.h"
#include "PackedScalars.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <limits>
#include <map>
#include <tuple>

namespace eacp::GPU
{
namespace
{
// What a thread index node holds: one lane of the position, or the whole of it
// in the width its rank dispatches over.
ValueType indexNodeType(DispatchRank forRank, int component)
{
    if (component != allComponents)
        return ValueType::UInt;

    return forRank == DispatchRank::TwoD ? ValueType::UInt2 : ValueType::UInt3;
}

// Whether a node evaluates to something other than a function of its arguments:
// a mutable local, or a resource the kernel may have written since. Two such
// nodes spelled identically are not the same value, so neither they nor
// anything built over them may be shared.
//
// A storage-buffer read is listed here and taken back out again by
// readsImmutableStorage below, since which of the two it is depends on the slot
// and not on the kind.
bool dependsOnMutableState(ExprKind kind)
{
    switch (kind)
    {
        case ExprKind::VarRead:
        case ExprKind::BufferRead:
        case ExprKind::BufferVectorRead:
        case ExprKind::AtomicLoad:
        case ExprKind::Sample:
        case ExprKind::Fetch:
        case ExprKind::TextureFunction:
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
        case ExprKind::SimdGroupIndex:
        case ExprKind::BuiltIn:
        case ExprKind::TextureQuery:
        case ExprKind::MatrixColumn:
        case ExprKind::Override:
        case ExprKind::FunctionCall:
            return false;
    }

    return false;
}

// What a function's body may not reach: everything a stage is handed, and the
// constant arrays an entry point declares in its own body.
[[maybe_unused]] bool isStageScoped(ExprKind kind)
{
    switch (kind)
    {
        case ExprKind::Input:
        case ExprKind::Varying:
        case ExprKind::ThreadId:
        case ExprKind::LocalId:
        case ExprKind::GroupId:
        case ExprKind::GridExtent:
        case ExprKind::SharedRead:
        case ExprKind::SimdGroupIndex:
        case ExprKind::BuiltIn:
        case ExprKind::ArrayRead:
            return true;
        default:
            return false;
    }
}

[[maybe_unused]] bool isStageScopedStatement(const Statement& statement)
{
    switch (statement.kind)
    {
        case StatementKind::SharedStore:
        case StatementKind::Barrier:
        case StatementKind::GroupReduce:
        case StatementKind::SimdMatrixFill:
        case StatementKind::SimdMatrixLoad:
        case StatementKind::SimdMatrixStore:
        case StatementKind::SimdMatrixMultiplyAdd:
            return true;
        case StatementKind::Atomic:
            return statement.memory == MemorySpace::Shared;
        default:
            return false;
    }
}

bool isStorageWrite(StatementKind kind)
{
    return kind == StatementKind::Store || kind == StatementKind::VectorStore
           || kind == StatementKind::TextureStore || kind == StatementKind::Atomic;
}

// The statements that name, in `slot`, a variable they write.
[[maybe_unused]] bool writesVariableSlot(StatementKind kind)
{
    return kind == StatementKind::Declare || kind == StatementKind::Assign
           || kind == StatementKind::Atomic;
}

[[maybe_unused]] bool isOverridableType(ValueType type)
{
    return type == ValueType::Bool || type == ValueType::Int
           || type == ValueType::UInt || type == ValueType::Float
           || type == ValueType::Half;
}

[[maybe_unused]] bool isIdentifier(std::string_view name)
{
    auto isLetter = [](char c)
    { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };

    if (name.empty() || !isLetter(name.front()))
        return false;

    auto isLetterOrDigit = [&isLetter](char c)
    { return isLetter(c) || (c >= '0' && c <= '9'); };

    return std::all_of(name.begin(), name.end(), isLetterOrDigit);
}

// Structural sharing for the three kinds that can take it. A key holds
// everything add() would have to compare to call two nodes the same value; a
// binary's operands and a read's index are node ids, which is enough because
// the nodes they name were themselves shared on the way in.
//
// A read's key is its kind and width beside its slot and its index, so a read2
// and a read4 starting at the same element stay two nodes - and only a read of
// a read-only slot is ever pure enough to reach the cache at all.
using ConstantKey = std::tuple<ValueType, int, std::uint32_t>;
using BinaryKey = std::tuple<ValueType, char, std::string, int, int>;
using ReadKey = std::tuple<ExprKind, ValueType, int, int, int>;

ConstantKey constantKeyFor(const Expr& node)
{
    return {node.type, node.index, std::bit_cast<std::uint32_t>(node.value)};
}

BinaryKey binaryKeyFor(const Expr& node)
{
    return {node.type, node.op, node.text, node.args[0], node.args[1]};
}

// The kind tells a scalar read from a record one and the type tells a record's
// width, so a read2 and a read4 at the same first element stay two nodes: they
// are different values, however much of the same memory they cover. Two
// fields of one struct element are two values for the same reason.
ReadKey readKeyFor(const Expr& node)
{
    return {node.kind, node.type, node.index, node.args[0], node.member};
}

constexpr auto vertexStageStatements = Array<StatementKind, 11> {
    StatementKind::Declare,
    StatementKind::Assign,
    StatementKind::If,
    StatementKind::Loop,
    StatementKind::Break,
    StatementKind::Continue,
    StatementKind::BreakIf,
    StatementKind::Switch,
    StatementKind::Case,
    StatementKind::Return,
    StatementKind::Call,
};

bool isVertexStageStatement(StatementKind kind)
{
    return vertexStageStatements.contains(kind);
}

bool fitsSelector(ValueType selector, std::int64_t value)
{
    if (selector == ValueType::Int)
        return value >= std::numeric_limits<std::int32_t>::min()
               && value <= std::numeric_limits<std::int32_t>::max();

    return value >= 0 && value <= std::numeric_limits<std::uint32_t>::max();
}

void setGraphResourceName(Vector<std::string>& names,
                          int slot,
                          const std::string& name)
{
    while (names.size() <= slot)
        names.add(std::string {});

    names[slot] = name;
}

const std::string& graphResourceName(const Vector<std::string>& names, int slot)
{
    static const auto unnamed = std::string {};
    return slot >= 0 && slot < names.size() ? names[slot] : unnamed;
}
} // namespace

struct ShaderGraph::Caches
{
    std::map<ConstantKey, int> constants;
    std::map<BinaryKey, int> binaries;
    std::map<ReadKey, int> reads;
};

ShaderGraph::SharingCaches::SharingCaches()
    : caches(std::make_unique<Caches>())
{
}

ShaderGraph::SharingCaches::~SharingCaches() = default;

std::unique_ptr<ShaderGraph::Caches>
    ShaderGraph::SharingCaches::copyOf(const SharingCaches& other)
{
    if (other.caches)
        return std::make_unique<Caches>(*other.caches);

    return std::make_unique<Caches>();
}

ShaderGraph::SharingCaches::SharingCaches(const SharingCaches& other)
    : caches(copyOf(other))
{
}

ShaderGraph::SharingCaches::SharingCaches(SharingCaches&& other) noexcept = default;

ShaderGraph::SharingCaches&
    ShaderGraph::SharingCaches::operator=(const SharingCaches& other)
{
    if (this != &other)
        caches = copyOf(other);

    return *this;
}

ShaderGraph::SharingCaches&
    ShaderGraph::SharingCaches::operator=(SharingCaches&& other) noexcept = default;

ShaderGraph::Caches& ShaderGraph::SharingCaches::get()
{
    if (!caches)
        caches = std::make_unique<Caches>();

    return *caches;
}

ShaderGraph::ShaderGraph()
{
    blocks.add(Block {});
    openBlocks.add(rootBlock);
}

ShaderGraph::~ShaderGraph() = default;
ShaderGraph::ShaderGraph(const ShaderGraph& other) = default;
ShaderGraph::ShaderGraph(ShaderGraph&& other) noexcept = default;
ShaderGraph& ShaderGraph::operator=(const ShaderGraph& other) = default;
ShaderGraph& ShaderGraph::operator=(ShaderGraph&& other) noexcept = default;

const ShaderGraph::Caches* ShaderGraph::SharingCaches::find() const
{
    return caches.get();
}

const Expr& ShaderGraph::expr(int node) const
{
    return nodes[node];
}

int ShaderGraph::nodeCount() const
{
    return nodes.size();
}

int ShaderGraph::textureCount() const
{
    return textureSamplings.size();
}

TextureSampling ShaderGraph::textureSampling(int slot) const
{
    return slot >= 0 && slot < textureSamplings.size() ? textureSamplings[slot]
                                                       : TextureSampling {};
}

TextureAccess ShaderGraph::textureAccess(int slot) const
{
    return slot >= 0 && slot < textureAccesses.size() ? textureAccesses[slot]
                                                      : TextureAccess::Sample;
}

TextureKind ShaderGraph::textureKind(int slot) const
{
    return slot >= 0 && slot < textureKinds.size() ? textureKinds[slot]
                                                   : TextureKind::Texture2D;
}

TextureFormat ShaderGraph::textureFormat(int slot) const
{
    return slot >= 0 && slot < textureFormats.size() ? textureFormats[slot]
                                                     : TextureFormat::RGBA32Float;
}

ValueType ShaderGraph::storageElementType(int slot) const
{
    return slot >= 0 && slot < storageElements.size() ? storageElements[slot]
                                                      : ValueType::Float;
}

ValueType ShaderGraph::sharedElementType(int slot) const
{
    return slot >= 0 && slot < sharedArrayList.size()
               ? sharedArrayList[slot].elementType
               : ValueType::Float;
}

bool ShaderGraph::usesGroupReduction() const
{
    return !reductionTypes.empty();
}

int ShaderGraph::simdMatrixCount() const
{
    return simdMatrixElementList.size();
}

bool ShaderGraph::usesSimdGroups() const
{
    return simdMatrixCount() > 0 || simdGroupIndexUsed;
}

SimdMatrixElement ShaderGraph::simdMatrixElement(int matrix) const
{
    return simdMatrixElementList[matrix];
}

bool ShaderGraph::isCompute() const
{
    return storeList.size() > 0 || textureStoreList.size() > 0 || atomicUsed
           || usesSimdGroups();
}

const Statement& ShaderGraph::statement(int index) const
{
    return statementList[index];
}

const Block& ShaderGraph::block(int index) const
{
    return blocks[index];
}

int ShaderGraph::statementCount() const
{
    return statementList.size();
}

int ShaderGraph::blockCount() const
{
    return blocks.size();
}

bool ShaderGraph::hasStatements() const
{
    return !blocks[rootBlock].statements.empty();
}

bool ShaderGraph::hasStatementOf(StatementKind kind) const
{
    for (const auto& recorded: statementList)
        if (recorded.kind == kind)
            return true;

    return false;
}

bool ShaderGraph::usesHalfStorage() const
{
    for (auto element: storageElements)
        if (isHalf(element))
            return true;

    for (auto structId: storageStructs)
        if (structId >= 0 && structHoldsHalf(structId))
            return true;

    return false;
}

bool ShaderGraph::usesHalf() const
{
    for (const auto& node: nodes)
        if (isHalf(node.type))
            return true;

    for (auto type: variableTypes)
        if (isHalf(type))
            return true;

    for (auto structId = 0; structId < structTypes.size(); ++structId)
        if (structHoldsHalf(structId))
            return true;

    return usesHalfStorage();
}

bool ShaderGraph::usesSampleRateShading() const
{
    for (const auto& varying: varyingSlots)
        if (varying.interpolation.sampling == InterpolationSampling::Sample)
            return true;

    return usesBuiltIn(ShaderBuiltIn::SampleIndex);
}

bool ShaderGraph::isPure(int node) const
{
    return node >= 0 && node < pureFlags.size() && pureFlags[node] != 0;
}

// The reads the rule above is too coarse for. Nothing can store to a read-only
// slot - ShaderBuilder::write takes an output - so what an element of one holds
// is fixed for the whole kernel, and two reads of it at the same index are the
// same value however far apart they were written. An output's read is not:
// it may hold what this very thread stored a statement ago, which is the whole
// point of OutputBuffer::operator[], so the access the slot was declared with
// is what decides.
//
// The index still has to be pure for the read to be, which purityOf checks for
// every node alike. That is what keeps a read subscripted by a loop counter out
// of this: the counter is a VarRead, so the read over it is impure and neither
// shared nor carried across the assignment that advances it.
bool ShaderGraph::readsImmutableStorage(const Expr& node) const
{
    if (node.kind != ExprKind::BufferRead && node.kind != ExprKind::BufferVectorRead)
        return false;

    return node.index >= 0 && node.index < storageSlots.size()
           && storageSlots[node.index] == BufferAccess::Read;
}

bool ShaderGraph::purityOf(const Expr& node) const
{
    if (dependsOnMutableState(node.kind) && !readsImmutableStorage(node))
        return false;

    if (node.kind == ExprKind::FunctionCall && !functionList[node.index].isPure)
        return false;

    for (auto argument: node.args)
        if (!isPure(argument))
            return false;

    return true;
}

// Constants, pure binaries and reads of read-only buffers are shared by
// structure rather than by the call that built them, so a base index two
// separate calls arrive at - the write's `gid * 4u` and the read's - is one node
// and prints under one name, and two readHalf(scale, i) calls at one index are
// one load rather than two.
//
// Only these three kinds: every other add() registers a slot in a parallel
// vector before it gets here, and returning an existing node would leave that
// registration stranded.
int ShaderGraph::findShared(const Expr& node) const
{
    auto* caches = sharing.find();

    if (caches == nullptr)
        return -1;

    if (node.kind == ExprKind::Constant)
    {
        auto& cache = caches->constants;
        auto found = cache.find(constantKeyFor(node));
        return found != cache.end() ? found->second : -1;
    }

    if (node.kind == ExprKind::Binary)
    {
        auto& cache = caches->binaries;
        auto found = cache.find(binaryKeyFor(node));
        return found != cache.end() ? found->second : -1;
    }

    if (node.kind == ExprKind::BufferRead || node.kind == ExprKind::BufferVectorRead)
    {
        auto& cache = caches->reads;
        auto found = cache.find(readKeyFor(node));
        return found != cache.end() ? found->second : -1;
    }

    return -1;
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
        auto& caches = sharing.get();

        if (node.kind == ExprKind::Constant)
            caches.constants.emplace(constantKeyFor(node), id);
        else if (node.kind == ExprKind::Binary)
            caches.binaries.emplace(binaryKeyFor(node), id);
        else if (node.kind == ExprKind::BufferRead
                 || node.kind == ExprKind::BufferVectorRead)
            caches.reads.emplace(readKeyFor(node), id);
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
    return addVarying(type, sourceNode, defaultInterpolation(type));
}

int ShaderGraph::addVarying(ValueType type,
                            int sourceNode,
                            Interpolation interpolation)
{
    assert(isValidInterpolation(type, interpolation)
           && "eacp: a Flat varying samples First or Either and the others "
              "Center, Centroid or Sample; an Int or UInt varying is Flat.");

    auto node = Expr {};
    node.kind = ExprKind::Varying;
    node.type = type;
    node.index = varyingSlots.size();
    varyingSlots.add({type, sourceNode, interpolation});
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

void ShaderGraph::nameUniform(int slot, const std::string& name)
{
    setGraphResourceName(uniformNames, slot, name);
}

void ShaderGraph::nameStorageBuffer(int slot, const std::string& name)
{
    setGraphResourceName(storageNames, slot, name);
}

void ShaderGraph::nameTexture(int slot, const std::string& name)
{
    setGraphResourceName(textureNames, slot, name);
}

const std::string& ShaderGraph::uniformName(int slot) const
{
    return graphResourceName(uniformNames, slot);
}

const std::string& ShaderGraph::storageBufferName(int slot) const
{
    return graphResourceName(storageNames, slot);
}

const std::string& ShaderGraph::textureName(int slot) const
{
    return graphResourceName(textureNames, slot);
}

int ShaderGraph::addConstant(float value)
{
    auto node = Expr {};
    node.kind = ExprKind::Constant;
    node.type = ValueType::Float;
    node.value = value;
    return add(std::move(node));
}

int ShaderGraph::addHalfConstant(float value)
{
    auto node = Expr {};
    node.kind = ExprKind::Constant;
    node.type = ValueType::Half;
    node.value = halfToFloat(halfFromFloat(value));
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

int ShaderGraph::addTextureSlot(TextureSampling sampling,
                                TextureAccess access,
                                TextureKind kind,
                                TextureFormat format)
{
    textureSamplings.add(sampling);
    textureAccesses.add(access);
    textureKinds.add(kind);
    textureFormats.add(format);
    return textureSamplings.size() - 1;
}

int ShaderGraph::addTexture(TextureSampling sampling)
{
    return addTextureSlot(sampling, TextureAccess::Sample, TextureKind::Texture2D);
}

int ShaderGraph::addCubeTexture(TextureSampling sampling)
{
    return addTextureSlot(sampling, TextureAccess::Sample, TextureKind::Cube);
}

int ShaderGraph::addDepthTexture(TextureSampling sampling)
{
    return addTextureSlot(sampling, TextureAccess::Sample, TextureKind::Depth2D);
}

// Everything addSample records, with the type it gives the node changed - which
// is what makes the emitter print `float d = ...` where a colour sample prints
// `float4 c = ...`, and is the only place the two differ.
int ShaderGraph::addDepthSample(int textureSlot, int uv)
{
    auto node = Expr {};
    node.kind = ExprKind::Sample;
    node.type = ValueType::Float;
    node.index = textureSlot;
    node.args.add(uv);
    return add(std::move(node));
}

int ShaderGraph::addArrayTexture(TextureSampling sampling)
{
    return addTextureSlot(
        sampling, TextureAccess::Sample, TextureKind::Texture2DArray);
}

int ShaderGraph::addDepthArrayTexture(TextureSampling sampling)
{
    return addTextureSlot(
        sampling, TextureAccess::Sample, TextureKind::Depth2DArray);
}

int ShaderGraph::addLayerSample(int textureSlot, int uv, int layer)
{
    auto node = Expr {};
    node.kind = ExprKind::Sample;
    node.type =
        isDepth(textureKind(textureSlot)) ? ValueType::Float : ValueType::Float4;
    node.index = textureSlot;
    node.args.add(uv);
    node.args.add(layer);
    return add(std::move(node));
}

int ShaderGraph::addCompareSample(int textureSlot, int uv, int reference)
{
    auto node = Expr {};
    node.kind = ExprKind::Sample;
    node.type = ValueType::Float;
    node.index = textureSlot;
    node.text = "compare";
    node.args.add(uv);
    node.args.add(reference);
    return add(std::move(node));
}

int ShaderGraph::addCompareSample(int textureSlot, int uv, int layer, int reference)
{
    auto node = Expr {};
    node.kind = ExprKind::Sample;
    node.type = ValueType::Float;
    node.index = textureSlot;
    node.text = "compare";
    node.args.add(uv);
    node.args.add(layer);
    node.args.add(reference);
    return add(std::move(node));
}

int ShaderGraph::addBuiltIn(ShaderBuiltIn which)
{
    builtInsUsed |= builtInBit(which);

    auto node = Expr {};
    node.kind = ExprKind::BuiltIn;
    node.type = builtInType(which);
    node.index = (int) which;
    return add(std::move(node));
}

int ShaderGraph::addWritableTexture()
{
    // The sampling is recorded to keep the lists parallel and is never read: a
    // written texture has no sampler on either backend. Spelled out rather than
    // braced - `add({})` is Vector's initializer-list overload with an empty
    // list, which adds nothing at all. The kind is 2D for the same reason: a
    // kernel writes an image, and there is no cube form of that to record.
    return addTextureSlot(
        TextureSampling {}, TextureAccess::Write, TextureKind::Texture2D);
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

int ShaderGraph::addStorageTexture(TextureKind kind,
                                   TextureFormat format,
                                   TextureAccess access)
{
    assert(isStorageAccess(access) && isStorageTextureFormat(format)
           && (kind == TextureKind::Texture2D || kind == TextureKind::Texture2DArray
               || kind == TextureKind::Texture3D));

    return addTextureSlot(TextureSampling {}, access, kind, format);
}

// The level rides along as a constant 0 so the read binding's load prints as
// a load of a sampled texture's level 0 does; the read-write one ignores it.
int ShaderGraph::addStorageLoad(int slot, int coordinates, int layer)
{
    assert(textureAccess(slot) == TextureAccess::Read
           || textureAccess(slot) == TextureAccess::ReadWrite);

    auto node = Expr {};
    node.kind = ExprKind::TextureFunction;
    node.type = storageTexelType(textureFormat(slot));
    node.index = slot;
    node.text = std::string(textureFunctionName(TextureFunction::Load));
    node.op = 'x';
    node.args.add(coordinates);

    if (layer >= 0)
        node.args.add(layer);

    node.args.add(addIntConstant(0));
    return add(std::move(node));
}

void ShaderGraph::addStorageStore(int slot, int coordinates, int layer, int value)
{
    assert(textureAccess(slot) == TextureAccess::Write
           || textureAccess(slot) == TextureAccess::ReadWrite);

    textureStoreList.add({slot, coordinates, -1, value, layer});

    auto statement = Statement {StatementKind::TextureStore};
    statement.slot = slot;
    statement.index = coordinates;
    statement.layer = layer;
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

int ShaderGraph::addTextureOfKind(TextureKind kind, TextureSampling sampling)
{
    return addTextureSlot(sampling, TextureAccess::Sample, kind);
}

// The comparison rides in the slot's sampling, which no sampler slot reads
// otherwise.
int ShaderGraph::addSampler(bool comparison)
{
    auto sampling = TextureSampling {};
    sampling.compare = comparison ? TextureCompare::LessEqual : TextureCompare::None;

    return addTextureSlot(sampling, TextureAccess::Sampler, TextureKind::Texture2D);
}

bool ShaderGraph::isComparisonSampler(int slot) const
{
    return textureAccess(slot) == TextureAccess::Sampler
           && textureSampling(slot).compare != TextureCompare::None;
}

namespace
{
constexpr auto textureFunctionNames = Array<std::string_view, 9> {"sample",
                                                                  "level",
                                                                  "bias",
                                                                  "grad",
                                                                  "compare",
                                                                  "compareLevel",
                                                                  "gather",
                                                                  "gatherCompare",
                                                                  "load"};

constexpr auto gatherComponents = std::string_view {"xyzw"};

ValueType textureFunctionType(TextureKind kind, TextureFunction function)
{
    switch (function)
    {
        case TextureFunction::Gather:
        case TextureFunction::GatherCompare:
            return ValueType::Float4;
        case TextureFunction::Compare:
        case TextureFunction::CompareLevel:
            return ValueType::Float;
        default:
            return isDepth(kind) ? ValueType::Float : ValueType::Float4;
    }
}

ValueType textureQueryType(TextureKind kind, const std::string& query)
{
    if (query != "size")
        return ValueType::UInt;

    return kind == TextureKind::Texture3D ? ValueType::UInt3 : ValueType::UInt2;
}
} // namespace

std::string_view textureFunctionName(TextureFunction function)
{
    return textureFunctionNames[(int) function];
}

TextureFunction textureFunctionNamed(std::string_view name)
{
    for (auto i = 0; i < textureFunctionNames.size(); ++i)
        if (textureFunctionNames[i] == name)
            return static_cast<TextureFunction>(i);

    return TextureFunction::Sample;
}

int ShaderGraph::addTextureFunction(int textureSlot,
                                    TextureFunction function,
                                    const Vector<int>& args,
                                    int component,
                                    int samplerSlot)
{
    assert(component >= 0 && component < 4);
    assert(textureAccess(textureSlot) != TextureAccess::Sampler);
    assert(
        (samplerSlot < 0
         || (textureAccess(samplerSlot) == TextureAccess::Sampler
             && isComparisonSampler(samplerSlot) == isComparisonFunction(function)))
        && "eacp: a comparison function samples through a comparison "
           "sampler, every other one through a plain sampler");

    auto node = Expr {};
    node.sampler = samplerSlot;
    node.kind = ExprKind::TextureFunction;
    node.type = textureFunctionType(textureKind(textureSlot), function);
    node.index = textureSlot;
    node.text = std::string(textureFunctionName(function));
    node.op = gatherComponents[(std::size_t) component];
    node.args = args;
    return add(std::move(node));
}

int ShaderGraph::addTextureQuery(int textureSlot,
                                 const std::string& query,
                                 int level)
{
    auto node = Expr {};
    node.kind = ExprKind::TextureQuery;
    node.type = textureQueryType(textureKind(textureSlot), query);
    node.index = textureSlot;
    node.text = query;

    if (level >= 0)
        node.args.add(level);

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
           && "eacp: a kernel takes the 1D indices (threadId, localId, groupId, "
              "gridCount), the 2D ones or the 3D ones, never two sets - the "
              "dispatch has one grid shape");

    rank = forRank;
    rankFixed = true;

    auto node = Expr {};
    node.kind = kind;
    node.type = indexNodeType(forRank, component);
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

int ShaderGraph::addThreadPosition3(int component)
{
    return addIndexNode(ExprKind::ThreadId, DispatchRank::ThreeD, component);
}

int ShaderGraph::addThreadId2()
{
    return addIndexNode(ExprKind::ThreadId, DispatchRank::TwoD, allComponents);
}

int ShaderGraph::addThreadId3()
{
    return addIndexNode(ExprKind::ThreadId, DispatchRank::ThreeD, allComponents);
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

int ShaderGraph::addLocalPosition3(int component)
{
    localIdUsed = true;
    return addIndexNode(ExprKind::LocalId, DispatchRank::ThreeD, component);
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

int ShaderGraph::addGroupPosition3(int component)
{
    groupIdUsed = true;
    return addIndexNode(ExprKind::GroupId, DispatchRank::ThreeD, component);
}

int ShaderGraph::addLocalId2()
{
    localIdUsed = true;
    return addIndexNode(ExprKind::LocalId, DispatchRank::TwoD, allComponents);
}

int ShaderGraph::addLocalId3()
{
    localIdUsed = true;
    return addIndexNode(ExprKind::LocalId, DispatchRank::ThreeD, allComponents);
}

int ShaderGraph::addGroupId2()
{
    groupIdUsed = true;
    return addIndexNode(ExprKind::GroupId, DispatchRank::TwoD, allComponents);
}

int ShaderGraph::addGroupId3()
{
    groupIdUsed = true;
    return addIndexNode(ExprKind::GroupId, DispatchRank::ThreeD, allComponents);
}

int ShaderGraph::addGridExtent(DispatchRank forRank, int component)
{
    // The width and the height are the same two uniforms whether the kernel is
    // 2D or 3D, so asking for one leaves a rank already fixed at ThreeD alone.
    auto wanted = forRank == DispatchRank::TwoD && rank == DispatchRank::ThreeD
                      ? rank
                      : forRank;

    return addIndexNode(ExprKind::GridExtent, wanted, component);
}

int ShaderGraph::addSharedArray(ValueType elementType, int elements, bool atomic)
{
    assert(
        (!atomic || elementType == ValueType::UInt || elementType == ValueType::Int)
        && "eacp: an atomic shared array holds UInt or Int");

    sharedArrayList.add({elementType, elements, atomic});
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

int ShaderGraph::addGroupReduction(GroupReduction operation,
                                   ValueType elementType,
                                   int value,
                                   ReductionScope scope)
{
    barrierUsed = true;

    if (!reductionTypes.contains(elementType))
        reductionTypes.add(elementType);

    if (scope == ReductionScope::Simd)
        simdReductionUsed = true;
    else if (!wholeGroupTypes.contains(elementType))
        wholeGroupTypes.add(elementType);

    auto slot = addVariableSlot(elementType);

    auto fold = Statement {StatementKind::GroupReduce};
    fold.slot = slot;
    fold.value = value;
    fold.reduction = operation;
    fold.scope = scope;
    addStatement(fold);

    return slot;
}

// The four matrix statements, and after them the index that places a SIMD
// group's tile. Each of the four marks the kernel as barriering, for the reason
// a reduction does: an intrinsic collective over a SIMD group is undefined
// where some of its lanes returned early, so a kernel holding one gets no
// bounds guard and bounds its own stores. The index marks nothing, being a read
// of a builtin rather than anything collective.
int ShaderGraph::addSimdMatrixFill(int value)
{
    barrierUsed = true;

    auto matrix = declareSimdMatrix(SimdMatrixElement::Float);

    auto fill = Statement {StatementKind::SimdMatrixFill};
    fill.slot = matrix;
    fill.value = value;
    addStatement(fill);

    return matrix;
}

int ShaderGraph::addSimdMatrixLoad(SimdMatrixMemory memory,
                                   int slot,
                                   int index,
                                   int stride,
                                   SimdMatrixElement element)
{
    assert(
        (element == SimdMatrixElement::Float || memory == SimdMatrixMemory::Buffer)
        && "eacp: a packed fragment is loaded out of a storage buffer. A "
           "threadgroup tile holds floats, so a patch of one is already the "
           "fragment simdMatrix(tile, ...) reads.");

    barrierUsed = true;

    auto matrix = declareSimdMatrix(element);

    auto load = Statement {StatementKind::SimdMatrixLoad};
    load.slot = matrix;
    load.memory = memory;
    load.bufferSlot = slot;
    load.index = index;
    load.stride = stride;
    load.element = element;
    addStatement(load);

    return matrix;
}

int ShaderGraph::declareSimdMatrix(SimdMatrixElement element)
{
    simdMatrixElementList.add(element);
    return simdMatrixElementList.size() - 1;
}

bool ShaderGraph::usesPackedSimdMatrix(SimdMatrixElement element) const
{
    return simdMatrixElementList.contains(element);
}

void ShaderGraph::addSimdMatrixStore(
    int matrix, SimdMatrixMemory memory, int slot, int index, int stride)
{
    assert(simdMatrixElement(matrix) == SimdMatrixElement::Float
           && "eacp: a packed fragment cannot be stored - there is no "
              "instruction that writes one back. Multiply it into a float "
              "accumulator and store that.");

    barrierUsed = true;

    auto store = Statement {StatementKind::SimdMatrixStore};
    store.slot = matrix;
    store.memory = memory;
    store.bufferSlot = slot;
    store.index = index;
    store.stride = stride;
    addStatement(store);
}

void ShaderGraph::addSimdMatrixMultiplyAdd(int accumulator, int left, int right)
{
    assert(simdMatrixElement(accumulator) == SimdMatrixElement::Float
           && "eacp: a product accumulates into a float fragment. A packed one "
              "is an operand only - sixteen bits would lose what the sum is "
              "being accumulated in.");

    barrierUsed = true;

    auto product = Statement {StatementKind::SimdMatrixMultiplyAdd};
    product.slot = accumulator;
    product.left = left;
    product.right = right;
    addStatement(product);
}

int ShaderGraph::addSimdGroupIndex()
{
    simdGroupIndexUsed = true;

    auto node = Expr {};
    node.kind = ExprKind::SimdGroupIndex;
    node.type = ValueType::UInt;
    return add(std::move(node));
}

ThreadGroupShape ShaderGraph::threadGroupShape() const
{
    if (groupShape.isSet())
        return groupShape;

    if (rank == DispatchRank::OneD)
        return {ComputePass::threadGroupWidth, 1, 1};

    if (rank == DispatchRank::TwoD)
        return {ComputePass::threadGroupSize2D, ComputePass::threadGroupSize2D, 1};

    return {ComputePass::threadGroupSize3D,
            ComputePass::threadGroupSize3D,
            ComputePass::threadGroupSize3D};
}

// What one element of a threadgroup array really costs, which is not always
// what the value occupies: an std430 block and a DXBC groupshared array both
// round a vector's stride up to sixteen bytes, so an array of three-vectors is
// a quarter larger than its components add up to. MSL packs them to twelve, so
// this is the worst of the three - which is what a budget is asked for.
int threadgroupElementBytes(ValueType type)
{
    auto bytes = byteSize(type);

    return componentCount(type) > 1 && bytes < 16 ? 16 : bytes;
}

// The kernel's declarations plus the emitter's own: one scratch array per
// element type any reduction folds, sized to the group, and one slice of a
// SIMD-group matrix scratch per SIMD group of it - two whole fragments each,
// which is what the product stages between its barriers.
//
// What it does not count is the padding *between* arrays. A backend may align
// one array's base against the next, so a kernel sitting within a few bytes of
// the budget may still be refused; the number is a bound on the declarations
// and not a byte-exact prediction of the allocation.
int ShaderGraph::threadgroupMemoryBytes() const
{
    auto threads = threadGroupShape().threadCount();
    auto bytes = 0;

    for (const auto& shared: sharedArrayList)
        bytes += shared.elements * threadgroupElementBytes(shared.elementType);

    for (auto elementType: reductionTypes)
        bytes += threads * threadgroupElementBytes(elementType);

    if (simdMatrixCount() > 0)
        bytes += threads / simdGroupWidth * 2 * simdMatrixSize * simdMatrixSize
                 * (int) sizeof(float);

    return bytes;
}

int ShaderGraph::addStorageBuffer(BufferAccess access, ValueType elementType)
{
    storageSlots.add(access);
    storageElements.add(elementType);
    storageStructs.add(-1);
    return storageSlots.size() - 1;
}

int ShaderGraph::addBufferRead(int slot, int index)
{
    auto node = Expr {};
    node.kind = ExprKind::BufferRead;
    node.type = storageElementType(slot);
    node.index = slot;
    node.args.add(index);
    return add(std::move(node));
}

int ShaderGraph::addBufferVectorRead(int slot, int firstElement, ValueType type)
{
    auto node = Expr {};
    node.kind = ExprKind::BufferVectorRead;
    node.type = type;
    node.index = slot;
    node.args.add(firstElement);
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

void ShaderGraph::addVectorStore(int slot, int firstElement, int value)
{
    storeList.add({slot, firstElement, value});

    auto statement = Statement {StatementKind::VectorStore};
    statement.slot = slot;
    statement.index = firstElement;
    statement.value = value;
    addStatement(statement);
}

void ShaderGraph::addRecordStore(int slot,
                                 const Vector<int>& indices,
                                 const Vector<int>& components,
                                 int record)
{
    for (auto component = 0; component < components.size(); ++component)
    {
        storeList.add({slot, indices[component], components[component]});

        auto statement = Statement {StatementKind::Store};
        statement.slot = slot;
        statement.index = indices[component];
        statement.value = components[component];
        statement.record = record;
        statement.recordComponentsLeft = components.size() - 1 - component;
        addStatement(statement);
    }
}

ShaderGraph::AtomicVariables ShaderGraph::addAtomic(AtomicOperation operation,
                                                    MemorySpace memory,
                                                    int slot,
                                                    int index,
                                                    int value,
                                                    int compare)
{
    assert((operation == AtomicOperation::CompareExchange) == (compare >= 0)
           && "eacp: a comparand belongs to a compare-exchange, and only there");

    atomicUsed = true;

    auto elementType = memory == MemorySpace::Buffer ? storageElementType(slot)
                                                     : sharedElementType(slot);

    auto variables = AtomicVariables {};
    variables.previous = addVariableSlot(elementType);

    if (compare >= 0)
        variables.exchanged = addVariableSlot(ValueType::Bool);

    auto statement = Statement {StatementKind::Atomic};
    statement.slot = variables.previous;
    statement.atomic = operation;
    statement.memory = memory;
    statement.bufferSlot = slot;
    statement.index = index;
    statement.value = value;
    statement.compare = compare;
    statement.exchanged = variables.exchanged;
    addStatement(statement);

    return variables;
}

int ShaderGraph::addAtomicAdd(int bufferSlot, int index, int value)
{
    return addAtomic(
               AtomicOperation::Add, MemorySpace::Buffer, bufferSlot, index, value)
        .previous;
}

int ShaderGraph::addAtomicLoad(int bufferSlot, int index)
{
    auto node = Expr {};
    node.kind = ExprKind::AtomicLoad;
    node.type = storageElementType(bufferSlot);
    node.index = bufferSlot;
    node.args.add(index);
    return add(std::move(node));
}

int ShaderGraph::addStatement(Statement newStatement)
{
    assert((!recordingVertexStage || isVertexStageStatement(newStatement.kind))
           && "eacp: the vertex stage records var, assign and the control "
              "statements only. Storage writes, atomics, barriers and group "
              "operations belong to a kernel, and discard to the fragment "
              "stage.");

    auto& open = blocks[openBlocks.back()];

    assert(((open.role == BlockRole::Cases)
            == (newStatement.kind == StatementKind::Case))
           && "eacp: a switch's clause list holds its Case statements and "
              "nothing else.");

    assert((open.statements.empty()
            || statementList[open.statements.back()].kind != StatementKind::BreakIf)
           && "eacp: breakLoopIf is the last statement of a continuing block.");

    statementList.add(newStatement);
    auto index = statementList.size() - 1;
    open.statements.add(index);
    return index;
}

BlockRole ShaderGraph::innermostJumpTarget() const
{
    for (auto at = openBlocks.size() - 1; at >= 0; --at)
    {
        auto role = blocks[openBlocks[at]].role;

        if (role == BlockRole::LoopBody || role == BlockRole::Continuing)
            return role;
    }

    return BlockRole::Plain;
}

bool ShaderGraph::isInsideContinuing() const
{
    for (auto open: openBlocks)
        if (blocks[open].role == BlockRole::Continuing)
            return true;

    return false;
}

bool ShaderGraph::areValidLabels(int selector, int cases) const
{
    auto type = nodes[selector].type;

    if (type != ValueType::Int && type != ValueType::UInt)
        return false;

    auto defaults = 0;
    auto seen = Vector<std::int64_t> {};

    for (auto clause: blocks[cases].statements)
    {
        const auto& labels = caseLabelList[statementList[clause].slot];
        defaults += labels.isDefault ? 1 : 0;

        for (auto value: labels.values)
        {
            if (!fitsSelector(type, value) || seen.contains(value))
                return false;

            seen.add(value);
        }
    }

    return defaults <= 1 && !blocks[cases].statements.empty();
}

int ShaderGraph::addVariableSlot(ValueType type)
{
    variableTypes.add(type);
    vertexVariables.add(recordingVertexStage ? char {1} : char {0});
    variableSizes.add(0);
    variableStructs.add(-1);
    variableFunctions.add(recordingFunction);
    return variableTypes.size() - 1;
}

int ShaderGraph::variableArraySize(int slot) const
{
    return slot >= 0 && slot < variableSizes.size() ? variableSizes[slot] : 0;
}

// min(i, size - 1) on a UInt, min(max(i, 0), size - 1) on an Int; a literal is
// clamped here and stays a literal.
int ShaderGraph::clampedIndex(int index, int size)
{
    auto type = nodes[index].type;
    auto isLiteral = nodes[index].kind == ExprKind::Constant;
    auto literal = nodes[index].index;
    auto last = size - 1;
    auto isUnsigned = type == ValueType::UInt;

    assert((isUnsigned || type == ValueType::Int)
           && "eacp: an array or a matrix column is indexed by an Int or a UInt");

    if (isLiteral)
    {
        if (isUnsigned)
            return addIntConstant(static_cast<int>(std::min(
                static_cast<unsigned>(literal), static_cast<unsigned>(last))));

        return addIntConstant(std::clamp(literal, 0, last));
    }

    if (isUnsigned)
        return addCall(ValueType::UInt,
                       "min",
                       Vector<int> {index, addUIntConstant((unsigned) last)});

    auto low =
        addCall(ValueType::Int, "max", Vector<int> {index, addIntConstant(0)});
    return addCall(ValueType::Int, "min", Vector<int> {low, addIntConstant(last)});
}

int ShaderGraph::addZero(ValueType type)
{
    if (isMatrix(type))
    {
        auto column = addZero(matrixColumnType(type));
        auto columns = Vector<int> {};
        columns.resize(matrixColumns(type), column);
        return addConstruct(type, std::move(columns));
    }

    auto scalar = isBoolean(type)           ? addBoolConstant(false)
                  : isSignedInteger(type)   ? addIntConstant(0)
                  : isUnsignedInteger(type) ? addUIntConstant(0u)
                  : isHalf(type)            ? addHalfConstant(0.0f)
                                            : addConstant(0.0f);

    if (componentCount(type) == 1)
        return scalar;

    auto components = Vector<int> {};
    components.resize(componentCount(type), scalar);
    return addConstruct(type, std::move(components));
}

int ShaderGraph::addArrayVariable(ValueType elementType, int size, int fill)
{
    assert(size > 0 && "eacp: a private array has at least one element");
    assert(nodes[fill].type == elementType
           && "eacp: a private array is filled with a value of its element type");

    auto slot = addVariableSlot(elementType);
    variableSizes[slot] = size;

    auto declaration = Statement {StatementKind::Declare};
    declaration.slot = slot;
    declaration.value = fill;
    addStatement(declaration);

    return slot;
}

int ShaderGraph::addVarElementRead(int slot, int index)
{
    assert(variableArraySize(slot) > 0 && "eacp: only a private array is indexed");

    auto node = Expr {};
    node.kind = ExprKind::VarRead;
    node.type = variableTypes[slot];
    node.index = slot;
    node.args.add(clampedIndex(index, variableSizes[slot]));
    return add(std::move(node));
}

void ShaderGraph::assignElement(int slot, int index, int value)
{
    assert(variableArraySize(slot) > 0 && "eacp: only a private array is indexed");
    assert(isVertexVariable(slot) == recordingVertexStage
           && "eacp: a variable is assigned in the stage that declared it - a "
              "vertexStage variable inside vertexStage, any other outside it.");
    assert(nodes[value].type == variableTypes[slot]
           && (variableStructType(slot) < 0
               || fitsField(variableStructType(slot), -1, value))
           && "eacp: a private array's element takes a value of its type");

    auto assignment = Statement {StatementKind::Assign};
    assignment.slot = slot;
    assignment.index = clampedIndex(index, variableSizes[slot]);
    assignment.value = value;
    addStatement(assignment);
}

int ShaderGraph::addMatrixColumn(int matrix, int index)
{
    auto type = nodes[matrix].type;
    assert(isMatrix(type) && "eacp: only a matrix has columns");

    auto node = Expr {};
    node.kind = ExprKind::MatrixColumn;
    node.type = matrixColumnType(type);
    node.args.add(matrix);
    node.args.add(clampedIndex(index, matrixColumns(type)));
    return add(std::move(node));
}

bool ShaderGraph::isVertexVariable(int slot) const
{
    return slot >= 0 && slot < vertexVariables.size() && vertexVariables[slot] != 0;
}

void ShaderGraph::beginVertexStage()
{
    assert(recordingFunction < 0
           && "eacp: vertexStage opens in define(), not in a function's body.");
    assert(!recordingVertexStage && openBlocks.size() == 1
           && "eacp: vertexStage opens at the top of define(), not inside "
              "another vertexStage or inside an ifThen or a loop.");

    if (vertexRootBlock < 0)
    {
        blocks.add(Block {});
        vertexRootBlock = blocks.size() - 1;
    }

    openBlocks.add(vertexRootBlock);
    recordingVertexStage = true;
}

void ShaderGraph::endVertexStage()
{
    openBlocks.pop_back();
    recordingVertexStage = false;
}

int ShaderGraph::addVariable(ValueType type, int initialValue)
{
    auto slot = addVariableSlot(type);

    auto declaration = Statement {StatementKind::Declare};
    declaration.slot = slot;
    declaration.value = initialValue;
    addStatement(declaration);

    return slot;
}

int ShaderGraph::addVarRead(int slot)
{
    assert(variableArraySize(slot) == 0
           && "eacp: a private array is read an element at a time");
    assert(variableStructType(slot) < 0
           && "eacp: a struct variable is read a field at a time");

    auto node = Expr {};
    node.kind = ExprKind::VarRead;
    node.type = variableTypes[slot];
    node.index = slot;
    return add(std::move(node));
}

void ShaderGraph::assign(int slot, int value)
{
    assert(variableArraySize(slot) == 0
           && "eacp: a private array is written an element at a time");
    assert(isVertexVariable(slot) == recordingVertexStage
           && "eacp: a variable is assigned in the stage that declared it - a "
              "vertexStage variable inside vertexStage, any other outside it.");
    assert((variableStructType(slot) < 0
            || fitsField(variableStructType(slot), -1, value))
           && "eacp: a struct variable takes a value of its struct");

    auto assignment = Statement {StatementKind::Assign};
    assignment.slot = slot;
    assignment.value = value;
    addStatement(assignment);
}

int ShaderGraph::pushBlock(BlockRole role)
{
    blocks.add(Block {{}, role});
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

void ShaderGraph::addLoop(int condition, int body, int continuing)
{
    auto loop = Statement {StatementKind::Loop};
    loop.value = condition;
    loop.body = body;
    loop.continuing = continuing;
    addStatement(loop);
}

void ShaderGraph::addBreak()
{
    assert(innermostJumpTarget() != BlockRole::Continuing
           && "eacp: a continuing block leaves its loop through breakLoopIf "
              "alone, as WGSL's does.");

    addStatement(Statement {StatementKind::Break});
}

void ShaderGraph::addContinue()
{
    assert(innermostJumpTarget() != BlockRole::Continuing
           && "eacp: continueLoop has no meaning inside a continuing block.");

    addStatement(Statement {StatementKind::Continue});
}

void ShaderGraph::addBreakIf(int condition)
{
    assert(blocks[openBlocks.back()].role == BlockRole::Continuing
           && "eacp: breakLoopIf belongs to a loop's continuing block, as its "
              "last statement, as WGSL's break if does.");

    auto breakIf = Statement {StatementKind::BreakIf};
    breakIf.value = condition;
    addStatement(breakIf);
}

void ShaderGraph::addCase(const CaseLabels& labels, int body)
{
    caseLabelList.add(labels);

    auto clause = Statement {StatementKind::Case};
    clause.slot = caseLabelList.size() - 1;
    clause.body = body;
    addStatement(clause);
}

void ShaderGraph::addSwitch(int selector, int cases)
{
    assert(areValidLabels(selector, cases)
           && "eacp: a switch takes a scalar Int or UInt, and at least one "
              "clause; each label fits that type and appears once, and there "
              "is at most one defaultCase.");

    auto choice = Statement {StatementKind::Switch};
    choice.value = selector;
    choice.body = cases;
    addStatement(choice);
}

const CaseLabels& ShaderGraph::caseLabels(int clause) const
{
    return caseLabelList[clause];
}

int ShaderGraph::caseCount() const
{
    return caseLabelList.size();
}

void ShaderGraph::addReturn()
{
    assert(!isInsideContinuing()
           && "eacp: a continuing block cannot return, as WGSL's cannot.");
    assert((recordingFunction < 0 || !functionList[recordingFunction].returnsValue)
           && "eacp: a function with a result returns one - returnEarly(value).");

    addStatement(Statement {StatementKind::Return});
}

void ShaderGraph::addReturn(int value)
{
    assert(recordingFunction >= 0 && functionList[recordingFunction].returnsValue
           && "eacp: only a function with a result returns a value; a stage's "
              "returnEarly() takes none.");
    assert(!isInsideContinuing()
           && "eacp: a continuing block cannot return, as WGSL's cannot.");

    [[maybe_unused]] const auto& declaration = functionList[recordingFunction];
    assert(nodes[value].type == declaration.returnType
           && (declaration.returnType != ValueType::Struct
               || nodes[value].structType == declaration.returnStruct)
           && "eacp: a function returns a value of its result's type.");

    auto statement = Statement {StatementKind::Return};
    statement.value = value;
    addStatement(statement);
}

void ShaderGraph::addDiscard()
{
    addStatement(Statement {StatementKind::Discard});
}

bool ShaderGraph::matchesDeclaredStruct(const StructType& declared,
                                        const StructType& type) const
{
    if (declared.members.size() != type.members.size())
        return false;

    for (auto which = 0; which < type.members.size(); ++which)
    {
        const auto& a = declared.members[which];
        const auto& b = type.members[which];

        if (a.name != b.name || a.type != b.type || a.structType != b.structType
            || a.offset != b.offset)
            return false;
    }

    return true;
}

// A nested struct's fields are copied in behind its own, their paths and
// offsets taken from where it sits; a value's components are its own.
void ShaderGraph::addStructFields(StructType& target,
                                  const StructType& source,
                                  int sourceId,
                                  const std::string& prefix,
                                  int baseOffset) const
{
    for (const auto& member: source.members)
    {
        auto field = StructField {};
        field.path = prefix + member.name;
        field.type = member.type;
        field.structType = member.structType;
        field.offset = baseOffset + member.offset;
        field.firstComponent = target.components.size();
        field.owner = sourceId;

        auto index = target.fields.size();
        target.fields.add(field);

        if (member.type == ValueType::Struct)
        {
            addStructFields(target,
                            structTypes[member.structType],
                            member.structType,
                            field.path + ".",
                            field.offset);
        }
        else
        {
            for (auto component = 0; component < componentCount(member.type);
                 ++component)
                target.components.add(
                    {field.offset + storageComponentOffset(member.type, component),
                     isHalf(member.type)});

            if (isBoolean(member.type))
                target.hostShareable = false;
        }

        target.fields[index].componentCount =
            target.components.size() - field.firstComponent;
    }
}

int ShaderGraph::addStruct(const StructType& type)
{
    for (auto id = 0; id < structTypes.size(); ++id)
    {
        if (structTypes[id].name != type.name)
            continue;

        assert(matchesDeclaredStruct(structTypes[id], type)
               && "eacp: two different structs share one name in a shader");
        return id;
    }

    auto declared = type;
    declared.fields.clear();
    declared.components.clear();

    for (const auto& member: type.members)
    {
        assert(
            (member.type != ValueType::Struct
             || (member.structType >= 0 && member.structType < structTypes.size()))
            && "eacp: a nested struct is added before the struct holding it");

        if (member.type == ValueType::Struct
            && !structTypes[member.structType].hostShareable)
            declared.hostShareable = false;
    }

    addStructFields(declared, declared, structTypes.size(), {}, 0);
    structTypes.add(declared);
    return structTypes.size() - 1;
}

const StructType& ShaderGraph::structType(int id) const
{
    return structTypes[id];
}

int ShaderGraph::structCount() const
{
    return structTypes.size();
}

bool ShaderGraph::structHoldsHalf(int structId) const
{
    for (const auto& field: structTypes[structId].fields)
        if (isHalf(field.type))
            return true;

    return false;
}

bool ShaderGraph::fitsField(int structId, int field, int value) const
{
    const auto& node = nodes[value];

    if (field < 0)
        return node.type == ValueType::Struct && node.structType == structId;

    const auto& target = structTypes[structId].fields[field];

    if (target.type == ValueType::Struct)
        return node.type == ValueType::Struct
               && node.structType == target.structType;

    return node.type == target.type;
}

int ShaderGraph::addStructConstruct(int structId, const Vector<int>& members)
{
    const auto& type = structTypes[structId];
    assert(members.size() == type.members.size()
           && "eacp: a struct is built from every one of its members");

    for (auto which = 0; which < members.size(); ++which)
    {
        const auto& member = type.members[which];
        const auto& node = nodes[members[which]];

        assert(node.type == member.type
               && (member.type != ValueType::Struct
                   || node.structType == member.structType)
               && "eacp: a struct member takes a value of its own type");
        (void) member;
        (void) node;
    }

    auto node = Expr {};
    node.kind = ExprKind::Construct;
    node.type = ValueType::Struct;
    node.structType = structId;
    node.args = members;
    return add(std::move(node));
}

int ShaderGraph::addStructZero(int structId)
{
    auto members = Vector<int> {};

    for (const auto& member: structTypes[structId].members)
        members.add(member.type == ValueType::Struct
                        ? addStructZero(member.structType)
                        : addZero(member.type));

    return addStructConstruct(structId, members);
}

int ShaderGraph::addStructVariable(int structId, int initialValue)
{
    assert(fitsField(structId, -1, initialValue)
           && "eacp: a struct variable starts as a value of its struct");

    auto slot = addVariableSlot(ValueType::Struct);
    variableStructs[slot] = structId;

    auto declaration = Statement {StatementKind::Declare};
    declaration.slot = slot;
    declaration.value = initialValue;
    addStatement(declaration);

    return slot;
}

int ShaderGraph::addStructArrayVariable(int structId, int size, int fill)
{
    assert(size > 0 && "eacp: a private array has at least one element");
    assert(fitsField(structId, -1, fill)
           && "eacp: a private array is filled with a value of its element type");

    auto slot = addVariableSlot(ValueType::Struct);
    variableSizes[slot] = size;
    variableStructs[slot] = structId;

    auto declaration = Statement {StatementKind::Declare};
    declaration.slot = slot;
    declaration.value = fill;
    addStatement(declaration);

    return slot;
}

int ShaderGraph::variableStructType(int slot) const
{
    return slot >= 0 && slot < variableStructs.size() ? variableStructs[slot] : -1;
}

int ShaderGraph::addOverride(const std::string& name, ValueType type)
{
    auto declaration = OverrideDeclaration {};
    declaration.name = name;
    declaration.type = type;
    return declareOverride(declaration);
}

int ShaderGraph::addOverride(const std::string& name,
                             ValueType type,
                             std::uint32_t defaultBits)
{
    auto declaration = OverrideDeclaration {};
    declaration.name = name;
    declaration.type = type;
    declaration.hasDefault = true;
    declaration.defaultBits = defaultBits;
    return declareOverride(declaration);
}

int ShaderGraph::findOverride(std::string_view name) const
{
    for (auto id = 0; id < overrideList.size(); ++id)
        if (overrideList[id].name == name)
            return id;

    return -1;
}

int ShaderGraph::declareOverride(const OverrideDeclaration& declaration)
{
    assert(isOverridableType(declaration.type)
           && "eacp: an override is a Bool, Int, UInt, Float or Half scalar");
    assert(isIdentifier(declaration.name)
           && "eacp: an override's name is an identifier");

    const auto existing = findOverride(declaration.name);

    if (existing >= 0)
    {
        const auto& declared = overrideList[existing];

        assert(declared.type == declaration.type
               && declared.hasDefault == declaration.hasDefault
               && declared.defaultBits == declaration.defaultBits
               && "eacp: an override declared twice differs from itself");

        return declared.node;
    }

    auto node = Expr {};
    node.kind = ExprKind::Override;
    node.type = declaration.type;
    node.index = overrideList.size();

    auto declared = declaration;
    declared.node = add(std::move(node));
    overrideList.add(declared);
    return declared.node;
}

int ShaderGraph::addVarMemberRead(int slot, int field)
{
    assert(variableArraySize(slot) == 0
           && "eacp: a private array is read an element at a time");

    auto node = Expr {};
    node.kind = ExprKind::VarRead;
    node.type = memberLeafType(variableStructType(slot), field);
    node.index = slot;
    node.member = field;
    return add(std::move(node));
}

int ShaderGraph::addVarElementMemberRead(int slot, int index, int field)
{
    assert(variableArraySize(slot) > 0 && "eacp: only a private array is indexed");

    auto node = Expr {};
    node.kind = ExprKind::VarRead;
    node.type = memberLeafType(variableStructType(slot), field);
    node.index = slot;
    node.member = field;
    node.args.add(index);
    return add(std::move(node));
}

int ShaderGraph::clampVariableIndex(int slot, int index)
{
    assert(variableArraySize(slot) > 0 && "eacp: only a private array is indexed");
    return clampedIndex(index, variableSizes[slot]);
}

void ShaderGraph::assignMember(int slot, int field, int value)
{
    assert(variableArraySize(slot) == 0
           && "eacp: a private array is written an element at a time");
    assert(isVertexVariable(slot) == recordingVertexStage
           && "eacp: a variable is assigned in the stage that declared it - a "
              "vertexStage variable inside vertexStage, any other outside it.");
    assert(variableStructType(slot) >= 0
           && fitsField(variableStructType(slot), field, value)
           && "eacp: a struct field takes a value of its own type");

    auto assignment = Statement {StatementKind::Assign};
    assignment.slot = slot;
    assignment.member = field;
    assignment.value = value;
    addStatement(assignment);
}

void ShaderGraph::assignElementMember(int slot, int index, int field, int value)
{
    assert(variableArraySize(slot) > 0 && "eacp: only a private array is indexed");
    assert(isVertexVariable(slot) == recordingVertexStage
           && "eacp: a variable is assigned in the stage that declared it - a "
              "vertexStage variable inside vertexStage, any other outside it.");
    assert(variableStructType(slot) >= 0
           && fitsField(variableStructType(slot), field, value)
           && "eacp: a struct field takes a value of its own type");

    auto assignment = Statement {StatementKind::Assign};
    assignment.slot = slot;
    assignment.index = index;
    assignment.member = field;
    assignment.value = value;
    addStatement(assignment);
}

int ShaderGraph::addStructBuffer(BufferAccess access, int structId)
{
    assert(access != BufferAccess::Atomic
           && "eacp: a struct buffer is read-only or read-write, never atomic");
    assert(structTypes[structId].hostShareable
           && "eacp: a Bool has no storage layout, so a struct holding one is "
              "never a buffer's element");

    auto slot = addStorageBuffer(access, ValueType::Struct);
    storageStructs[slot] = structId;
    return slot;
}

int ShaderGraph::storageStructType(int slot) const
{
    return slot >= 0 && slot < storageStructs.size() ? storageStructs[slot] : -1;
}

int ShaderGraph::addBufferMemberRead(int slot, int index, int field)
{
    auto node = Expr {};
    node.kind = ExprKind::BufferRead;
    node.type = memberLeafType(storageStructType(slot), field);
    node.index = slot;
    node.member = field;
    node.args.add(index);
    return add(std::move(node));
}

void ShaderGraph::addMemberStore(int slot, int index, int field, int value)
{
    assert(storageSlots[slot] == BufferAccess::Write
           && "eacp: only a StructOutputBuffer is written");
    assert(storageStructType(slot) >= 0
           && fitsField(storageStructType(slot), field, value)
           && "eacp: a struct field takes a value of its own type");

    storeList.add({slot, index, value});

    auto statement = Statement {StatementKind::Store};
    statement.slot = slot;
    statement.index = index;
    statement.value = value;
    statement.member = field;
    addStatement(statement);
}

// What reading a field yields: a field holding a nested struct is read
// through its own fields.
ValueType ShaderGraph::memberLeafType(int structId, int field) const
{
    assert(structId >= 0 && field >= 0 && field < structTypes[structId].fields.size()
           && "eacp: a struct is read a field at a time");

    auto type = structTypes[structId].fields[field].type;
    assert(type != ValueType::Struct
           && "eacp: a nested struct is read through its own fields");
    return type;
}

int ShaderGraph::beginFunction(const std::string& name,
                               ValueType returnType,
                               int returnStruct,
                               bool returnsValue)
{
    assert(recordingFunction < 0
           && "eacp: a function is declared in define(), not in another "
              "function's body.");
    assert(isIdentifier(name) && "eacp: a function's name is an identifier");

    for ([[maybe_unused]] const auto& declared: functionList)
        assert(declared.name != name
               && "eacp: two functions of one shader have two names.");

    auto declaration = FunctionDeclaration {};
    declaration.name = name;
    declaration.returnType = returnsValue ? returnType : ValueType::Bool;
    declaration.returnStruct = returnStruct;
    declaration.returnsValue = returnsValue;

    blocks.add(Block {});
    declaration.body = blocks.size() - 1;
    functionList.add(declaration);

    blocksOpenOutsideFunction = openBlocks;
    vertexStageOutsideFunction = recordingVertexStage;
    openBlocks = Vector<int> {declaration.body};
    recordingVertexStage = false;
    recordingFunction = functionList.size() - 1;
    return recordingFunction;
}

int ShaderGraph::addParameter(ValueType type, int structType)
{
    assert(recordingFunction >= 0
           && "eacp: a parameter belongs to the function being declared");

    auto slot = addVariableSlot(type);
    variableStructs[slot] = structType;
    functionList[recordingFunction].parameters.add(slot);
    return slot;
}

void ShaderGraph::endFunction(int result)
{
    assert(recordingFunction >= 0 && "eacp: endFunction without beginFunction");

    auto id = recordingFunction;

    if (functionList[id].returnsValue)
        addReturn(result);

    auto declaration = functionList[id];
    checkFunctionBody(declaration, id);
    declaration.isComplete = true;
    functionList[id] = declaration;

    openBlocks = blocksOpenOutsideFunction;
    recordingVertexStage = vertexStageOutsideFunction;
    recordingFunction = -1;
}

void ShaderGraph::checkFunctionBody(FunctionDeclaration& declaration, int id) const
{
    auto seen = Vector<char> {};
    seen.resize(nodes.size(), 0);

    declaration.isPure = true;
    checkFunctionBlock(declaration, id, declaration.body, seen);

    for (auto callee: declaration.callees)
    {
        const auto& called = functionList[callee];
        declaration.writesStorage =
            declaration.writesStorage || called.writesStorage;
        declaration.readsMutableStorage =
            declaration.readsMutableStorage || called.readsMutableStorage;
        declaration.discards = declaration.discards || called.discards;
    }

    declaration.isPure = declaration.isPure && !declaration.writesStorage
                         && !declaration.readsMutableStorage
                         && !declaration.discards;
}

void ShaderGraph::checkFunctionBlock(FunctionDeclaration& declaration,
                                     int id,
                                     int block,
                                     Vector<char>& seen) const
{
    for (auto index: blocks[block].statements)
    {
        const auto& statement = statementList[index];

        assert(!isStageScopedStatement(statement)
               && "eacp: a function's body has no shared memory, barrier, "
                  "group fold or SIMD-group matrix - a kernel's define() does.");

        assert((!writesVariableSlot(statement.kind)
                || variableFunctions[statement.slot] == id)
               && (statement.exchanged < 0
                   || variableFunctions[statement.exchanged] == id)
               && "eacp: a function writes only its own variables - not the "
                  "caller's, and not its parameters.");

        assert((statement.kind != StatementKind::Assign
                || !declaration.parameters.contains(statement.slot))
               && "eacp: a function's parameters are read, never assigned - "
                  "copy one into a var() to change it.");

        declaration.writesStorage =
            declaration.writesStorage || isStorageWrite(statement.kind);
        declaration.discards =
            declaration.discards || statement.kind == StatementKind::Discard;

        for (auto node: {statement.value,
                         statement.index,
                         statement.indexY,
                         statement.layer,
                         statement.stride,
                         statement.compare,
                         statement.record})
            checkFunctionNode(declaration, id, node, seen);

        for (auto body: {statement.body, statement.elseBody, statement.continuing})
            if (body >= 0)
                checkFunctionBlock(declaration, id, body, seen);
    }
}

void ShaderGraph::checkFunctionNode(FunctionDeclaration& declaration,
                                    int id,
                                    int node,
                                    Vector<char>& seen) const
{
    if (node < 0 || seen[node] != 0)
        return;

    seen[node] = 1;
    const auto& expr = nodes[node];

    assert(!isStageScoped(expr.kind)
           && "eacp: a function's body reads no built-in, vertex input, varying, "
              "thread index, shared memory or constant array: pass the value "
              "in as an argument.");

    assert((expr.kind != ExprKind::VarRead || variableFunctions[expr.index] == id)
           && "eacp: a function reads its parameters and its own variables, "
              "not the caller's - pass the value in as an argument.");

    auto isOwnRead = expr.kind == ExprKind::VarRead;
    auto isMutableRead = (expr.kind == ExprKind::BufferRead
                          || expr.kind == ExprKind::BufferVectorRead)
                         && !readsImmutableStorage(expr);
    auto isWritableTexture =
        (expr.kind == ExprKind::TextureFunction || expr.kind == ExprKind::Fetch
         || expr.kind == ExprKind::Sample)
        && (textureAccesses[expr.index] == TextureAccess::Write
            || textureAccesses[expr.index] == TextureAccess::ReadWrite);

    declaration.readsMutableStorage = declaration.readsMutableStorage
                                      || isMutableRead || isWritableTexture
                                      || expr.kind == ExprKind::AtomicLoad;

    if (dependsOnMutableState(expr.kind) && !isOwnRead
        && !readsImmutableStorage(expr))
        declaration.isPure = false;

    if (expr.kind == ExprKind::FunctionCall)
    {
        if (!declaration.callees.contains(expr.index))
            declaration.callees.add(expr.index);

        declaration.isPure = declaration.isPure && functionList[expr.index].isPure;
    }

    for (auto argument: expr.args)
        checkFunctionNode(declaration, id, argument, seen);
}

ShaderGraph::FunctionResult
    ShaderGraph::addFunctionCall(int function, const Vector<int>& arguments)
{
    assert(function >= 0 && function < functionList.size()
           && functionList[function].isComplete
           && "eacp: a function is called once it is declared, and not from its "
              "own body - WGSL has no recursion.");

    const auto& declaration = functionList[function];

    assert(arguments.size() == declaration.parameters.size()
           && "eacp: a call passes one argument per parameter");

    for (auto which = 0; which < arguments.size(); ++which)
    {
        [[maybe_unused]] auto parameter = declaration.parameters[which];
        [[maybe_unused]] const auto& argument = nodes[arguments[which]];
        assert(argument.type == variableTypes[parameter]
               && argument.structType == variableStructs[parameter]
               && "eacp: an argument is a value of its parameter's type");
    }

    assert((!recordingVertexStage
            || (!declaration.writesStorage && !declaration.discards))
           && "eacp: the vertex stage calls no function that writes storage, "
              "runs an atomic or discards.");

    auto call = Expr {};
    call.kind = ExprKind::FunctionCall;
    call.type = declaration.returnType;
    call.structType = declaration.returnsValue ? declaration.returnStruct : -1;
    call.index = function;
    call.args = arguments;

    if (!isCalledAsStatement(function))
        return {add(std::move(call)), -1};

    auto variable = -1;
    auto returnsStruct = declaration.returnType == ValueType::Struct;

    if (declaration.returnsValue)
    {
        variable = addVariableSlot(declaration.returnType);
        variableStructs[variable] = declaration.returnStruct;
    }

    auto statement = Statement {StatementKind::Call};
    statement.value = add(std::move(call));
    statement.slot = variable;
    addStatement(statement);

    if (variable < 0 || returnsStruct)
        return {-1, variable};

    return {addVarRead(variable), variable};
}

bool ShaderGraph::isCalledAsStatement(int id) const
{
    const auto& declaration = functionList[id];

    return !declaration.returnsValue || declaration.returnType == ValueType::Struct
           || declaration.writesStorage || declaration.readsMutableStorage
           || declaration.discards;
}

const FunctionDeclaration& ShaderGraph::function(int id) const
{
    return functionList[id];
}

int ShaderGraph::functionCount() const
{
    return functionList.size();
}

int ShaderGraph::variableFunction(int slot) const
{
    return slot >= 0 && slot < variableFunctions.size() ? variableFunctions[slot]
                                                        : -1;
}
} // namespace eacp::GPU
