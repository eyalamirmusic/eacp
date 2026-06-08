#include <eacp/Core/Utils/WinInclude.h>

#include "ComputePass.h"

#include "../Buffer/Buffer.h"
#include "../Pipeline/ComputePipeline.h"
#include "../Windows/D3DTypes.h"

#include <d3d11.h>

#include <memory>

// Windows/D3D11 backend. Records onto the immediate context the command
// buffer's beginCompute handed over via the D3DComputeEncoder. Compute I/O uses
// views: a read-only input binds as a shader-resource view, a read-write output
// as an unordered-access view, and uniforms as a constant buffer.

namespace eacp::GPU
{
struct ComputePass::Native
{
    explicit Native(void* encoderHandle)
        : encoder(static_cast<D3DComputeEncoder*>(encoderHandle))
    {
    }

    std::unique_ptr<D3DComputeEncoder> encoder;
};

ComputePass::ComputePass(void* encoder)
    : impl(encoder)
{
}

ComputePass::~ComputePass()
{
    end();
}

void ComputePass::setPipeline(const ComputePipeline& pipeline)
{
    if (!impl->encoder)
        return;

    auto* shader = static_cast<ID3D11ComputeShader*>(pipeline.nativeState());
    impl->encoder->context->CSSetShader(shader, nullptr, 0);
}

void ComputePass::setInputBuffer(const Buffer& buffer, int slot)
{
    if (!impl->encoder)
        return;

    auto* view = static_cast<ID3D11ShaderResourceView*>(buffer.nativeReadView());
    impl->encoder->context->CSSetShaderResources(static_cast<UINT>(slot), 1, &view);
}

void ComputePass::setOutputBuffer(const Buffer& buffer, int slot)
{
    if (!impl->encoder)
        return;

    auto* view = static_cast<ID3D11UnorderedAccessView*>(buffer.nativeWriteView());
    impl->encoder->context->CSSetUnorderedAccessViews(
        static_cast<UINT>(slot), 1, &view, nullptr);
}

void ComputePass::setBytes(const void* data, std::size_t bytes, int slot)
{
    if (!impl->encoder)
        return;

    auto* context = impl->encoder->context;

    winrt::com_ptr<ID3D11Device> device;
    context->GetDevice(device.put());

    if (!device)
        return;

    auto constantBuffer = makeConstantBuffer(device.get(), data, bytes);

    if (!constantBuffer)
        return;

    auto* rawBuffer = constantBuffer.get();
    context->CSSetConstantBuffers(static_cast<UINT>(slot), 1, &rawBuffer);
}

void ComputePass::dispatch(int count)
{
    if (!impl->encoder || count <= 0)
        return;

    auto groups =
        (static_cast<UINT>(count) + threadGroupWidth - 1) / threadGroupWidth;
    impl->encoder->context->Dispatch(groups, 1, 1);
}

void ComputePass::end()
{
    if (!impl->encoder)
        return;

    // Unbind the output views so the buffer can be read back (and to quiet the
    // debug layer's bound-as-UAV hazard warning) before the pass is reused.
    auto* context = impl->encoder->context;
    ID3D11UnorderedAccessView* nullViews[8] = {};
    context->CSSetUnorderedAccessViews(0, 8, nullViews, nullptr);
    context->CSSetShader(nullptr, nullptr, 0);

    impl->encoder.reset();
}
} // namespace eacp::GPU
