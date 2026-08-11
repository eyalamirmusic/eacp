#include <eacp/Core/Utils/WinInclude.h>

#include "Frame.h"

#include "../Device/Device.h"
#include "../Windows/D3D12Types.h"

namespace eacp::GPU
{
struct Frame::Native
{
    Native(Device& deviceToUse,
           void* drawableHandle,
           void* msaaTextureHandle,
           void* depthTextureHandle)
        : device(&deviceToUse)
        , drawable(static_cast<D3D12Drawable*>(drawableHandle))
        , msaa(static_cast<D3D12MsaaTarget*>(msaaTextureHandle))
        , depth(static_cast<D3D12DepthTarget*>(depthTextureHandle))
    {
        if (deviceToUse.isValid() && drawable != nullptr)
            open(getD3D12Context().acquire());
    }

    // The colour target arrives as a D3D12Drawable with a null swapChain, so it
    // renders like a back buffer but is read back instead of presented.
    Native(Device& deviceToUse, const OffscreenTarget& target)
        : device(&deviceToUse)
        , drawable(static_cast<D3D12Drawable*>(target.colorTexture))
        , msaa(static_cast<D3D12MsaaTarget*>(target.msaaTexture))
        , depth(static_cast<D3D12DepthTarget*>(target.depthTexture))
        , offscreen(true)
    {
        if (deviceToUse.isValid() && drawable != nullptr
            && drawable->backBuffer != nullptr)
            open(getD3D12Context().acquire());
    }

    // Publishes the recording so CPU uploads join it; ~Frame withdraws it
    // before submitting, so no upload is handed a closed list.
    void open(CommandContext* commandsToUse)
    {
        commands = commandsToUse;
        getD3D12Context().setOpenRecording(commands);
    }

    // Must run after Device::beginFrame(), which gives the timer its slot -
    // hence the call from Frame's constructor body, not from Native's.
    void beginTiming()
    {
        if (commands != nullptr)
            device->frameTimer().beginRecording(commands->list.get());
    }

    void timePass(D3D12Encoder& encoder, std::string_view label)
    {
        auto& timer = device->frameTimer();
        const auto pass = timer.beginPass(label);

        if (pass < 0 || commands == nullptr)
            return;

        auto* heap = static_cast<ID3D12QueryHeap*>(timer.nativeSamples());

        if (heap == nullptr)
            return;

        commands->list->EndQuery(
            heap, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(pass * 2));

        encoder.queryHeap = heap;
        encoder.endQuery = pass * 2 + 1;
    }

    bool useMsaa() const { return msaa != nullptr && msaa->texture != nullptr; }

    // Root signature and heaps are fixed for every render pipeline, so binding
    // here frees the pass from call ordering.
    void bindRootState(D3D12Context& context, ID3D12GraphicsCommandList* list)
    {
        ID3D12DescriptorHeap* heaps[] = {context.getTextureHeap(),
                                         context.getSamplerHeap()};
        list->SetDescriptorHeaps(2, heaps);
        list->SetGraphicsRootSignature(context.getRenderRootSignature());

        // Resource Binding Tier 1 hardware silently drops a draw if any
        // descriptor table the root signature declares is unset, even ones the
        // shader never reads, so seed every SRV table with a null descriptor.
        const auto nullTexture = context.getNullTextureDescriptor();

        if (nullTexture.ptr == 0)
            return;

        for (auto slot = 0; slot < maxTextureSlots; ++slot)
            list->SetGraphicsRootDescriptorTable(renderTextureParam(slot),
                                                 nullTexture);
    }

    Device* device = nullptr;
    CommandContext* commands = nullptr;
    D3D12Drawable* drawable = nullptr;
    D3D12MsaaTarget* msaa = nullptr;
    D3D12DepthTarget* depth = nullptr;
    bool passBegun = false;
    bool offscreen = false;
};

Frame::Frame(Device& device, void* drawable, void* msaaTexture, void* depthTexture)
    : impl(device, drawable, msaaTexture, depthTexture)
{
    device.beginFrame();
    impl->beginTiming();
}

Frame::Frame(Device& device, const OffscreenTarget& target)
    : impl(device, target)
{
    device.beginFrame();
    impl->beginTiming();
}

Frame::~Frame()
{
    // Nothing may record onto this list from here on: what follows closes it.
    getD3D12Context().setOpenRecording(nullptr);

    if (impl->commands == nullptr || impl->drawable == nullptr)
        return;

    auto* list = impl->commands->list.get();
    auto* backBuffer = impl->drawable->backBuffer;

    if (impl->offscreen)
    {
        // Left in COPY_SOURCE for GPUView's read-back. The colour texture was
        // created in RESOLVE_DEST when multisampling, RENDER_TARGET otherwise.
        if (impl->useMsaa() && backBuffer != nullptr)
        {
            transition(list,
                       impl->msaa->texture,
                       D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_RESOLVE_SOURCE);

            list->ResolveSubresource(
                backBuffer, 0, impl->msaa->texture, 0, impl->msaa->format);

            transition(list,
                       backBuffer,
                       D3D12_RESOURCE_STATE_RESOLVE_DEST,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
        else if (backBuffer != nullptr)
        {
            transition(list,
                       backBuffer,
                       D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
        }

        auto& context = getD3D12Context();

        impl->device->frameTimer().endFrame(list);
        impl->device->frameTimer().noteSubmitted(context.submit(impl->commands));

        context.waitIdle();
        return;
    }

    if (impl->useMsaa() && backBuffer != nullptr)
    {
        // The MSAA target rests in RENDER_TARGET between frames and the back
        // buffer never left PRESENT, so both transition around the resolve only.
        transition(list,
                   impl->msaa->texture,
                   D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        transition(list,
                   backBuffer,
                   D3D12_RESOURCE_STATE_PRESENT,
                   D3D12_RESOURCE_STATE_RESOLVE_DEST);

        list->ResolveSubresource(
            backBuffer, 0, impl->msaa->texture, 0, impl->msaa->format);

        transition(list,
                   impl->msaa->texture,
                   D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        transition(list,
                   backBuffer,
                   D3D12_RESOURCE_STATE_RESOLVE_DEST,
                   D3D12_RESOURCE_STATE_PRESENT);
    }
    else if (impl->passBegun && backBuffer != nullptr)
    {
        transition(list,
                   backBuffer,
                   D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PRESENT);
    }

    impl->device->frameTimer().endFrame(list);
    impl->device->frameTimer().noteSubmitted(
        getD3D12Context().submit(impl->commands));

    if (impl->drawable->swapChain != nullptr)
        impl->drawable->swapChain->Present(1, 0);
}

RenderPass Frame::beginPass(const RenderPassDescriptor& descriptor)
{
    if (impl->commands == nullptr || impl->drawable == nullptr
        || impl->drawable->backBuffer == nullptr)
        return RenderPass(nullptr);

    auto& context = getD3D12Context();
    auto* list = impl->commands->list.get();

    impl->bindRootState(context, list);

    // Only a swapchain back buffer starts in PRESENT; the off-screen colour
    // texture is created in RENDER_TARGET already.
    if (!impl->useMsaa() && !impl->passBegun && !impl->offscreen)
        transition(list,
                   impl->drawable->backBuffer,
                   D3D12_RESOURCE_STATE_PRESENT,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);

    impl->passBegun = true;

    auto target =
        impl->useMsaa() ? impl->msaa->view : impl->drawable->backBufferView;
    auto hasDepth = impl->depth != nullptr && impl->depth->view.ptr != 0;

    list->OMSetRenderTargets(
        1, &target, FALSE, hasDepth ? &impl->depth->view : nullptr);

    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(impl->drawable->width);
    viewport.Height = static_cast<float>(impl->drawable->height);
    viewport.MaxDepth = 1.0f;
    list->RSSetViewports(1, &viewport);

    // D3D12 has no default scissor; an unset one clips everything away.
    D3D12_RECT scissor = {0,
                          0,
                          static_cast<LONG>(impl->drawable->width),
                          static_cast<LONG>(impl->drawable->height)};
    list->RSSetScissorRects(1, &scissor);

    if (descriptor.clear)
    {
        const auto& color = descriptor.clearColor;
        const float clearColor[4] = {color.r, color.g, color.b, color.a};
        list->ClearRenderTargetView(target, clearColor, 0, nullptr);
    }

    // Cleared to the far plane, matching Metal's unconditional depth clear.
    if (hasDepth)
        list->ClearDepthStencilView(
            impl->depth->view, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    auto* encoder = new D3D12Encoder {impl->commands, {}};
    impl->timePass(*encoder, descriptor.label);

    return RenderPass(encoder,
                      static_cast<int>(impl->drawable->width),
                      static_cast<int>(impl->drawable->height));
}

// Leaves passBegun alone: it tracks the back buffer leaving PRESENT, and a
// texture-only frame must not transition one back. The texture stays in
// RENDER_TARGET until RenderPass::setFragmentTexture samples it.
RenderPass Frame::beginPass(const Texture& target,
                            const RenderPassDescriptor& descriptor)
{
    auto* data = static_cast<D3D12TextureData*>(target.nativeTexture());

    if (impl->commands == nullptr || data == nullptr || !target.isRenderTarget())
        return RenderPass(nullptr);

    auto& context = getD3D12Context();
    auto* list = impl->commands->list.get();

    impl->bindRootState(context, list);
    transitionTextureForUse(list, *data, D3D12_RESOURCE_STATE_RENDER_TARGET);

    auto hasDepth = data->hasDepth();
    list->OMSetRenderTargets(1, &data->rtv, FALSE, hasDepth ? &data->dsv : nullptr);

    auto width = target.width();
    auto height = target.height();

    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;
    list->RSSetViewports(1, &viewport);

    // D3D12 has no default scissor; an unset one clips everything away.
    D3D12_RECT scissor = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    list->RSSetScissorRects(1, &scissor);

    if (descriptor.clear)
    {
        const auto& color = descriptor.clearColor;
        const float clearColor[4] = {color.r, color.g, color.b, color.a};
        list->ClearRenderTargetView(data->rtv, clearColor, 0, nullptr);
    }

    // Cleared to the far plane, matching Metal's unconditional depth clear.
    if (hasDepth)
        list->ClearDepthStencilView(
            data->dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    auto* encoder = new D3D12Encoder {impl->commands, {}};
    impl->timePass(*encoder, descriptor.label);

    return RenderPass(encoder, width, height);
}

// Graphics and compute root signatures occupy separate command-list slots, so
// binding one here leaves the render state beginPass set alone.
ComputePass Frame::beginCompute(std::string_view label)
{
    if (impl->commands == nullptr)
        return ComputePass(nullptr);

    bindComputeRootState(getD3D12Context(), impl->commands->list.get());

    auto* encoder = new D3D12ComputeEncoder {impl->commands};

    auto& timer = impl->device->frameTimer();
    const auto pass = timer.beginPass(label);

    if (pass >= 0)
    {
        if (auto* heap = static_cast<ID3D12QueryHeap*>(timer.nativeSamples()))
        {
            impl->commands->list->EndQuery(
                heap, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(pass * 2));

            encoder->queryHeap = heap;
            encoder->endQuery = pass * 2 + 1;
        }
    }

    return ComputePass(encoder);
}

bool Frame::isValid() const
{
    return impl->commands != nullptr && impl->drawable != nullptr
           && impl->drawable->backBuffer != nullptr;
}
} // namespace eacp::GPU
