#pragma once

#include <eacp/Core/Utils/WinInclude.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <winrt/base.h>

#include <cstddef>
#include <cstring>
#include <vector>

// Internal shared types for the Windows/D3D11 GPU backend. The public GPU
// classes expose opaque void* handles (nativeBuffer/nativeLibrary/nativeState/
// ...); these structs are what those handles point to, so the separate
// translation units (Shader, Pipeline, Frame, RenderPass, View) agree on the
// concrete layout without leaking D3D types into the public headers. Not part of
// GPU.h.

namespace eacp::GPU
{
// Result of compiling a ShaderSource: the vertex and pixel shaders plus the
// vertex bytecode the pipeline needs to validate its input layout. Pointed to by
// ShaderLibrary::nativeLibrary().
struct D3DShaderProgram
{
    winrt::com_ptr<ID3D11VertexShader> vertexShader;
    winrt::com_ptr<ID3D11PixelShader> pixelShader;
    winrt::com_ptr<ID3D11ComputeShader> computeShader;
    winrt::com_ptr<ID3DBlob> vertexBytecode;
};

// A render pipeline bundle. D3D11 has no single pipeline-state object, so this
// gathers the shaders, input layout and fixed-function state a render pass binds
// together. Pointed to by RenderPipeline::nativeState().
struct D3DPipeline
{
    winrt::com_ptr<ID3D11VertexShader> vertexShader;
    winrt::com_ptr<ID3D11PixelShader> pixelShader;
    winrt::com_ptr<ID3D11InputLayout> inputLayout;
    winrt::com_ptr<ID3D11RasterizerState> rasterizerState;
    winrt::com_ptr<ID3D11BlendState> blendState; // null = opaque, no blending
    winrt::com_ptr<ID3D11DepthStencilState> depthStencilState; // null = no depth
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    UINT stride = 0;
};

// The frame's color target. All members are owned by GPUView and stay valid for
// the lifetime of the Frame. Pointed to by the drawable handle passed to Frame.
struct D3DDrawable
{
    IDXGISwapChain1* swapChain = nullptr;
    ID3D11Texture2D* backBuffer = nullptr;
    ID3D11RenderTargetView* backBufferView = nullptr;
    UINT width = 0;
    UINT height = 0;
};

// Optional multisample target. When present the pass renders into it and the
// frame resolves it into the swapchain back buffer. Owned by GPUView. Pointed to
// by the msaaTexture handle passed to Frame.
struct D3DMsaaTarget
{
    ID3D11Texture2D* texture = nullptr;
    ID3D11RenderTargetView* view = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
};

// Optional depth buffer. When present the pass binds it alongside the colour
// target for depth testing and clears it at the start of the frame. Owned by
// GPUView. Pointed to by the depthTexture handle passed to Frame.
struct D3DDepthTarget
{
    ID3D11Texture2D* texture = nullptr;
    ID3D11DepthStencilView* view = nullptr;
};

// Carries the immediate context (and the active pipeline stride) from a Frame's
// beginPass to the RenderPass that records into it. Heap-allocated by beginPass
// and owned by the RenderPass. Stands in for Metal's render command encoder.
struct D3DEncoder
{
    ID3D11DeviceContext* context = nullptr;
    UINT stride = 0;
};

// Carries the immediate context from CommandBuffer::beginCompute to the
// ComputePass that records onto it. The compute sibling of D3DEncoder, without
// the render-only stride. Heap-allocated by beginCompute, owned by the pass.
struct D3DComputeEncoder
{
    ID3D11DeviceContext* context = nullptr;
};

// Builds a constant buffer holding a uniform block, padded to the 16-byte
// multiple D3D11 requires. Shared by the render and compute passes' setBytes so
// the padded-upload logic lives in one place; each pass binds the result to its
// own stage (VSSetConstantBuffers / CSSetConstantBuffers).
inline winrt::com_ptr<ID3D11Buffer>
    makeConstantBuffer(ID3D11Device* device, const void* data, std::size_t bytes)
{
    auto paddedSize =
        static_cast<UINT>((bytes + 15) & ~static_cast<std::size_t>(15));
    auto padded = std::vector<unsigned char>(paddedSize, 0);
    std::memcpy(padded.data(), data, bytes);

    D3D11_BUFFER_DESC descriptor = {};
    descriptor.ByteWidth = paddedSize;
    descriptor.Usage = D3D11_USAGE_DEFAULT;
    descriptor.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    D3D11_SUBRESOURCE_DATA initialData = {};
    initialData.pSysMem = padded.data();

    winrt::com_ptr<ID3D11Buffer> buffer;
    device->CreateBuffer(&descriptor, &initialData, buffer.put());
    return buffer;
}
} // namespace eacp::GPU
