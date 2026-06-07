#pragma once

#include "../Pipeline/VertexLayout.h"
#include "../Shader/ShaderSource.h"
#include "GeneratedShader.h"
#include "ShaderEmitter.h"
#include "ShaderGraph.h"
#include "ShaderTypes.h"
#include "ShaderValue.h"

#include <utility>

namespace eacp::GPU
{
inline VertexFormat toVertexFormat(ValueType type)
{
    switch (type)
    {
        case ValueType::Float:
            return VertexFormat::Float;
        case ValueType::Float2:
            return VertexFormat::Float2;
        case ValueType::Float3:
            return VertexFormat::Float3;
        case ValueType::Float4:
            return VertexFormat::Float4;
    }

    return VertexFormat::Float;
}

inline VertexLayout buildVertexLayout(const ShaderGraph& graph)
{
    auto layout = VertexLayout {};
    auto offset = 0;

    for (auto i = 0; i < graph.inputs().size(); ++i)
    {
        auto type = graph.inputs()[i];
        layout.attribute(toVertexFormat(type), offset);
        offset += byteSize(type);
    }

    layout.stride = offset;
    return layout;
}

// The string-free authoring entry point. Declare vertex inputs and varyings by
// call order, write the position and fragment outputs with value-type
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

    void position(const Float4& clipPosition)
    {
        graphData.setPosition(clipPosition.node);
    }

    void fragment(const Float4& color) { graphData.setFragment(color.node); }

    GeneratedShader build() const
    {
#if defined(_WIN32)
        auto source = ShaderSource::hlsl(emitHlsl(graphData));
#else
        auto source = ShaderSource::msl(emitMetal(graphData));
#endif
        source.withVertex("vertexMain").withFragment("fragmentMain");

        auto result = GeneratedShader {};
        result.source = std::move(source);
        result.vertexLayout = buildVertexLayout(graphData);
        return result;
    }

    const ShaderGraph& graph() const { return graphData; }

private:
    ShaderGraph graphData;
};
} // namespace eacp::GPU
