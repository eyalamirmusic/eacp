#pragma once

#include "../Common.h"

#include "ShaderTypes.h"

#include "../Pipeline/VertexLayout.h"

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
    Unary, // (op child); args = {child}; op. Currently only negation.
    Binary, // (lhs op rhs); args = {lhs, rhs}
    Mul, // matrix * vector; args = {matrix, vector}. Emits per-backend (MSL uses
    // the * operator, HLSL uses mul()), so it is not a plain Binary.
    Sample, // texture sample; index = texture slot, args = {uv}. Emits
    // per-backend (MSL t.sample(s, uv), HLSL t.Sample(s, uv)).
    ThreadId, // compute work-item id; emitted as the kernel's gid parameter
    BufferRead // storage-buffer element read; index = buffer slot, args = {index}
};

// How a kernel accesses a storage buffer: a read-only input (Metal device
// const / D3D SRV) or a writable output (Metal device / D3D UAV).
enum class BufferAccess
{
    Read,
    Write
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
    struct VaryingSlot
    {
        ValueType type = ValueType::Float;
        int sourceNode = -1; // vertex-stage expression feeding this varying
    };

    // One kernel output write: buffer[index] = value. Stores are the compute
    // roots, the way position/fragment are the render roots; recording any
    // store marks the whole graph as a compute kernel.
    struct Store
    {
        int slot = -1;
        int index = -1;
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
    int addConstruct(ValueType type, Vector<int> args);
    int addSwizzle(ValueType type, int child, std::string components);
    int addCall(ValueType type, std::string name, int argument);
    int addCall(ValueType type, std::string name, Vector<int> args);
    int addUnary(ValueType type, char op, int child);
    int addBinary(ValueType type, char op, int lhs, int rhs);
    int addMul(ValueType type, int matrix, int vector);

    // Registers a 2D texture slot (always a float-returning texture2d, so only
    // the slot index and how it is sampled are stored), and a sample of it at a
    // float2 coordinate.
    int addTexture(TextureSampling sampling = {});
    int addSample(int textureSlot, int uv);

    // Compute kernel pieces: the 1D work-item id, a storage-buffer slot (float
    // elements; inputs and outputs share one slot space, so every buffer gets a
    // distinct index), an element read, and an element write.
    int addThreadId();
    int addStorageBuffer(BufferAccess access);
    int addBufferRead(int slot, int index);
    void addStore(int slot, int index, int value);

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

    int position() const { return positionNode; }
    int fragment() const { return fragmentNode; }
    int discard() const { return discardNode; }
    float discardThreshold() const { return discardValue; }

    const Vector<BufferAccess>& storageBuffers() const { return storageSlots; }
    const Vector<Store>& stores() const { return storeList; }
    bool isCompute() const { return storeList.size() > 0; }

private:
    int add(Expr node);

    Vector<Expr> nodes;
    Vector<ValueType> inputTypes;
    Vector<StepRate> inputRates; // parallel to inputTypes
    Vector<int> inputSlots; // parallel to inputTypes; the buffer slot
    Vector<VaryingSlot> varyingSlots;
    Vector<ValueType> uniformTypes;
    Vector<BufferAccess> storageSlots;
    Vector<Store> storeList;
    Vector<TextureSampling> textureSamplings;
    int positionNode = -1;
    int fragmentNode = -1;
    int discardNode = -1;
    float discardValue = 0.0f;
};
} // namespace eacp::GPU
