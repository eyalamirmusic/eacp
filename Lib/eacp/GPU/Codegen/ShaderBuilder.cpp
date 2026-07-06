#include "ShaderBuilder.h"

#include "../Pipeline/VertexLayout.h"

#include <cassert>
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
        case ValueType::Float4x4:
        case ValueType::UInt:
            return VertexFormat::Float4; // matrix/uint are never vertex attributes
    }

    return VertexFormat::Float;
}

VertexLayout buildVertexLayout(const ShaderGraph& graph)
{
    auto layout = VertexLayout {};

    // Group inputs by slot: each slot's attributes accumulate their offsets in
    // declaration order, and the slot's stride is the sum of its byte sizes.
    // Step rate is inherited from the first attribute assigned to a slot -
    // callers who pass explicit bufferIndex are responsible for keeping all
    // attributes in a given slot at the same rate.
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

        // First attribute in a slot establishes its step rate; every later
        // attribute must match. Mixing PerVertex + PerInstance in a single
        // slot would produce a subtly wrong pipeline that each backend
        // resolves differently - a silent cross-platform footgun. Loud in
        // Debug matches the assert-on-unhandled-mode convention added in
        // the BlendModes PR.
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
        // Multi-slot layout: publish stride + rate for every slot the graph
        // populated. Empty leading slots (rare) get PerVertex + stride 0 by
        // default, which is a safe no-op for backends.
        for (auto slot = 0; slot < perSlotOffsets.size(); ++slot)
            layout.buffer(slot, perSlotOffsets[slot], perSlotRates[slot]);
    }
    else
    {
        // Single-buffer shortcut: keep the pre-instancing shape (buffers empty,
        // stride populated) so existing single-buffer consumers see no change.
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
        return result;
    }

    source.withVertex("vertexMain").withFragment("fragmentMain");
    result.source = std::move(source);
    result.vertexLayout = buildVertexLayout(graphData);
    return result;
}
} // namespace eacp::GPU
