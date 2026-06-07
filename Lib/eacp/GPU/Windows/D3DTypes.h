#pragma once

#include <eacp/Core/Utils/WinInclude.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <winrt/base.h>

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

// Carries the immediate context (and the active pipeline stride) from a Frame's
// beginPass to the RenderPass that records into it. Heap-allocated by beginPass
// and owned by the RenderPass. Stands in for Metal's render command encoder.
struct D3DEncoder
{
    ID3D11DeviceContext* context = nullptr;
    UINT stride = 0;
};
} // namespace eacp::GPU
