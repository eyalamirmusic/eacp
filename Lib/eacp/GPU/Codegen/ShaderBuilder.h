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

    void position(const Float4& clipPosition);
    void fragment(const Float4& color);

    GeneratedShader build() const;

    const ShaderGraph& graph() const { return graphData; }

private:
    ShaderGraph graphData;
};
} // namespace eacp::GPU
