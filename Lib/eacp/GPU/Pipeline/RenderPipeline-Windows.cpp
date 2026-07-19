#include <eacp/Core/Utils/WinInclude.h>

#include "RenderPipeline.h"

#include "../Device/Device.h"
#include "../Shader/ShaderLibrary.h"
#include "../Windows/D3D12Types.h"

#include <winrt/base.h>

// Windows/D3D12 backend. Everything the D3D11 backend kept as five separate
// state objects bakes into a single pipeline-state object against the shared
// render root signature; only the topology and vertex stride stay outside the
// PSO, read by the render pass at draw time.

namespace eacp::GPU
{
namespace
{
D3D12_PRIMITIVE_TOPOLOGY toD3DTopology(PrimitiveTopology topology)
{
    switch (topology)
    {
        case PrimitiveTopology::Triangles:
            return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        case PrimitiveTopology::TriangleStrip:
            return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        case PrimitiveTopology::Lines:
            return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        case PrimitiveTopology::LineStrip:
            return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        case PrimitiveTopology::Points:
            return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    }

    return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE toTopologyType(PrimitiveTopology topology)
{
    switch (topology)
    {
        case PrimitiveTopology::Triangles:
        case PrimitiveTopology::TriangleStrip:
            return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        case PrimitiveTopology::Lines:
        case PrimitiveTopology::LineStrip:
            return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        case PrimitiveTopology::Points:
            return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    }

    return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
}

DXGI_FORMAT toDXGIFormat(VertexFormat format)
{
    switch (format)
    {
        case VertexFormat::Float:
            return DXGI_FORMAT_R32_FLOAT;
        case VertexFormat::Float2:
            return DXGI_FORMAT_R32G32_FLOAT;
        case VertexFormat::Float3:
            return DXGI_FORMAT_R32G32B32_FLOAT;
        case VertexFormat::Float4:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
    }

    return DXGI_FORMAT_R32G32B32_FLOAT;
}

// Vertex attributes are matched to the HLSL input by a TEXCOORD semantic
// indexed by attribute position, mirroring Metal's [[attribute(n)]] binding.
// Reads the step rate for a given slot from the layout. Multi-buffer layouts
// carry per-slot metadata; legacy single-buffer layouts (buffers empty) are
// always PerVertex at slot 0.
StepRate stepRateForSlot(const VertexLayout& layout, int slot)
{
    if (slot >= 0 && slot < (int) layout.buffers.size())
        return layout.buffers[slot].stepRate;
    return StepRate::PerVertex;
}

Vector<D3D12_INPUT_ELEMENT_DESC> makeInputLayout(const VertexLayout& layout)
{
    auto elements = Vector<D3D12_INPUT_ELEMENT_DESC>();

    for (auto i = 0; i < layout.attributes.size(); ++i)
    {
        const auto& attribute = layout.attributes[i];
        auto rate = stepRateForSlot(layout, attribute.bufferIndex);
        auto perInstance = rate == StepRate::PerInstance;

        D3D12_INPUT_ELEMENT_DESC element = {};
        element.SemanticName = "TEXCOORD";
        element.SemanticIndex = static_cast<UINT>(i);
        element.Format = toDXGIFormat(attribute.format);
        element.InputSlot = static_cast<UINT>(attribute.bufferIndex);
        element.AlignedByteOffset = static_cast<UINT>(attribute.offset);
        element.InputSlotClass = perInstance
                                     ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                     : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        element.InstanceDataStepRate = perInstance ? 1 : 0;

        elements.push_back(element);
    }

    return elements;
}

// Builds the per-slot stride table the RenderPass reads at setVertexBuffer
// time. Populated from layout.buffers when present; falls back to a single
// slot with layout.stride so single-buffer callers see no behavioural change.
Vector<UINT> makeStrideTable(const VertexLayout& layout)
{
    if (!layout.buffers.empty())
    {
        auto strides = Vector<UINT>();
        strides.reserve(layout.buffers.size());
        for (auto i = 0; i < layout.buffers.size(); ++i)
            strides.push_back(static_cast<UINT>(layout.buffers[i].stride));
        return strides;
    }

    return {static_cast<UINT>(layout.stride)};
}

D3D12_RASTERIZER_DESC makeRasterizerDesc(int sampleCount)
{
    D3D12_RASTERIZER_DESC desc = {};
    desc.FillMode = D3D12_FILL_MODE_SOLID;
    // Match Metal's default of no face culling.
    desc.CullMode = D3D12_CULL_MODE_NONE;
    desc.DepthClipEnable = TRUE;
    desc.MultisampleEnable = sampleCount > 1 ? TRUE : FALSE;
    return desc;
}

D3D12_BLEND_DESC makeBlendDesc(BlendMode mode)
{
    D3D12_BLEND_DESC desc = {};
    auto& target = desc.RenderTarget[0];
    target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    switch (mode)
    {
        case BlendMode::None:
            return desc;
        case BlendMode::AlphaBlend:
            target.BlendEnable = TRUE;
            target.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            target.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            target.BlendOp = D3D12_BLEND_OP_ADD;
            target.SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
            target.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            return desc;
        case BlendMode::Additive:
            target.BlendEnable = TRUE;
            target.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            target.DestBlend = D3D12_BLEND_ONE;
            target.BlendOp = D3D12_BLEND_OP_ADD;
            target.SrcBlendAlpha = D3D12_BLEND_ONE;
            target.DestBlendAlpha = D3D12_BLEND_ONE;
            target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            return desc;
        default:
            // Guards against a future BlendMode value that this backend was
            // never taught to handle - would otherwise silently produce a
            // no-blend pipeline (that's what the zero-initialised desc is).
            // Loud in Debug, degrades to None in Release (both backends
            // match this behaviour).
            assert(false && "eacp: unhandled BlendMode in D3D12 backend");
            return desc;
    }
}

// Less-equal depth test with depth writes on, matching the Metal backend. The
// [0,1] depth range is shared by both APIs, so no convention flip is needed.
D3D12_DEPTH_STENCIL_DESC makeDepthStencilDesc(bool depth)
{
    D3D12_DEPTH_STENCIL_DESC desc = {};

    if (!depth)
        return desc;

    desc.DepthEnable = TRUE;
    desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    return desc;
}
} // namespace

struct RenderPipeline::Native
{
    Native(Device& device, const RenderPipelineDescriptor& descriptor)
        : topology(descriptor.topology)
    {
        pipeline.topology = toD3DTopology(descriptor.topology);
        pipeline.strides = makeStrideTable(descriptor.vertexLayout);
        pipeline.depth = descriptor.depth;

        auto& context = getD3D12Context();

        if (!context.isValid() || !device.isValid()
            || context.getRenderRootSignature() == nullptr
            || descriptor.library == nullptr)
            return;

        auto* program =
            static_cast<D3D12ShaderProgram*>(descriptor.library->nativeLibrary());

        if (program == nullptr || program->vertexBytecode == nullptr
            || program->pixelBytecode == nullptr)
            return;

        auto inputLayout = makeInputLayout(descriptor.vertexLayout);

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = context.getRenderRootSignature();
        desc.VS.pShaderBytecode = program->vertexBytecode->GetBufferPointer();
        desc.VS.BytecodeLength = program->vertexBytecode->GetBufferSize();
        desc.PS.pShaderBytecode = program->pixelBytecode->GetBufferPointer();
        desc.PS.BytecodeLength = program->pixelBytecode->GetBufferSize();
        desc.BlendState = makeBlendDesc(descriptor.blendMode);
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState = makeRasterizerDesc(descriptor.sampleCount);
        desc.DepthStencilState = makeDepthStencilDesc(descriptor.depth);
        desc.InputLayout.pInputElementDescs = inputLayout.data();
        desc.InputLayout.NumElements = static_cast<UINT>(inputLayout.size());
        desc.PrimitiveTopologyType = toTopologyType(descriptor.topology);
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.DSVFormat =
            descriptor.depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = static_cast<UINT>(
            descriptor.sampleCount > 1 ? descriptor.sampleCount : 1);

        context.getDevice()->CreateGraphicsPipelineState(
            &desc, __uuidof(ID3D12PipelineState), pipeline.state.put_void());
    }

    // A command list holds a reference to every pipeline state it binds, so a
    // PSO built and dropped inside one frame — which is what constructing a
    // SpriteRenderer in render() does — has to outlive the recording rather
    // than release here. See D3D12Context::deferRelease.
    ~Native() { getD3D12Context().deferRelease(std::move(pipeline.state)); }

    PrimitiveTopology topology = PrimitiveTopology::Triangles;
    D3D12Pipeline pipeline;
};

RenderPipeline::RenderPipeline(Device& device,
                               const RenderPipelineDescriptor& descriptor)
    : impl(device, descriptor)
{
}

bool RenderPipeline::isValid() const
{
    return impl->pipeline.state != nullptr;
}

PrimitiveTopology RenderPipeline::topology() const
{
    return impl->topology;
}

void* RenderPipeline::nativeState() const
{
    return const_cast<D3D12Pipeline*>(&impl->pipeline);
}

void* RenderPipeline::nativeDepthState() const
{
    // Depth state is baked into the PSO on D3D12; the handle exists for the
    // Metal backend, which binds it separately.
    return impl->pipeline.depth ? const_cast<D3D12Pipeline*>(&impl->pipeline)
                                : nullptr;
}
} // namespace eacp::GPU
