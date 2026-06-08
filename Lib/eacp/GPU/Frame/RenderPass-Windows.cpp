#include <eacp/Core/Utils/WinInclude.h>

#include "RenderPass.h"

#include "../Buffer/Buffer.h"
#include "../Pipeline/RenderPipeline.h"
#include "../Windows/D3DTypes.h"

#include <d3d11.h>

#include <memory>

// Windows/D3D11 backend. Records draw commands onto the immediate context the
// frame's beginPass handed over via the D3DEncoder. The encoder is owned here so
// it is freed when the pass goes out of scope.

namespace eacp::GPU
{
struct RenderPass::Native
{
    explicit Native(void* encoderHandle)
        : encoder(static_cast<D3DEncoder*>(encoderHandle))
    {
    }

    std::unique_ptr<D3DEncoder> encoder;
};

RenderPass::RenderPass(void* encoder)
    : impl(encoder)
{
}

RenderPass::~RenderPass()
{
    end();
}

void RenderPass::setPipeline(const RenderPipeline& pipeline)
{
    if (!impl->encoder)
        return;

    auto* state = static_cast<D3DPipeline*>(pipeline.nativeState());

    if (state == nullptr)
        return;

    auto* context = impl->encoder->context;

    context->IASetInputLayout(state->inputLayout.get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(state->vertexShader.get(), nullptr, 0);
    context->PSSetShader(state->pixelShader.get(), nullptr, 0);
    context->RSSetState(state->rasterizerState.get());

    // Null state restores D3D's default; with no depth view bound (the non-depth
    // path) that default has no buffer to test against, so it is a no-op there.
    context->OMSetDepthStencilState(state->depthStencilState.get(), 0);

    const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    context->OMSetBlendState(state->blendState.get(), blendFactor, 0xffffffff);

    impl->encoder->stride = state->stride;
}

void RenderPass::setVertexBuffer(const Buffer& buffer, int index)
{
    if (!impl->encoder)
        return;

    auto* vertexBuffer = static_cast<ID3D11Buffer*>(buffer.nativeBuffer());

    if (vertexBuffer == nullptr)
        return;

    UINT stride = impl->encoder->stride;
    UINT offset = 0;
    impl->encoder->context->IASetVertexBuffers(
        static_cast<UINT>(index), 1, &vertexBuffer, &stride, &offset);
}

void RenderPass::setVertexBytes(const void* data, std::size_t bytes, int slot)
{
    if (!impl->encoder)
        return;

    auto* context = impl->encoder->context;

    winrt::com_ptr<ID3D11Device> device;
    context->GetDevice(device.put());

    if (!device)
        return;

    // A fresh constant buffer per call keeps this self-contained; the context
    // AddRefs it on bind, so the local com_ptr can release. A Device-cached
    // dynamic buffer is a future optimisation.
    auto constantBuffer = makeConstantBuffer(device.get(), data, bytes);

    if (!constantBuffer)
        return;

    auto* rawBuffer = constantBuffer.get();
    context->VSSetConstantBuffers(static_cast<UINT>(slot), 1, &rawBuffer);
}

void RenderPass::draw(int vertexCount, int firstVertex)
{
    if (!impl->encoder)
        return;

    impl->encoder->context->Draw(static_cast<UINT>(vertexCount),
                                 static_cast<UINT>(firstVertex));
}

void RenderPass::end()
{
    // Commands are recorded straight onto the immediate context, so there is
    // nothing to flush; releasing the encoder marks the pass finished.
    impl->encoder.reset();
}
} // namespace eacp::GPU
