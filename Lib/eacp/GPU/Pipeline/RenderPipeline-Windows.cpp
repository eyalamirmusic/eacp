#include <eacp/Core/Utils/WinInclude.h>

#include "RenderPipeline.h"

#include "../Device/Device.h"
#include "../Shader/ShaderLibrary.h"
#include "../Windows/D3D12Types.h"

#include <winrt/base.h>

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

        // UNORM/SNORM rather than UINT/SINT, so the shader reads 0..1 and -1..1
        // as Metal's Normalized formats do.
        case VertexFormat::UByte4Norm:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case VertexFormat::Half2:
            return DXGI_FORMAT_R16G16_FLOAT;
        case VertexFormat::Half4:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case VertexFormat::Short2Norm:
            return DXGI_FORMAT_R16G16_SNORM;
        case VertexFormat::Short4Norm:
            return DXGI_FORMAT_R16G16B16A16_SNORM;
    }

    return DXGI_FORMAT_R32G32B32_FLOAT;
}

DXGI_FORMAT toDXGIFormat(PixelFormat format)
{
    switch (format)
    {
        case PixelFormat::RGBA8Unorm:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case PixelFormat::RGBA16Float:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case PixelFormat::RGBA32Float:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case PixelFormat::BGRA8Unorm:
            break;
    }

    return DXGI_FORMAT_B8G8R8A8_UNORM;
}

// Attributes match the HLSL input by a TEXCOORD semantic indexed by attribute
// position, mirroring Metal's [[attribute(n)]].
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

// The per-slot stride table RenderPass reads at setVertexBuffer time.
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

D3D12_CULL_MODE toD3DCullMode(CullMode mode)
{
    switch (mode)
    {
        case CullMode::None:
            return D3D12_CULL_MODE_NONE;
        case CullMode::Front:
            return D3D12_CULL_MODE_FRONT;
        case CullMode::Back:
            return D3D12_CULL_MODE_BACK;
    }

    return D3D12_CULL_MODE_NONE;
}

D3D12_COMPARISON_FUNC toD3DComparison(DepthCompare compare)
{
    switch (compare)
    {
        case DepthCompare::Never:
            return D3D12_COMPARISON_FUNC_NEVER;
        case DepthCompare::Less:
            return D3D12_COMPARISON_FUNC_LESS;
        case DepthCompare::LessEqual:
            return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case DepthCompare::Equal:
            return D3D12_COMPARISON_FUNC_EQUAL;
        case DepthCompare::NotEqual:
            return D3D12_COMPARISON_FUNC_NOT_EQUAL;
        case DepthCompare::GreaterEqual:
            return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case DepthCompare::Greater:
            return D3D12_COMPARISON_FUNC_GREATER;
        case DepthCompare::Always:
            return D3D12_COMPARISON_FUNC_ALWAYS;
    }

    return D3D12_COMPARISON_FUNC_LESS_EQUAL;
}

// FrontCounterClockwise is TRUE for Winding::CounterClockwise - not the D3D12
// default, and the same spelling Metal uses, both APIs reversing winding by the
// same amount on the way to screen space (Tests/GPU/CullModeTests.cpp).
D3D12_RASTERIZER_DESC makeRasterizerDesc(const RenderPipelineDescriptor& from)
{
    D3D12_RASTERIZER_DESC desc = {};
    desc.FillMode = D3D12_FILL_MODE_SOLID;
    desc.CullMode = toD3DCullMode(from.cullMode);
    desc.FrontCounterClockwise = from.frontFace == Winding::CounterClockwise;
    desc.DepthClipEnable = TRUE;
    desc.MultisampleEnable = from.sampleCount > 1 ? TRUE : FALSE;
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
        case BlendMode::AlphaBlendOntoTransparent:
            target.BlendEnable = TRUE;
            target.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            target.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            target.BlendOp = D3D12_BLEND_OP_ADD;
            target.SrcBlendAlpha = D3D12_BLEND_ONE;
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
            // An unhandled BlendMode would silently give a no-blend pipeline;
            // loud in Debug, degrades to None in Release.
            assert(false && "eacp: unhandled BlendMode in D3D12 backend");
            return desc;
    }
}

// Both APIs share the [0,1] depth range, so the comparison needs no flip.
D3D12_DEPTH_STENCIL_DESC makeDepthStencilDesc(const RenderPipelineDescriptor& from)
{
    D3D12_DEPTH_STENCIL_DESC desc = {};

    if (!from.depth)
        return desc;

    desc.DepthEnable = TRUE;
    desc.DepthWriteMask =
        from.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthFunc = toD3DComparison(from.depthCompare);
    return desc;
}
} // namespace

struct RenderPipeline::Native
{
    Native(Device& device, const RenderPipelineDescriptor& descriptor)
        : topology(descriptor.topology)
        , cullMode(descriptor.cullMode)
        , frontFace(descriptor.frontFace)
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
        desc.RasterizerState = makeRasterizerDesc(descriptor);
        desc.DepthStencilState = makeDepthStencilDesc(descriptor);
        desc.InputLayout.pInputElementDescs = inputLayout.data();
        desc.InputLayout.NumElements = static_cast<UINT>(inputLayout.size());
        desc.PrimitiveTopologyType = toTopologyType(descriptor.topology);
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = toDXGIFormat(descriptor.colorFormat);
        desc.DSVFormat =
            descriptor.depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = static_cast<UINT>(
            descriptor.sampleCount > 1 ? descriptor.sampleCount : 1);

        context.getDevice()->CreateGraphicsPipelineState(
            &desc, __uuidof(ID3D12PipelineState), pipeline.state.put_void());
    }

    // A command list references every PSO it binds, so one built and dropped
    // inside a frame must outlive the recording. See D3D12Context::deferRelease.
    ~Native() { getD3D12Context().deferRelease(std::move(pipeline.state)); }

    PrimitiveTopology topology = PrimitiveTopology::Triangles;
    CullMode cullMode = CullMode::None;
    Winding frontFace = Winding::CounterClockwise;
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

// Already inside the PSO here; reported for parity with the Metal backend.
CullMode RenderPipeline::cullMode() const
{
    return impl->cullMode;
}

Winding RenderPipeline::frontFace() const
{
    return impl->frontFace;
}

void* RenderPipeline::nativeState() const
{
    return const_cast<D3D12Pipeline*>(&impl->pipeline);
}

void* RenderPipeline::nativeDepthState() const
{
    // Baked into the PSO on D3D12; the handle exists for Metal, which binds it
    // separately.
    return impl->pipeline.depth ? const_cast<D3D12Pipeline*>(&impl->pipeline)
                                : nullptr;
}
} // namespace eacp::GPU
