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

        // UNORM and SNORM rather than UINT and SINT: the shader reads these as
        // 0..1 and -1..1, which is what the Normalized Metal formats give. The
        // integer variants would deliver raw 0..255 and disagree with the other
        // backend rather than fail, which is why VertexFormatTests compares a
        // packed render against an unpacked one instead of trusting either.
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

// The attachment the pipeline writes. It has to match whatever the pass binds
// as its render target - the swapchain back buffer, or a texture created with
// TextureDescriptor::renderTarget.
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

D3D12_COMPARISON_FUNC toD3DComparison(CompareFunction compare)
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

// Culling here is pipeline state, where Metal makes it encoder state - the one
// place in this file where the two APIs disagree about *when* a setting is
// fixed rather than about what it is called. eacp resolves that the way it
// already resolves topology: the descriptor owns it, and the Metal backend
// applies it when the pipeline is bound.
//
// FrontCounterClockwise is TRUE for Winding::CounterClockwise, which is not the
// D3D12 default and is what delivers the convention CullMode promises.
//
// The tempting reading is that D3D12 needs the opposite of Metal because it
// decides facing in screen space, after a y flip Metal does not have. It has no
// such extra flip: both APIs put clip-space y up and the framebuffer origin at
// the top left, so the NDC-to-screen mapping reverses winding by exactly the
// same amount on each, and one convention is spelled the same way on both.
// Measured, not reasoned - it was FALSE on that reasoning and
// Tests/GPU/CullModeTests.cpp culled the opposite face.
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

D3D12_BLEND toD3DBlendFactor(BlendFactor factor)
{
    switch (factor)
    {
        case BlendFactor::Zero:
            return D3D12_BLEND_ZERO;
        case BlendFactor::One:
            return D3D12_BLEND_ONE;
        case BlendFactor::SourceColor:
            return D3D12_BLEND_SRC_COLOR;
        case BlendFactor::OneMinusSourceColor:
            return D3D12_BLEND_INV_SRC_COLOR;
        case BlendFactor::SourceAlpha:
            return D3D12_BLEND_SRC_ALPHA;
        case BlendFactor::OneMinusSourceAlpha:
            return D3D12_BLEND_INV_SRC_ALPHA;
        case BlendFactor::DestinationColor:
            return D3D12_BLEND_DEST_COLOR;
        case BlendFactor::OneMinusDestinationColor:
            return D3D12_BLEND_INV_DEST_COLOR;
        case BlendFactor::DestinationAlpha:
            return D3D12_BLEND_DEST_ALPHA;
        case BlendFactor::OneMinusDestinationAlpha:
            return D3D12_BLEND_INV_DEST_ALPHA;
        case BlendFactor::SourceAlphaSaturated:
            return D3D12_BLEND_SRC_ALPHA_SAT;
    }

    return D3D12_BLEND_ONE;
}

// The alpha slots, where D3D12 is narrower than Metal and the difference is a
// spelling rather than a capability.
//
// SrcBlendAlpha and DestBlendAlpha reject the four *_COLOR factors outright:
// a pipeline naming D3D12_BLEND_SRC_COLOR there fails to create, with the
// debug layer saying so. Metal accepts the same request and computes what the
// name means - and in the alpha channel, the alpha component of SourceColor
// *is* SourceAlpha, so what it computes is the alpha-named factor's value.
//
// Substituting is therefore exact rather than approximate, and it is what
// makes one BlendState mean one thing on both backends. Doom-3-era material
// systems are the reason it comes up at all: OpenGL's glBlendFunc sets one
// factor pair for colour and alpha together, so a material asking for
// (DST_COLOR, ZERO) is asking for that pair in the alpha channel too.
D3D12_BLEND toD3DAlphaBlendFactor(BlendFactor factor)
{
    switch (factor)
    {
        case BlendFactor::SourceColor:
            return D3D12_BLEND_SRC_ALPHA;
        case BlendFactor::OneMinusSourceColor:
            return D3D12_BLEND_INV_SRC_ALPHA;
        case BlendFactor::DestinationColor:
            return D3D12_BLEND_DEST_ALPHA;
        case BlendFactor::OneMinusDestinationColor:
            return D3D12_BLEND_INV_DEST_ALPHA;
        default:
            return toD3DBlendFactor(factor);
    }
}

D3D12_BLEND_OP toD3DBlendOperation(BlendOperation operation)
{
    switch (operation)
    {
        case BlendOperation::Add:
            return D3D12_BLEND_OP_ADD;
        case BlendOperation::Subtract:
            return D3D12_BLEND_OP_SUBTRACT;
        case BlendOperation::ReverseSubtract:
            return D3D12_BLEND_OP_REV_SUBTRACT;
        case BlendOperation::Min:
            return D3D12_BLEND_OP_MIN;
        case BlendOperation::Max:
            return D3D12_BLEND_OP_MAX;
    }

    return D3D12_BLEND_OP_ADD;
}

UINT8 toD3DWriteMask(const ColorWriteMask& mask)
{
    UINT8 value = 0;

    if (mask.red)
        value |= D3D12_COLOR_WRITE_ENABLE_RED;
    if (mask.green)
        value |= D3D12_COLOR_WRITE_ENABLE_GREEN;
    if (mask.blue)
        value |= D3D12_COLOR_WRITE_ENABLE_BLUE;
    if (mask.alpha)
        value |= D3D12_COLOR_WRITE_ENABLE_ALPHA;

    return value;
}

// One path for the named modes and for a written-out equation, because
// blendStateFor turns the first into the second. A preset's meaning is stated
// once, in the header, rather than once per backend.
D3D12_BLEND_DESC makeBlendDesc(const RenderPipelineDescriptor& from)
{
    D3D12_BLEND_DESC desc = {};
    auto& target = desc.RenderTarget[0];

    target.RenderTargetWriteMask = toD3DWriteMask(from.colorWriteMask);

    const auto blend = from.blend ? *from.blend : blendStateFor(from.blendMode);

    if (!blend.enabled)
        return desc;

    target.BlendEnable = TRUE;
    target.SrcBlend = toD3DBlendFactor(blend.sourceColor);
    target.DestBlend = toD3DBlendFactor(blend.destinationColor);
    target.BlendOp = toD3DBlendOperation(blend.colorOperation);
    target.SrcBlendAlpha = toD3DAlphaBlendFactor(blend.sourceAlpha);
    target.DestBlendAlpha = toD3DAlphaBlendFactor(blend.destinationAlpha);
    target.BlendOpAlpha = toD3DBlendOperation(blend.alphaOperation);

    return desc;
}

D3D12_STENCIL_OP toD3DStencilOp(StencilOp op)
{
    switch (op)
    {
        case StencilOp::Keep:
            return D3D12_STENCIL_OP_KEEP;
        case StencilOp::Zero:
            return D3D12_STENCIL_OP_ZERO;
        case StencilOp::Replace:
            return D3D12_STENCIL_OP_REPLACE;
        case StencilOp::IncrementClamp:
            return D3D12_STENCIL_OP_INCR_SAT;
        case StencilOp::DecrementClamp:
            return D3D12_STENCIL_OP_DECR_SAT;
        case StencilOp::Invert:
            return D3D12_STENCIL_OP_INVERT;
        case StencilOp::IncrementWrap:
            return D3D12_STENCIL_OP_INCR;
        case StencilOp::DecrementWrap:
            return D3D12_STENCIL_OP_DECR;
    }

    return D3D12_STENCIL_OP_KEEP;
}

// D3D12's INCR/DECR are the wrapping pair and INCR_SAT/DECR_SAT the clamping
// one, which is the opposite way round from how the names read - GL and Metal
// both spell the wrapping pair with the longer name. Worth stating because
// getting it backwards produces a shadow that is right until a pixel is inside
// 255 volumes, which no test would ever reach.
D3D12_DEPTH_STENCILOP_DESC toD3DStencilFace(const StencilFace& face)
{
    D3D12_DEPTH_STENCILOP_DESC desc = {};

    desc.StencilFailOp = toD3DStencilOp(face.stencilFail);
    desc.StencilDepthFailOp = toD3DStencilOp(face.depthFail);
    desc.StencilPassOp = toD3DStencilOp(face.pass);
    desc.StencilFunc = toD3DComparison(face.compare);

    return desc;
}

// The [0,1] depth range is shared by both APIs, so no convention flip is needed
// and the comparison means the same thing on each. The same holds for the
// stencil: an 8-bit unsigned plane, one reference value, one read mask and one
// write mask on both.
D3D12_DEPTH_STENCIL_DESC makeDepthStencilDesc(const RenderPipelineDescriptor& from)
{
    D3D12_DEPTH_STENCIL_DESC desc = {};

    if (from.depth)
    {
        desc.DepthEnable = TRUE;
        desc.DepthWriteMask = from.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL
                                              : D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.DepthFunc = toD3DComparison(from.depthCompare);
    }

    if (from.stencil)
    {
        desc.StencilEnable = TRUE;
        desc.StencilReadMask = from.stencilReadMask;
        desc.StencilWriteMask = from.stencilWriteMask;
        desc.FrontFace = toD3DStencilFace(from.stencilFront);
        desc.BackFace = toD3DStencilFace(from.stencilBack);
    }

    return desc;
}
} // namespace

struct RenderPipeline::Native
{
    Native(Device& device, const RenderPipelineDescriptor& descriptor)
        : context(getD3D12Context(device))
        , topology(descriptor.topology)
        , cullMode(descriptor.cullMode)
        , frontFace(descriptor.frontFace)
    {
        pipeline.topology = toD3DTopology(descriptor.topology);
        pipeline.strides = makeStrideTable(descriptor.vertexLayout);
        pipeline.depth = descriptor.depth;
        pipeline.stencil = descriptor.stencil;

        if (!context.isValid() || context.getRenderRootSignature() == nullptr
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
        desc.BlendState = makeBlendDesc(descriptor);
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState = makeRasterizerDesc(descriptor);
        desc.DepthStencilState = makeDepthStencilDesc(descriptor);
        desc.InputLayout.pInputElementDescs = inputLayout.data();
        desc.InputLayout.NumElements = static_cast<UINT>(inputLayout.size());
        desc.PrimitiveTopologyType = toTopologyType(descriptor.topology);
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = toDXGIFormat(descriptor.colorFormat);
        desc.DSVFormat = descriptor.depth || descriptor.stencil
                             ? depthAttachmentFormat(descriptor.stencil)
                             : DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = static_cast<UINT>(
            descriptor.sampleCount > 1 ? descriptor.sampleCount : 1);

        context.getDevice()->CreateGraphicsPipelineState(
            &desc, __uuidof(ID3D12PipelineState), pipeline.state.put_void());
    }

    // A command list holds a reference to every pipeline state it binds, so a
    // PSO built and dropped inside one frame — which is what constructing a
    // SpriteRenderer in render() does — has to outlive the recording rather
    // than release here. See D3D12Context::deferRelease.
    ~Native() { context.deferRelease(std::move(pipeline.state)); }

    D3D12Context& context;
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

// Both are already inside the PSO here. Reported anyway so the class reads the
// same on either backend, and so a caller can ask a pipeline what it does.
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
    // Depth state is baked into the PSO on D3D12; the handle exists for the
    // Metal backend, which binds it separately.
    return impl->pipeline.depth ? const_cast<D3D12Pipeline*>(&impl->pipeline)
                                : nullptr;
}
} // namespace eacp::GPU
