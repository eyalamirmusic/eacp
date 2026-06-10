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

    template <typename T>
    T varying(const T& vertexValue)
    {
        auto value = T {};
        value.graph = &graphData;
        value.node = graphData.addVarying(ValueTypeOf<T>::value, vertexValue.node);
        return value;
    }

    template <typename T>
    T uniform()
    {
        auto value = T {};
        value.graph = &graphData;
        value.node = graphData.addUniform(ValueTypeOf<T>::value);
        return value;
    }

    // A 2D texture sampled by the fragment expression. Returns the slot-indexed
    // handle sample() reads; bind the matching GPU::Texture at the same slot.
    Texture2D texture() { return {&graphData, graphData.addTexture()}; }

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

    void position(const Float4& clipPosition);
    void fragment(const Float4& color);

    GeneratedShader build() const;

    const ShaderGraph& graph() const { return graphData; }

private:
    ShaderGraph graphData;
};
} // namespace eacp::GPU
