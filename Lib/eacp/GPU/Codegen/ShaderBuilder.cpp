#include "ShaderBuilder.h"

#include "../Pipeline/VertexLayout.h"
#include "ShaderEmitter.h"

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
        case ValueType::Float2x2:
        case ValueType::Float3x3:
        case ValueType::Float4x4:
        case ValueType::UInt:
        case ValueType::Int:
        case ValueType::Int2:
        case ValueType::Int3:
        case ValueType::Int4:
        case ValueType::Bool:
        case ValueType::Bool2:
        case ValueType::Bool3:
        case ValueType::Bool4:
            return VertexFormat::Float4; // matrix/integer/bool are never attributes
    }

    return VertexFormat::Float;
}

VertexLayout buildVertexLayout(const ShaderGraph& graph)
{
    auto layout = VertexLayout {};

    // Each slot's attributes accumulate offsets in declaration order.
    auto perSlotOffsets = Vector<int> {};
    auto perSlotRates = Vector<StepRate> {};
    auto sawInstance = false;

    for (auto i = 0; i < graph.inputs().size(); ++i)
    {
        auto type = graph.inputs()[i];
        auto rate = graph.inputStepRates()[i];
        auto slot = graph.inputBufferIndices()[i];

        while (perSlotOffsets.size() <= slot)
        {
            perSlotOffsets.add(0);
            perSlotRates.add(StepRate::PerVertex);
        }

        // The first attribute in a slot establishes its step rate; mixing rates
        // within one slot resolves differently on each backend.
        auto firstInSlot = perSlotOffsets[slot] == 0;
        if (firstInSlot)
        {
            perSlotRates[slot] = rate;
        }
        else
        {
            assert(perSlotRates[slot] == rate
                   && "eacp: attributes in a single vertex-buffer slot must "
                      "share a step rate (all PerVertex or all PerInstance)");
        }

        layout.attribute(toVertexFormat(type), perSlotOffsets[slot], slot);
        perSlotOffsets[slot] += byteSize(type);

        if (rate == StepRate::PerInstance)
            sawInstance = true;
    }

    if (sawInstance)
    {
        // Empty leading slots get PerVertex and stride 0, a backend no-op.
        for (auto slot = 0; slot < perSlotOffsets.size(); ++slot)
            layout.buffer(slot, perSlotOffsets[slot], perSlotRates[slot]);
    }
    else
    {
        // Single-buffer shape: buffers empty, stride populated.
        layout.stride = perSlotOffsets.empty() ? 0 : perSlotOffsets[0];
    }

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

    auto result = GeneratedShader {};

    if (graphData.isCompute())
    {
        source.withCompute("computeMain");
        result.source = std::move(source);
        result.dispatchRank = graphData.dispatchRank();
        return result;
    }

    source.withVertex("vertexMain").withFragment("fragmentMain");
    result.source = std::move(source);
    result.vertexLayout = buildVertexLayout(graphData);
    result.vertexReadsUniforms = vertexReadsUniforms(graphData);
    result.fragmentReadsUniforms = fragmentReadsUniforms(graphData);
    return result;
}
} // namespace eacp::GPU
