#include <eacp/Core/Utils/WinInclude.h>

#include "RenderPipeline.h"

#include "../Device/Device.h"
#include "../Shader/ShaderLibrary.h"
#include "../Windows/D3DTypes.h"

#include <d3d11.h>

#include <vector>

#include <winrt/base.h>

// Windows/D3D11 backend. Assembles the shaders compiled by ShaderLibrary with an
// input layout and fixed-function state into the D3DPipeline a render pass binds.

namespace eacp::GPU
{
namespace
{
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

// Vertex attributes are matched to the HLSL input by a TEXCOORD semantic indexed
// by attribute position, mirroring Metal's [[attribute(n)]] index-based binding.
winrt::com_ptr<ID3D11InputLayout> makeInputLayout(ID3D11Device* device,
                                                  const VertexLayout& layout,
                                                  ID3DBlob* vertexBytecode)
{
    if (layout.attributes.empty() || vertexBytecode == nullptr)
        return nullptr;

    auto elements = std::vector<D3D11_INPUT_ELEMENT_DESC>();

    for (auto i = 0; i < layout.attributes.size(); ++i)
    {
        const auto& attribute = layout.attributes[i];

        D3D11_INPUT_ELEMENT_DESC element = {};
        element.SemanticName = "TEXCOORD";
        element.SemanticIndex = static_cast<UINT>(i);
        element.Format = toDXGIFormat(attribute.format);
        element.InputSlot = static_cast<UINT>(attribute.bufferIndex);
        element.AlignedByteOffset = static_cast<UINT>(attribute.offset);
        element.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;

        elements.push_back(element);
    }

    winrt::com_ptr<ID3D11InputLayout> inputLayout;
    device->CreateInputLayout(elements.data(),
                              static_cast<UINT>(elements.size()),
                              vertexBytecode->GetBufferPointer(),
                              vertexBytecode->GetBufferSize(),
                              inputLayout.put());

    return inputLayout;
}

winrt::com_ptr<ID3D11RasterizerState> makeRasterizerState(ID3D11Device* device,
                                                          int sampleCount)
{
    D3D11_RASTERIZER_DESC descriptor = {};
    descriptor.FillMode = D3D11_FILL_SOLID;
    // Match Metal's default of no face culling.
    descriptor.CullMode = D3D11_CULL_NONE;
    descriptor.DepthClipEnable = TRUE;
    descriptor.MultisampleEnable = sampleCount > 1 ? TRUE : FALSE;

    winrt::com_ptr<ID3D11RasterizerState> state;
    device->CreateRasterizerState(&descriptor, state.put());
    return state;
}

winrt::com_ptr<ID3D11BlendState> makeBlendState(ID3D11Device* device)
{
    D3D11_BLEND_DESC descriptor = {};
    auto& target = descriptor.RenderTarget[0];
    target.BlendEnable = TRUE;
    target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOp = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
    target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    winrt::com_ptr<ID3D11BlendState> state;
    device->CreateBlendState(&descriptor, state.put());
    return state;
}
} // namespace

struct RenderPipeline::Native
{
    Native(Device& device, const RenderPipelineDescriptor& descriptor)
    {
        auto* d3dDevice = static_cast<ID3D11Device*>(device.nativeDevice());

        if (d3dDevice == nullptr || descriptor.library == nullptr)
            return;

        auto* program =
            static_cast<D3DShaderProgram*>(descriptor.library->nativeLibrary());

        if (program == nullptr || !program->vertexShader || !program->pixelShader)
            return;

        pipeline.vertexShader = program->vertexShader;
        pipeline.pixelShader = program->pixelShader;
        pipeline.inputLayout = makeInputLayout(
            d3dDevice, descriptor.vertexLayout, program->vertexBytecode.get());
        pipeline.rasterizerState =
            makeRasterizerState(d3dDevice, descriptor.sampleCount);
        pipeline.stride = static_cast<UINT>(descriptor.vertexLayout.stride);

        if (descriptor.blending)
            pipeline.blendState = makeBlendState(d3dDevice);
    }

    D3DPipeline pipeline;
};

RenderPipeline::RenderPipeline(Device& device,
                               const RenderPipelineDescriptor& descriptor)
    : impl(device, descriptor)
{
}

bool RenderPipeline::isValid() const
{
    return impl->pipeline.vertexShader != nullptr
           && impl->pipeline.pixelShader != nullptr;
}

void* RenderPipeline::nativeState() const
{
    return const_cast<D3DPipeline*>(&impl->pipeline);
}

void* RenderPipeline::nativeDepthState() const
{
    // Depth buffering is implemented on the Metal backend only for now.
    return nullptr;
}
} // namespace eacp::GPU
