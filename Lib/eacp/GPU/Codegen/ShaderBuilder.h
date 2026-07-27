#pragma once

#include "../Shader/ShaderSource.h"
#include "GeneratedShader.h"
#include "ShaderGraph.h"
#include "ShaderValue.h"

namespace eacp::GPU
{
namespace detail
{
// Emits the native shader source for the platform's backend (MSL on Apple, HLSL
// on Windows). Defined per-platform in ShaderBuilder-macOS.cpp / -Windows.cpp,
// mirroring the rest of the GPU module, so build() needs no preprocessor branch.
ShaderSource nativeShaderSource(const ShaderGraph& graph);
} // namespace detail

// The string-free authoring entry point. Declare vertex inputs, uniforms and
// varyings by call order, write the position and fragment outputs with value-type
// expressions, then build() emits the current backend's source and the matching
// vertex layout. No native shader files, no string literals at the call site.
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

    // Per-instance sibling of vertexInput<T>. The returned handle behaves
    // identically in shader expressions; the emitted VertexLayout routes
    // instance inputs to a dedicated buffer slot with PerInstance step rate,
    // so the caller binds a separate per-instance buffer at that slot.
    //
    // The zero-arg form auto-assigns slot 1 (the common case: one per-instance
    // buffer alongside the per-vertex buffer at slot 0). Pass an explicit
    // bufferIndex when a shader needs multiple per-instance streams in
    // distinct buffers (e.g. per-instance transform in slot 1, per-instance
    // colour in slot 2).
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

    // A per-frame constant. Every value type crosses from the CPU except a
    // Float2x2 or a Float3x3: MSL and HLSL pack those to different sizes inside
    // the value itself, so one block of bytes would read back as two different
    // matrices on the two backends (see UniformLayout.h). Send a Float4x4, or
    // send the columns as vectors and assemble the matrix in the shader.
    //
    // A Bool is refused for the same reason at a smaller scale: MSL packs one
    // into a byte and an HLSL cbuffer gives it four. Send the flag as a float
    // and compare it, which is what the block already knows how to carry. The
    // boolean vectors go with it; the integer ones do not, since both languages
    // pack an intN exactly where they pack a floatN.
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

    // A 2D texture sampled by the fragment expression. Returns the slot-indexed
    // handle sample() reads; bind the matching GPU::Texture at the same slot.
    Texture2D texture(TextureSampling sampling = {})
    {
        return {&graphData, graphData.addTexture(sampling)};
    }

    // Compute kernel authoring. Declaring buffers assigns slots in call order
    // (inputs and outputs share one slot space, matching Metal's flat buffer
    // indices); threadId() is the 1D work-item index; write() records a kernel
    // output, which is what marks the built shader as compute.
    UInt threadId()
    {
        auto value = UInt {};
        value.graph = &graphData;
        value.node = graphData.addThreadId();
        return value;
    }

    InputBuffer inputBuffer()
    {
        return {&graphData, graphData.addStorageBuffer(BufferAccess::Read)};
    }

    OutputBuffer outputBuffer()
    {
        return {&graphData, graphData.addStorageBuffer(BufferAccess::Write)};
    }

    void write(const OutputBuffer& buffer, const UInt& index, const Float& value)
    {
        graphData.addStore(buffer.slot, index.node, value.node);
    }

    // Non-templated siblings of vertexInput()/uniform() keyed on a runtime
    // ValueType. The reflection-driven ShaderProgram visitor walks erased member
    // handles, so it needs to add a slot from a ValueType it carries rather than a
    // compile-time T. The returned handle is adopted by the declaring member.
    detail::ValueHandle addVertexInput(ValueType type)
    {
        return {&graphData, graphData.addInput(type)};
    }

    // Per-instance sibling of addVertexInput, keyed on a runtime ValueType for
    // the reflection-driven ShaderProgram path. Routes the input to the given
    // buffer slot with PerInstance step rate (see the templated instanceInput).
    detail::ValueHandle addInstanceInput(ValueType type, int bufferIndex)
    {
        return {&graphData, graphData.addInstanceInput(type, bufferIndex)};
    }

    detail::ValueHandle addUniform(ValueType type)
    {
        return {&graphData, graphData.addUniform(type)};
    }

    // A scalar literal usable in expressions (e.g. an ambient term).
    Float constant(float value)
    {
        auto result = Float {};
        result.graph = &graphData;
        result.node = graphData.addConstant(value);
        return result;
    }

    // Its boolean sibling, for a flag a shader sets and later tests. Spelled
    // apart from constant() rather than overloaded on it: an integer literal
    // converts to both float and bool, so one name would make constant(1)
    // ambiguous.
    Bool boolean(bool value)
    {
        auto result = Bool {};
        result.graph = &graphData;
        result.node = graphData.addBoolConstant(value);
        return result;
    }

    // And its integer one, for an index a shader starts from. Spelled apart for
    // the same reason: constant(1) would otherwise be ambiguous.
    Int integer(int value)
    {
        auto result = Int {};
        result.graph = &graphData;
        result.node = graphData.addIntConstant(value);
        return result;
    }

    // A constant array, its size taken from the pack. Every element is an
    // ordinary value - a literal vector, or something built from a uniform -
    // and all of them are evaluated once at the top of the shader body, which
    // is why a mutable local cannot be one. See ConstantArray for what a
    // subscript of one costs and what bounds it.
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

    // A mutable local, initialised from a value or from a literal. This is what
    // makes a loop worth having: something the body can write that the code
    // after it reads. Its type follows the initialiser.
    //
    // Control flow is a fragment-stage (or kernel) facility, like sampling: the
    // statements a shader records are emitted into the fragment function, so a
    // variable must not feed the position expression or a varying.
    // Constrained on the handle rather than on the float vocabulary, so the
    // flag a shader sets and later tests and the cell it walks a grid with are
    // variables on the same terms a colour is.
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

    // A matrix is a mutable local on the same terms - the orientation a shader
    // builds up over several steps before it goes through it - and it needs an
    // overload of its own for the reason it needs one everywhere here: it is
    // outside all three handle families, none of whose operators it has.
    template <typename T>
        requires(isMatrix(ValueTypeOf<T>::value))
    Var<T> var(const T& initialValue)
    {
        return {graphData, ValueTypeOf<T>::value, initialValue.node};
    }

    // The statements. Each body is a callable recording into a block of its
    // own, so what it declares is scoped to it in the emitted source exactly as
    // it is in the C++ lambda that wrote it.
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

    // The condition is a value built before the loop, and it is still re-tested
    // every iteration: what the emitter writes into the while header is the
    // expression, printed in place, not a name bound to it beforehand. That is
    // why a condition never becomes one of the emitter's shared locals - a loop
    // testing a value computed once before it started would never end.
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
