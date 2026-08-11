#pragma once

#include "../Shader/ShaderSource.h"
#include "GeneratedShader.h"
#include "ShaderGraph.h"
#include "ShaderValue.h"

namespace eacp::GPU
{
namespace detail
{
// MSL on Apple, HLSL on Windows; defined per-platform so build() needs no
// preprocessor branch.
ShaderSource nativeShaderSource(const ShaderGraph& graph);
} // namespace detail

// The string-free authoring entry point. Vertex inputs, uniforms and varyings
// are assigned slots in call order.
class ShaderBuilder
{
public:
    template <typename T>
    T vertexInput()
    {
        auto value = T {};
        value.graph = &graphData;
        value.node = graphData.addInput(ValueTypeOf<T>::value);
        return value;
    }

    // Per-instance sibling of vertexInput<T>: the emitted VertexLayout routes
    // it to its own buffer slot with PerInstance step rate, which the caller
    // binds separately. The zero-arg form auto-assigns slot 1.
    template <typename T>
    T instanceInput()
    {
        auto value = T {};
        value.graph = &graphData;
        value.node = graphData.addInstanceInput(ValueTypeOf<T>::value);
        return value;
    }

    template <typename T>
    T instanceInput(int bufferIndex)
    {
        auto value = T {};
        value.graph = &graphData;
        value.node = graphData.addInstanceInput(ValueTypeOf<T>::value, bufferIndex);
        return value;
    }

    template <typename T>
    T varying(const T& vertexValue)
    {
        auto value = T {};
        value.graph = &graphData;
        value.node = graphData.addVarying(ValueTypeOf<T>::value, vertexValue.node);
        return value;
    }

    // A per-frame constant. Every value type crosses from the CPU except
    // Float2x2, Float3x3 and the booleans, which MSL and HLSL size differently
    // (see UniformLayout.h).
    template <typename T>
    T uniform()
    {
        static_assert(!isMatrix(ValueTypeOf<T>::value)
                          || ValueTypeOf<T>::value == ValueType::Float4x4,
                      "Float2x2/Float3x3 cannot be uniforms: MSL and HLSL pack "
                      "them to different sizes. Use a Float4x4, or pass the "
                      "columns as vectors and build the matrix in the shader.");

        static_assert(!isBoolean(ValueTypeOf<T>::value),
                      "Bool cannot be a uniform: MSL packs it into one byte and "
                      "an HLSL cbuffer into four. Send a Float and compare it.");

        auto value = T {};
        value.graph = &graphData;
        value.node = graphData.addUniform(ValueTypeOf<T>::value);
        return value;
    }

    // Bind the matching GPU::Texture at the same slot.
    Texture2D texture(TextureSampling sampling = {})
    {
        return {&graphData, graphData.addTexture(sampling)};
    }

    // Compute kernel authoring. Buffers take slots in call order, inputs and
    // outputs sharing one slot space; recording a write() is what marks the
    // built shader as compute.
    UInt threadId()
    {
        auto value = UInt {};
        value.graph = &graphData;
        value.node = graphData.addThreadId();
        return value;
    }

    // Asking for this makes the kernel a 2D one, dispatched with
    // ComputePass::dispatch(width, height). Take this or threadId(), not both.
    ThreadPosition threadPosition()
    {
        auto position = ThreadPosition {};

        position.x.graph = &graphData;
        position.x.node = graphData.addThreadPosition(0);
        position.y.graph = &graphData;
        position.y.node = graphData.addThreadPosition(1);

        return position;
    }

    // Where a thread sits inside its threadgroup, and which group it belongs
    // to. Asking for one fixes the dispatch rank as the global ids do.
    UInt localId()
    {
        auto value = UInt {};
        value.graph = &graphData;
        value.node = graphData.addLocalId();
        return value;
    }

    ThreadPosition localPosition()
    {
        auto position = ThreadPosition {};

        position.x.graph = &graphData;
        position.x.node = graphData.addLocalPosition(0);
        position.y.graph = &graphData;
        position.y.node = graphData.addLocalPosition(1);

        return position;
    }

    UInt groupId()
    {
        auto value = UInt {};
        value.graph = &graphData;
        value.node = graphData.addGroupId();
        return value;
    }

    ThreadPosition groupPosition()
    {
        auto position = ThreadPosition {};

        position.x.graph = &graphData;
        position.x.node = graphData.addGroupPosition(0);
        position.y.graph = &graphData;
        position.y.node = graphData.addGroupPosition(1);

        return position;
    }

    // The implicit grid bound the dispatch supplied, readable in the body: what
    // a barriering kernel, which gets no generated guard, bounds its stores by.
    UInt gridCount()
    {
        auto value = UInt {};
        value.graph = &graphData;
        value.node = graphData.addGridExtent(DispatchRank::OneD, 0);
        return value;
    }

    UInt gridWidth()
    {
        auto value = UInt {};
        value.graph = &graphData;
        value.node = graphData.addGridExtent(DispatchRank::TwoD, 0);
        return value;
    }

    UInt gridHeight()
    {
        auto value = UInt {};
        value.graph = &graphData;
        value.node = graphData.addGridExtent(DispatchRank::TwoD, 1);
        return value;
    }

    // A compile-time constant in the emitted kernel: size it against the group
    // shape the dispatch uses (ComputePass::threadGroupWidth in 1D,
    // threadGroupSize2D squared in 2D).
    template <typename T>
    Shared<T> shared(int count)
    {
        return {&graphData, graphData.addSharedArray(ValueTypeOf<T>::value, count)};
    }

    // Recording one removes the kernel's early-return bounds guard - a barrier
    // below a return some threads took is undefined on both backends - so the
    // kernel must bound its own stores, typically with ifThen(id < gridCount()).
    void barrier() { graphData.addBarrier(); }

    InputBuffer inputBuffer()
    {
        return {&graphData, graphData.addStorageBuffer(BufferAccess::Read)};
    }

    OutputBuffer outputBuffer()
    {
        return {&graphData, graphData.addStorageBuffer(BufferAccess::Write)};
    }

    // A buffer of unsigned integers, taking a slot from the same counter the
    // other two do and binding exactly as an output does.
    AtomicBuffer atomicBuffer()
    {
        return {&graphData, graphData.addStorageBuffer(BufferAccess::Atomic)};
    }

    // Adds to one element and yields what it held *before*. Relaxed ordering:
    // the read-modify-write cannot be interleaved, and nothing is promised
    // about other memory either side of it.
    UInt atomicAdd(const AtomicBuffer& buffer, const UInt& index, const UInt& value)
    {
        auto previous = graphData.addAtomicAdd(buffer.slot, index.node, value.node);

        auto result = UInt {};
        result.graph = &graphData;
        result.node = graphData.addVarRead(previous);
        return result;
    }

    UInt atomicAdd(const AtomicBuffer& buffer, unsigned index, const UInt& value)
    {
        return atomicAdd(buffer, buffer.literal(index), value);
    }

    UInt atomicAdd(const AtomicBuffer& buffer, const UInt& index, unsigned value)
    {
        return atomicAdd(buffer, index, buffer.literal(value));
    }

    UInt atomicAdd(const AtomicBuffer& buffer, unsigned index, unsigned value)
    {
        return atomicAdd(buffer, buffer.literal(index), buffer.literal(value));
    }

    // Takes a slot from the same counter texture() does, so a kernel reading
    // one texture and writing another binds them at distinct indices.
    WritableTexture2D writableTexture()
    {
        return {&graphData, graphData.addWritableTexture()};
    }

    void write(const OutputBuffer& buffer, const UInt& index, const Float& value)
    {
        graphData.addStore(buffer.slot, index.node, value.node);
    }

    // The vector writes lay a record of N floats down at index * N, matching
    // InputBuffer::read2/3/4. The index is in records, not floats. N scalar
    // stores, so the buffer stays a run of floats bindable as a vertex stream.
    void write(const OutputBuffer& buffer, const UInt& index, const Float2& value)
    {
        auto base = index * 2u;
        graphData.addStore(buffer.slot, base.node, value.x().node);
        graphData.addStore(buffer.slot, (base + 1u).node, value.y().node);
    }

    void write(const OutputBuffer& buffer, const UInt& index, const Float3& value)
    {
        auto base = index * 3u;
        graphData.addStore(buffer.slot, base.node, value.x().node);
        graphData.addStore(buffer.slot, (base + 1u).node, value.y().node);
        graphData.addStore(buffer.slot, (base + 2u).node, value.z().node);
    }

    void write(const OutputBuffer& buffer, const UInt& index, const Float4& value)
    {
        auto base = index * 4u;
        graphData.addStore(buffer.slot, base.node, value.x().node);
        graphData.addStore(buffer.slot, (base + 1u).node, value.y().node);
        graphData.addStore(buffer.slot, (base + 2u).node, value.z().node);
        graphData.addStore(buffer.slot, (base + 3u).node, value.w().node);
    }

    // One element of an atomic buffer, set outright rather than added to.
    void write(const AtomicBuffer& buffer, const UInt& index, const UInt& value)
    {
        graphData.addStore(buffer.slot, index.node, value.node);
    }

    void write(const AtomicBuffer& buffer, const UInt& index, unsigned value)
    {
        write(buffer, index, buffer.literal(value));
    }

    void write(const AtomicBuffer& buffer, unsigned index, const UInt& value)
    {
        write(buffer, buffer.literal(index), value);
    }

    void write(const AtomicBuffer& buffer, unsigned index, unsigned value)
    {
        write(buffer, buffer.literal(index), buffer.literal(value));
    }

    // A single wide store whatever the element type: the array never crosses
    // the CPU boundary, so there is no layout contract to keep.
    template <typename T>
    void write(const Shared<T>& array, const UInt& index, const T& value)
    {
        graphData.addSharedStore(array.slot, index.node, value.node);
    }

    // One texel of a kernel's output image.
    void write(const WritableTexture2D& texture,
               const UInt& x,
               const UInt& y,
               const Float4& color)
    {
        graphData.addTextureStore(texture.slot, x.node, y.node, color.node);
    }

    // Non-templated siblings of vertexInput()/uniform() keyed on a runtime
    // ValueType, for the reflection-driven ShaderProgram visitor.
    detail::ValueHandle addVertexInput(ValueType type)
    {
        return {&graphData, graphData.addInput(type)};
    }

    detail::ValueHandle addInstanceInput(ValueType type, int bufferIndex)
    {
        return {&graphData, graphData.addInstanceInput(type, bufferIndex)};
    }

    detail::ValueHandle addUniform(ValueType type)
    {
        return {&graphData, graphData.addUniform(type)};
    }

    Float constant(float value)
    {
        auto result = Float {};
        result.graph = &graphData;
        result.node = graphData.addConstant(value);
        return result;
    }

    // Named apart from constant() because an integer literal converts to both
    // float and bool, which would make constant(1) ambiguous.
    Bool boolean(bool value)
    {
        auto result = Bool {};
        result.graph = &graphData;
        result.node = graphData.addBoolConstant(value);
        return result;
    }

    Int integer(int value)
    {
        auto result = Int {};
        result.graph = &graphData;
        result.node = graphData.addIntConstant(value);
        return result;
    }

    // A constant array, its size taken from the pack. Elements are evaluated
    // once at the top of the shader body, so none of them may be a mutable
    // local.
    template <ShaderHandleLike T, SameShaderHandle<T>... Rest>
    ConstantArray<ShaderHandle<T>, 1 + (int) sizeof...(Rest)>
        array(const T& first, const Rest&... rest)
    {
        auto elements = Vector<int> {};
        elements.add(ShaderHandle<T>(first).node);
        (elements.add(ShaderHandle<T>(rest).node), ...);

        return {&graphData,
                graphData.addArray(ValueTypeOf<ShaderHandle<T>>::value,
                                   std::move(elements))};
    }

    // A mutable local, its type following the initialiser. Statements are
    // emitted into the fragment (or kernel) function, so a variable must not
    // feed the position expression or a varying.
    template <ShaderHandleLike T>
    Var<ShaderHandle<T>> var(const T& initialValue)
    {
        return {graphData,
                ValueTypeOf<ShaderHandle<T>>::value,
                ShaderHandle<T>(initialValue).node};
    }

    Var<Float> var(float initialValue)
    {
        return {graphData, ValueType::Float, graphData.addConstant(initialValue)};
    }

    Var<Bool> var(bool initialValue)
    {
        return {graphData, ValueType::Bool, graphData.addBoolConstant(initialValue)};
    }

    Var<Int> var(int initialValue)
    {
        return {graphData, ValueType::Int, graphData.addIntConstant(initialValue)};
    }

    Var<UInt> var(unsigned initialValue)
    {
        return {graphData, ValueType::UInt, graphData.addUIntConstant(initialValue)};
    }

    // A matrix needs its own overload: it is outside all three handle families.
    template <typename T>
        requires(isMatrix(ValueTypeOf<T>::value))
    Var<T> var(const T& initialValue)
    {
        return {graphData, ValueTypeOf<T>::value, initialValue.node};
    }

    // Each body records into a block of its own, so what it declares is scoped
    // to it in the emitted source as it is in the C++ lambda that wrote it.
    template <typename Body>
    void ifThen(const Bool& condition, Body&& body)
    {
        auto block = graphData.pushBlock();
        body();
        graphData.popBlock();

        graphData.addIf(condition.node, block, -1);
    }

    template <typename Then, typename Else>
    void ifThen(const Bool& condition, Then&& whenTrue, Else&& whenFalse)
    {
        auto thenBlock = graphData.pushBlock();
        whenTrue();
        graphData.popBlock();

        auto elseBlock = graphData.pushBlock();
        whenFalse();
        graphData.popBlock();

        graphData.addIf(condition.node, thenBlock, elseBlock);
    }

    // The condition is built before the loop but printed into the while header
    // in place, so it is re-evaluated every iteration rather than hoisted.
    template <typename Body>
    void loop(const Bool& condition, Body&& body)
    {
        auto block = graphData.pushBlock();
        body();
        graphData.popBlock();

        graphData.addLoop(condition.node, block);
    }

    void breakLoop() { graphData.addBreak(); }
    void continueLoop() { graphData.addContinue(); }

    void position(const Float4& clipPosition);
    void fragment(const Float4& color);

    void discardBelow(const Float& value, float threshold)
    {
        graphData.setDiscard(value.node, threshold);
    }

    GeneratedShader build() const;

    const ShaderGraph& graph() const { return graphData; }

private:
    ShaderGraph graphData;
};
} // namespace eacp::GPU
