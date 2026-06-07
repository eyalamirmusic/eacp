#include "ShaderBuilder.h"

#include "../Pipeline/VertexLayout.h"

#include <utility>

namespace eacp::GPU
{
namespace
{
VertexFormat toVertexFormat(ValueType type)
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
        case ValueType::Float4x4:
            return VertexFormat::Float4; // matrices are never vertex attributes
    }

    return VertexFormat::Float;
}

VertexLayout buildVertexLayout(const ShaderGraph& graph)
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
} // namespace

void ShaderBuilder::position(const Float4& clipPosition)
{
    graphData.setPosition(clipPosition.node);
}

void ShaderBuilder::fragment(const Float4& color)
{
    graphData.setFragment(color.node);
}

GeneratedShader ShaderBuilder::build() const
{
    auto source = detail::nativeShaderSource(graphData);
    source.withVertex("vertexMain").withFragment("fragmentMain");

    auto result = GeneratedShader {};
    result.source = std::move(source);
    result.vertexLayout = buildVertexLayout(graphData);
    return result;
}
} // namespace eacp::GPU
