#pragma once

#include "../Common.h"

#include "ShaderTypes.h"

#include "../Pipeline/VertexLayout.h"

#include <cstdint>
#include <map>
#include <string>
#include <tuple>

namespace eacp::GPU
{
enum class ExprKind
{
    Input, // vertex attribute; index = attribute slot
    Varying, // fragment-stage read of a varying; index = varying slot
    Uniform, // per-frame constant; index = field slot in the uniform block
    Constant, // scalar float literal; value
    Construct, // floatN(args...); args = child nodes
    Swizzle, // child.<components>; args[0] = child
    Call, // builtin call text(args...), named as MSL spells it
    Unary, // (op child); args = {child}; op
    Binary, // (lhs op rhs); args = {lhs, rhs}; op, or text for the two shifts
    Compare, // (lhs op rhs) yielding a Bool; args = {lhs, rhs}, op in text
    Select, // (condition ? whenTrue : whenFalse); args = {condition, a, b}
    VarRead, // mutable local; index = variable slot. Impure: never hoisted past
    // an assignment
    Mul, // matrix product; args = {left, right} in the order written
    Sample, // texture sample; index = texture slot, args = {uv} or {uv, level}
    Fetch, // sampler-less texel read; index = texture slot, args = {coordinates}
    ThreadId, // compute work-item id; index = component
    BufferRead, // storage-buffer element read; index = buffer slot, args = {index}
    AtomicLoad, // atomic buffer element; index = buffer slot, args = {index}
    ArrayRead, // constant-array element read; index = array slot, args = {index}
    LocalId, // position within the threadgroup; index = component
    GroupId, // the threadgroup's own index in the grid; index = component
    GridExtent, // the implicit bounds uniform: count for 1D, width/height for 2D
    SharedRead // threadgroup-array element read; index = slot, args = {index}
};

// How a kernel accesses a storage buffer. Atomic changes the element type to
// unsigned integer, so its bits read as garbage through a float Read.
enum class BufferAccess
{
    Read,
    Write,
    Atomic
};

// How a shader accesses a texture. Both kinds take slots from one counter,
// because Metal binds them to one texture index space.
enum class TextureAccess
{
    Sample,
    Write
};

// The shape of the grid a kernel is dispatched over, fixed by which thread
// index its body asked for; a kernel cannot ask for both.
enum class DispatchRank
{
    OneD,
    TwoD
};

enum class StatementKind
{
    Declare, // <type> vN = value; slot = variable, value = its initial value
    Assign, // vN = value; slot = variable, value = the expression assigned
    If, // if (value) { body } else { elseBody }
    Loop, // while (value) { body }
    Break,
    Continue,
    Store, // buffer[index] = value; slot = the storage slot
    TextureStore, // texture[index, indexY] = value; slot = the texture slot
    SharedStore, // shared[index] = value; slot = the threadgroup-array slot
    Barrier, // threadgroup barrier
    AtomicAdd // vN = atomicAdd(buffer[index], value); slot = the variable the
    // pre-add value lands in. A statement, not an expression, because HLSL's
    // InterlockedAdd returns the old value through an out parameter
};

// Which fields carry meaning depends on the kind above.
struct Statement
{
    StatementKind kind = StatementKind::Assign;
    int slot = -1; // Declare / Assign: the variable written; stores: the slot
    int value = -1; // Declare / Assign / stores: the value; If / Loop: the condition
    int body = -1; // If / Loop: the block that runs
    int elseBody = -1; // If: the block that runs when the condition is false
    int index = -1; // Store: the element index; TextureStore: x; AtomicAdd: the
    // element
    int indexY = -1; // TextureStore: y
    int bufferSlot = -1; // AtomicAdd: the buffer, its slot field being taken by
    // the variable the old value lands in
};

struct Block
{
    Vector<int> statements; // indices into the graph's statement store
};

// A constant array the shader subscripts. Its elements are evaluated once where
// the array is declared at the top of the body, so they may read uniforms and
// varyings but not a mutable local, which does not exist yet at that point.
struct ArrayConstant
{
    ValueType elementType = ValueType::Float;
    Vector<int> elements; // expression nodes, one per element
};

// A threadgroup-shared array. It never crosses the CPU boundary, so it carries
// no layout contract; its size is a compile-time constant fixed when define()
// runs.
struct SharedArray
{
    ValueType elementType = ValueType::Float;
    int elements = 0;
};

// Plain data referenced by integer id, so value handles stay trivially
// copyable and the graph owns every node.
struct Expr
{
    ExprKind kind = ExprKind::Constant;
    ValueType type = ValueType::Float;
    int index = 0; // Input / Varying / Uniform slot; value of a UInt Constant
    float value = 0.0f; // Float Constant
    char op = '+'; // Binary
    std::string text; // Swizzle components ("xy") or Call name ("sin")
    Vector<int> args; // child node ids
};

// Backend-agnostic shader IR. Built by ShaderBuilder, read by the emitters; the
// same node list drives the emitted source and the vertex layout.
class ShaderGraph
{
public:
    ShaderGraph()
    {
        blocks.add(Block {});
        openBlocks.add(rootBlock);
    }

    struct VaryingSlot
    {
        ValueType type = ValueType::Float;
        int sourceNode = -1; // vertex-stage expression feeding this varying
    };

    // One kernel output write: buffer[index] = value. Also recorded as a
    // statement in the block open at the time, which is where it is emitted.
    struct Store
    {
        int slot = -1;
        int index = -1;
        int value = -1;
    };

    // Its texture sibling: texture[x, y] = colour.
    struct TextureStore
    {
        int slot = -1;
        int x = -1;
        int y = -1;
        int value = -1;
    };

    int addInput(ValueType type);

    // A per-instance input. The split shows up only in the emitted
    // VertexLayout; the zero-arg form auto-assigns buffer slot 1.
    int addInstanceInput(ValueType type);
    int addInstanceInput(ValueType type, int bufferIndex);
    int addVarying(ValueType type, int sourceNode);
    int addUniform(ValueType type);
    int addConstant(float value);
    int addUIntConstant(unsigned value);
    int addIntConstant(int value);
    int addBoolConstant(bool value);
    int addConstruct(ValueType type, Vector<int> args);
    int addSwizzle(ValueType type, int child, std::string components);
    int addCall(ValueType type, std::string name, int argument);
    int addCall(ValueType type, std::string name, Vector<int> args);
    int addUnary(ValueType type, char op, int child);
    int addBinary(ValueType type, char op, int lhs, int rhs);

    // For an operator a char cannot hold, which is the two shifts and nothing
    // else.
    int addBinary(ValueType type, std::string op, int lhs, int rhs);

    int addCompare(std::string op, int lhs, int rhs);

    // The componentwise form, yielding a boolean of the operands' width.
    int addCompare(ValueType type, std::string op, int lhs, int rhs);
    int addSelect(ValueType type, int condition, int whenTrue, int whenFalse);
    int addMul(ValueType type, int left, int right);

    // Statements append to the block on top of the stack, which is also a
    // variable's scope. pushBlock/popBlock bracket the body of an if or a loop;
    // the block they leave behind is what addIf/addLoop then names.
    int addVariable(ValueType type, int initialValue);
    int addVarRead(int slot);
    void assign(int slot, int value);

    int pushBlock();
    void popBlock();

    void addIf(int condition, int body, int elseBody);
    void addLoop(int condition, int body);
    void addBreak();
    void addContinue();

    // A 2D texture slot, always float-returning, and a sample of it at a float2
    // coordinate - with the hardware-derived mip level, or a chosen one.
    int addTexture(TextureSampling sampling = {});
    int addSample(int textureSlot, int uv);
    int addSample(int textureSlot, int uv, int level);

    // A texture slot a kernel writes rather than reads. It takes a slot from
    // the same counter addTexture does, as Metal has one texture index space.
    int addWritableTexture();
    void addTextureStore(int slot, int x, int y, int value);

    // One texel at integer coordinates: no sampler, so no filtering, no
    // addressing and no interpolation.
    int addFetch(int textureSlot, int coordinates);

    // A constant array and a subscript of one. Reading past the end is
    // undefined in both shading languages, so bound the index yourself.
    int addArray(ValueType elementType, Vector<int> elements);
    int addArrayRead(int slot, int index);

    // Compute kernel pieces. Inputs and outputs share one storage-slot space,
    // and the first thread index a kernel asks for fixes its dispatch rank.
    int addThreadId();
    int addThreadPosition(int component);
    int addStorageBuffer(BufferAccess access);
    int addBufferRead(int slot, int index);
    void addStore(int slot, int index, int value);

    // addAtomicAdd returns a *variable* slot holding the pre-add value, for
    // addVarRead - being a statement, it does not yield a node.
    int addAtomicAdd(int bufferSlot, int index, int value);
    int addAtomicLoad(int bufferSlot, int index);

    // The threadgroup pieces. Each id kind fixes the dispatch rank exactly as
    // the global ids do, so a flat local id cannot mix with a grid dispatch.
    int addLocalId();
    int addLocalPosition(int component);
    int addGroupId();
    int addGroupPosition(int component);
    int addGridExtent(DispatchRank forRank, int component);
    int addSharedArray(ValueType elementType, int elements);
    int addSharedRead(int slot, int index);
    void addSharedStore(int slot, int index, int value);
    void addBarrier();

    void setPosition(int node) { positionNode = node; }
    void setFragment(int node) { fragmentNode = node; }

    // The alpha test: below the threshold the fragment is killed outright,
    // writing neither colour nor depth.
    void setDiscard(int node, float threshold)
    {
        discardNode = node;
        discardValue = threshold;
    }

    const Expr& expr(int node) const { return nodes[node]; }
    int nodeCount() const { return nodes.size(); }
    const Vector<ValueType>& inputs() const { return inputTypes; }
    const Vector<StepRate>& inputStepRates() const { return inputRates; }
    const Vector<int>& inputBufferIndices() const { return inputSlots; }
    const Vector<VaryingSlot>& varyings() const { return varyingSlots; }
    const Vector<ValueType>& uniforms() const { return uniformTypes; }
    int textureCount() const { return textureSamplings.size(); }

    TextureSampling textureSampling(int slot) const
    {
        return slot >= 0 && slot < textureSamplings.size() ? textureSamplings[slot]
                                                           : TextureSampling {};
    }

    TextureAccess textureAccess(int slot) const
    {
        return slot >= 0 && slot < textureAccesses.size() ? textureAccesses[slot]
                                                          : TextureAccess::Sample;
    }

    int position() const { return positionNode; }
    int fragment() const { return fragmentNode; }
    int discard() const { return discardNode; }
    float discardThreshold() const { return discardValue; }

    const Vector<BufferAccess>& storageBuffers() const { return storageSlots; }
    const Vector<ArrayConstant>& arrays() const { return arrayConstants; }
    const Vector<Store>& stores() const { return storeList; }
    const Vector<TextureStore>& textureStores() const { return textureStoreList; }
    const Vector<SharedArray>& sharedArrays() const { return sharedArrayList; }

    // A kernel that barriers gets no early-return bounds guard - a barrier
    // below a return some threads took is undefined on both backends - so it
    // must bound its own stores, typically against gridExtent.
    bool usesLocalId() const { return localIdUsed; }
    bool usesGroupId() const { return groupIdUsed; }
    bool usesBarrier() const { return barrierUsed; }

    // Recording any store - to a buffer, to a texture, or an atomic add - is
    // what marks the graph as a kernel. A kernel that only counts things has no
    // store of its own, which is why the atomic case is in here too.
    bool isCompute() const
    {
        return storeList.size() > 0 || textureStoreList.size() > 0 || atomicUsed;
    }

    DispatchRank dispatchRank() const { return rank; }

    // The body every recorded statement ends up in, run before the fragment or
    // the kernel's stores are evaluated.
    static constexpr int rootBlock = 0;

    const Statement& statement(int index) const { return statementList[index]; }
    const Block& block(int index) const { return blocks[index]; }
    const Vector<ValueType>& variables() const { return variableTypes; }
    bool hasStatements() const { return !blocks[rootBlock].statements.empty(); }

private:
    int add(Expr node);
    int addStatement(Statement newStatement);
    int addIndexNode(ExprKind kind, DispatchRank forRank, int component);

    // Structural sharing for the two kinds that can take it: the key holds
    // everything add() compares to call two nodes the same value.
    using ConstantKey = std::tuple<ValueType, int, std::uint32_t>;
    using BinaryKey = std::tuple<ValueType, char, std::string, int, int>;

    static ConstantKey constantKeyFor(const Expr& node);
    static BinaryKey binaryKeyFor(const Expr& node);

    bool isPure(int node) const;
    bool purityOf(const Expr& node) const;
    int findShared(const Expr& node) const;

    std::map<ConstantKey, int> constantCache;
    std::map<BinaryKey, int> binaryCache;
    Vector<char> pureFlags; // parallel to nodes

    Vector<Expr> nodes;
    Vector<ValueType> inputTypes;
    Vector<StepRate> inputRates; // parallel to inputTypes
    Vector<int> inputSlots; // parallel to inputTypes; the buffer slot
    Vector<VaryingSlot> varyingSlots;
    Vector<ValueType> uniformTypes;
    Vector<BufferAccess> storageSlots;
    Vector<Store> storeList;
    Vector<TextureStore> textureStoreList;
    Vector<TextureSampling> textureSamplings;
    Vector<TextureAccess> textureAccesses; // parallel to textureSamplings
    Vector<ArrayConstant> arrayConstants;
    Vector<SharedArray> sharedArrayList;
    bool localIdUsed = false;
    bool groupIdUsed = false;
    bool barrierUsed = false;

    bool atomicUsed = false;

    Vector<ValueType> variableTypes;
    Vector<Statement> statementList;
    Vector<Block> blocks; // blocks[rootBlock] is the shader's body
    Vector<int> openBlocks; // innermost last; blocks[back()] takes new statements
    DispatchRank rank = DispatchRank::OneD;
    bool rankFixed = false;
    int positionNode = -1;
    int fragmentNode = -1;
    int discardNode = -1;
    float discardValue = 0.0f;
};
} // namespace eacp::GPU
