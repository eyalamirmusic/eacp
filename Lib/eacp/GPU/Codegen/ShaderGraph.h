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
    Call, // builtin call text(args...); e.g. sin/cos. The emitter translates
    // the canonical (MSL) name where HLSL spells it differently.
    Unary, // (op child); args = {child}; op. Negation, logical and bitwise not.
    Binary, // (lhs op rhs); args = {lhs, rhs}. The operator is `op` when it fits
    // in a char and `text` when it does not - which is only the two shifts.
    Compare, // (lhs op rhs) yielding a Bool; args = {lhs, rhs}, op text in `text`.
    // Separate from Binary for two reasons: <=, == and && do not fit in a char,
    // and the result is a Bool whatever shape the operands are.
    Select, // (condition ? whenTrue : whenFalse); args = {condition, a, b}. Both
    // languages spell the conditional operator the same way.
    VarRead, // the current value of a mutable local; index = variable slot.
    // Unlike every other node this is not a pure expression: what it evaluates
    // to depends on which statements have run, so the emitter never hoists one
    // past an assignment (see the per-statement local planning in the emitter).
    Mul, // a matrix product: args = {left, right} in the order written, which
    // is matrix * vector, vector * matrix or matrix * matrix. Emits per-backend
    // (MSL uses the * operator, HLSL uses mul()), so it is not a plain Binary -
    // and both languages read whichever operand is on the left the same way, so
    // the order is the whole of what distinguishes the three.
    Sample, // texture sample; index = texture slot, args = {uv} or {uv, level}.
    // Emits per-backend (MSL t.sample(s, uv), HLSL t.Sample(s, uv)).
    Fetch, // texel read at integer coordinates, no sampler; index = texture slot,
    // args = {coordinates}. Emits per-backend (MSL t.read(), HLSL t.Load()).
    ThreadId, // compute work-item id; emitted as the kernel's gid parameter.
    // index is the component: a 1D kernel has only 0 and prints the whole gid,
    // a 2D one prints gid.x or gid.y.
    BufferRead, // storage-buffer element read; index = buffer slot, args = {index}
    AtomicLoad, // one element of an atomic buffer; index = buffer slot,
    // args = {index}. An expression on both backends, unlike the add - MSL
    // spells it atomic_load_explicit and HLSL is an ordinary subscript, since
    // there a UAV element is already what an interlocked op works on.
    ArrayRead, // constant-array element read; index = array slot, args = {index}
    LocalId, // position within the threadgroup; index = component, like ThreadId
    GroupId, // the threadgroup's own index in the grid; index = component
    GridExtent, // the implicit bounds uniform the generated guard reads: count
    // for a 1D kernel, width/height by component for a 2D one. Exposed so a
    // kernel that barriers - and therefore has no early-return guard - can
    // bound its stores against the very same value the dispatch supplied.
    SharedRead // threadgroup-array element read; index = slot, args = {index}
};

// How a kernel accesses a storage buffer: a read-only input (Metal device
// const / D3D SRV), a writable output (Metal device / D3D UAV), or an atomic
// one - unsigned integer elements every thread may read-modify-write at once.
//
// Atomic is a separate access rather than a flag on Write because it changes
// the element type: MSL needs device atomic_uint* and HLSL an
// RWStructuredBuffer<uint>, neither of which is the run of floats the other two
// are. The bits in one are integers, so a buffer written atomically by one
// kernel and read as floats by the next reads garbage; load it atomically, or
// have the kernel that fills it convert.
enum class BufferAccess
{
    Read,
    Write,
    Atomic
};

// How a shader accesses a texture: sampled and fetched (Metal
// texture2d<float>, D3D Texture2D through an SRV) or written by a kernel
// (Metal access::write, D3D RWTexture2D through a UAV). Both kinds take slots
// from one counter, because Metal binds them to one texture index space.
enum class TextureAccess
{
    Sample,
    Write
};

// What shape a texture slot is: a 2D image sampled with a float2, or six square
// faces sampled with a float3 direction. It rides beside TextureAccess rather
// than inside it because the two answer different questions - one is how the
// shader reaches the texture, the other is what the texture is - and only the
// declaration the emitter prints depends on this one.
//
// The sample itself does not. `t.sample(s, uv)` on Metal and `t.Sample(s, uv)`
// on HLSL are how both kinds are read, with the coordinate's own width deciding
// which; so the emitter's Sample case is untouched by cube textures, and the
// only place a kind is read is where the parameter or the global is declared.
enum class TextureKind
{
    Texture2D,
    Cube
};

// The shape of the grid a kernel is dispatched over, decided by which thread
// index its body asked for: threadId() gives one index over a flat count,
// threadPosition() gives a pair over a width and a height. The emitter takes
// the entry signature and the bounds guard from this, and the dispatch takes
// the grid from the matching ComputePass::dispatch overload - which is why a
// kernel cannot ask for both.
enum class DispatchRank
{
    OneD,
    TwoD
};

// What a statement does. Statements are what the expression store on its own
// cannot say: that one value is computed before another, that a value changes,
// and that a run of them repeats or is skipped. Both shading languages spell
// all six identically, so unlike the expression kinds none of these needs a
// per-backend form.
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
    Barrier, // threadgroup barrier: every thread in the group arrives before
    // any proceeds, and threadgroup memory written before it is visible after
    AtomicAdd // vN = atomicAdd(buffer[index], value), declaring vN. slot = the
    // variable the value *before* the add lands in, bufferSlot / index = which
    // element, value = what is added.
    //
    // A statement rather than an expression, and it is the one place the two
    // languages force that: MSL's atomic_fetch_add_explicit returns the old
    // value, but HLSL's InterlockedAdd writes it through an out parameter and
    // cannot appear in the middle of one. Naming the result is the only shape
    // both can print, and it is the shape a caller wants anyway - the old value
    // is a slot reserved for this thread, which is what the whole operation is
    // usually for.
};

// One statement. Which fields carry meaning depends on the kind above; the
// bodies are block indices so a statement stays plain data of a fixed size and
// the graph owns every block, exactly as it owns every expression node.
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

// A run of statements, held by index so a nested body is an int on the
// statement that owns it.
struct Block
{
    Vector<int> statements; // indices into the graph's statement store
};

// A constant array the shader subscripts: the palette a procedural shader picks
// a colour out of, the offsets a sampling kernel walks. It lives beside the
// expression store rather than in it because an array is a declaration and not
// a value - the one thing a shader names that no single node stands for. Its
// elements are ordinary expressions, evaluated once where the array is declared
// at the top of the shader body, so they may read uniforms and varyings but not
// a mutable local, which does not exist yet at that point.
struct ArrayConstant
{
    ValueType elementType = ValueType::Float;
    Vector<int> elements; // expression nodes, one per element
};

// A threadgroup-shared array: the tile a reduction or a blocked matmul stages
// in on-chip memory. Unlike a storage buffer it never crosses the CPU
// boundary, so its element type is whatever the kernel wants - a float4 tile
// is one wide element, not four scalars with a layout contract. The size is a
// compile-time constant in the emitted source, fixed when define() runs.
struct SharedArray
{
    ValueType elementType = ValueType::Float;
    int elements = 0;
};

// One node in the shader expression tree. Plain data referenced by integer id so
// value handles stay trivially copyable and the graph owns every node.
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

// Backend-agnostic shader IR: an expression-node store plus the shader's I/O
// (ordered vertex inputs, ordered varyings, the clip-space position expression
// and the fragment-output expression). The same node list drives both the
// emitted source and the vertex layout, so a shader and its layout cannot drift.
// Built by ShaderBuilder, read by the emitters; never uses runtime reflection.
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

    // One kernel output write: buffer[index] = value. Recording any store
    // marks the whole graph as a compute kernel, the way position/fragment
    // mark a render one - this list is that signature. Each store is also
    // recorded as a statement in the block open at the time, which is where
    // it is emitted: a write inside a loop body runs once per iteration,
    // not once after the loop.
    struct Store
    {
        int slot = -1;
        int index = -1;
        int value = -1;
    };

    // Its texture sibling: texture[x, y] = colour. A compute signature entry
    // exactly as a buffer store is, and what makes a kernel able to produce
    // something a later render pass samples.
    struct TextureStore
    {
        int slot = -1;
        int x = -1;
        int y = -1;
        int value = -1;
    };

    int addInput(ValueType type);

    // A per-instance input. Emitted shader source is identical to a per-vertex
    // input; the split shows up only in the emitted VertexLayout, which routes
    // instance inputs to a dedicated buffer slot with PerInstance step rate.
    // The zero-arg form auto-assigns slot 1 (the common case: one
    // per-instance buffer alongside the per-vertex buffer at slot 0). Pass an
    // explicit bufferIndex when a shader needs multiple per-instance streams
    // in distinct buffers (e.g. per-instance transform in slot 1, per-instance
    // colour in slot 2).
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

    // The same node for an operator a char cannot hold, which is the two shifts
    // and nothing else. Kept off addCompare, whose result is a Bool whatever it
    // was given: a shift is shaped like the value being shifted.
    int addBinary(ValueType type, std::string op, int lhs, int rhs);

    int addCompare(std::string op, int lhs, int rhs);

    // The componentwise form, whose result is a boolean of the operands' width
    // rather than a scalar. Both languages give `<` on two vectors exactly this,
    // so the node prints the same way the scalar one does.
    int addCompare(ValueType type, std::string op, int lhs, int rhs);
    int addSelect(ValueType type, int condition, int whenTrue, int whenFalse);
    int addMul(ValueType type, int left, int right);

    // Mutable locals and the statements that drive them. A variable is declared
    // where it is created, so the statement stream is also its scope: creating
    // one inside a loop body declares it there, and the C++ handle that names it
    // goes out of scope at the same brace.
    //
    // Statements append to the block on top of the stack. pushBlock/popBlock
    // bracket the body of an if or a loop; the block they leave behind is what
    // addIf/addLoop then names.
    int addVariable(ValueType type, int initialValue);
    int addVarRead(int slot);
    void assign(int slot, int value);

    int pushBlock();
    void popBlock();

    void addIf(int condition, int body, int elseBody);
    void addLoop(int condition, int body);
    void addBreak();
    void addContinue();

    // Registers a 2D texture slot (always a float-returning texture2d, so only
    // the slot index and how it is sampled are stored), and a sample of it at a
    // float2 coordinate - with the mip level the hardware picks from the
    // derivatives, or with one the shader chooses.
    int addTexture(TextureSampling sampling = {});
    int addSample(int textureSlot, int uv);
    int addSample(int textureSlot, int uv, int level);

    // A cube slot, from the same counter and the same sampler space as the 2D
    // one - a shader that declares both binds them at distinct indices, and
    // RenderPass::setFragmentTexture takes either at either. What it changes is
    // the declaration the emitter prints and the width of the coordinate
    // addSample is handed; nothing else here knows the difference.
    int addCubeTexture(TextureSampling sampling = {});

    // A texture slot a kernel writes rather than reads, and one such write. It
    // takes a slot from the same counter addTexture does, so a kernel that
    // reads one texture and writes another binds them at distinct indices -
    // which is what Metal's single texture index space requires.
    int addWritableTexture();
    void addTextureStore(int slot, int x, int y, int value);

    // One texel read straight out of the texture at integer coordinates: no
    // sampler, so no filtering, no addressing and no interpolation.
    int addFetch(int textureSlot, int coordinates);

    // A constant array and a subscript of one. Reading past the end is
    // undefined in both shading languages exactly as it is in GLSL, so an index
    // a shader has not already bounded is worth masking or clamping.
    int addArray(ValueType elementType, Vector<int> elements);
    int addArrayRead(int slot, int index);

    // Compute kernel pieces: the 1D work-item id, one component of the 2D one,
    // a storage-buffer slot (float elements; inputs and outputs share one slot
    // space, so every buffer gets a distinct index), an element read, and an
    // element write. The first thread index a kernel asks for fixes its
    // dispatch rank, and asking for the other one afterwards is a contradiction
    // the emitted kernel could not express.
    int addThreadId();
    int addThreadPosition(int component);
    int addStorageBuffer(BufferAccess access);
    int addBufferRead(int slot, int index);
    void addStore(int slot, int index, int value);

    // The atomic pair. addAtomicAdd returns the *variable* slot holding the
    // element's value from before the add, which addVarRead then reads - it is a
    // statement, so unlike every other producer here it does not yield a node.
    int addAtomicAdd(int bufferSlot, int index, int value);
    int addAtomicLoad(int bufferSlot, int index);

    // The threadgroup pieces: where a thread sits inside its group and which
    // group it belongs to (components on the terms ThreadId sets - a 1D kernel
    // has only component 0), the implicit grid bound as a readable value, a
    // shared array with its subscript read and write, and the barrier that
    // orders them. Each id kind fixes the dispatch rank exactly as the global
    // ids do, so a kernel cannot mix a flat local id with a grid dispatch.
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

    // The alpha test: a third fragment-stage root, evaluated before the colour
    // is written. When the node's value falls below the threshold the fragment
    // is killed outright, writing neither colour nor depth.
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

    // How texture `slot` is to be sampled, as its shader declared it.
    TextureSampling textureSampling(int slot) const
    {
        return slot >= 0 && slot < textureSamplings.size() ? textureSamplings[slot]
                                                           : TextureSampling {};
    }

    // Whether the shader reads texture `slot` or writes it, which is what
    // decides the declaration each backend emits for it.
    TextureAccess textureAccess(int slot) const
    {
        return slot >= 0 && slot < textureAccesses.size() ? textureAccesses[slot]
                                                          : TextureAccess::Sample;
    }

    // Whether texture `slot` is a 2D image or a cube - the other half of that
    // declaration, and the only other thing the emitter needs to print it.
    TextureKind textureKind(int slot) const
    {
        return slot >= 0 && slot < textureKinds.size() ? textureKinds[slot]
                                                       : TextureKind::Texture2D;
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

    // Which threadgroup pieces the kernel asked for, driving what the emitters
    // add to the entry signature - and, for the barrier, what they take away:
    // a kernel that barriers gets no early-return bounds guard, because a
    // barrier below a return some threads took is undefined on both backends.
    // Such a kernel bounds its own stores, typically against gridExtent.
    bool usesLocalId() const { return localIdUsed; }
    bool usesGroupId() const { return groupIdUsed; }
    bool usesBarrier() const { return barrierUsed; }

    // Recording any store - to a buffer, to a texture, or an atomic add - is
    // what marks the graph as a kernel.
    //
    // The atomic case is easy to leave out and impossible to miss afterwards: a
    // kernel that only counts things writes nothing, so a graph judged by its
    // stores alone would emit a vertex/fragment pair for it and fail to compile
    // on a `gid` no render stage has.
    bool isCompute() const
    {
        return storeList.size() > 0 || textureStoreList.size() > 0 || atomicUsed;
    }

    DispatchRank dispatchRank() const { return rank; }

    // The body every recorded statement ends up in, directly or inside a nested
    // block. It runs before the fragment (or the kernel's stores) is evaluated,
    // which is what makes a mutable local visible to the expression that reads
    // it afterwards.
    static constexpr int rootBlock = 0;

    const Statement& statement(int index) const { return statementList[index]; }
    const Block& block(int index) const { return blocks[index]; }
    const Vector<ValueType>& variables() const { return variableTypes; }
    bool hasStatements() const { return !blocks[rootBlock].statements.empty(); }

private:
    int add(Expr node);
    int addStatement(Statement newStatement);
    int addIndexNode(ExprKind kind, DispatchRank forRank, int component);

    // Structural sharing for the two kinds that can take it. A key holds
    // everything add() would have to compare to call two nodes the same value;
    // a binary's operands are node ids, which is enough because the nodes they
    // name were themselves shared on the way in.
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
    Vector<TextureKind> textureKinds; // parallel to textureSamplings
    Vector<ArrayConstant> arrayConstants;
    Vector<SharedArray> sharedArrayList;
    bool localIdUsed = false;
    bool groupIdUsed = false;
    bool barrierUsed = false;

    // Whether anything atomic was recorded. A kernel whose only output is a
    // counter has no store to be recognised by, so this is what tells the
    // emitter it is one - see isCompute.
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
