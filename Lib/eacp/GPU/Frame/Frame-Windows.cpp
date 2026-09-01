#include <eacp/Core/Utils/WinInclude.h>

#include "Frame.h"

#include "../Device/Device.h"
#include "../Windows/D3D12Types.h"

// Windows/D3D12 backend. The drawable and msaaTexture handles point at the
// D3D12Drawable / D3D12MsaaTarget owned by GPUView. The frame owns one
// CommandContext recording: beginPass transitions and clears the target, the
// pass records draws onto it, and the destructor resolves any multisample
// target, returns the back buffer to PRESENT, executes the recording and
// presents the swapchain (mirroring the Metal frame's present-on-destroy).

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
            open(context().acquire());
    }

    // Off-screen snapshot target (GPUView::renderNativeContent). The colour
    // target is passed as a D3D12Drawable with a null swapChain, so beginPass
    // renders into it exactly like a back buffer but the destructor resolves and
    // hands it back for read-back instead of presenting.
    Native(Device& deviceToUse, const OffscreenTarget& target)
        : device(&deviceToUse)
        , drawable(static_cast<D3D12Drawable*>(target.colorTexture))
        , msaa(static_cast<D3D12MsaaTarget*>(target.msaaTexture))
        , depth(static_cast<D3D12DepthTarget*>(target.depthTexture))
        , offscreen(true)
    {
        if (deviceToUse.isValid() && drawable != nullptr
            && drawable->backBuffer != nullptr)
            open(context().acquire());
    }

    // Takes the recording and publishes it as the one a CPU upload may record
    // onto, for as long as this frame is the thing recording. Withdrawn in
    // ~Frame before anything is submitted, so an upload can never be handed a
    // list that has already been closed.
    void open(CommandContext* commandsToUse)
    {
        commands = commandsToUse;
        context().setOpenRecording(commands);
    }

    // The frame belongs to its Device's context: the recording came out of that
    // queue's pool and is submitted back to it.
    D3D12Context& context() const { return getD3D12Context(*device); }

    // The frame's opening timestamp, which has to be the first thing on the
    // list for the total to mean the frame. Metal takes this off the command
    // buffer afterwards and records nothing here.
    //
    // Called from Frame's constructor body rather than from this one, and the
    // order is the whole point: Device::beginFrame() is what gives the timer
    // the slot to write into, and it runs after every member is built. Recorded
    // from here it would go into the previous frame's query heap.
    void beginTiming()
    {
        if (commands != nullptr)
            device->frameTimer().beginRecording(commands->list.get());
    }

    // Opens a timed pass on the list and hands the encoder what it needs to
    // close it when the pass ends.
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

    // The state every render pass needs bound before it can draw, whether it
    // targets the back buffer or an app-owned texture. Shared so the two paths
    // cannot drift apart on the platform I cannot test.
    void bindRootState(D3D12Context& context, ID3D12GraphicsCommandList* list)
    {
        // The root signature and heaps are fixed for every render pipeline, so
        // binding them here frees the pass from caring about call ordering.
        ID3D12DescriptorHeap* heaps[] = {context.getTextureHeap(),
                                         context.getSamplerHeap()};
        list->SetDescriptorHeaps(2, heaps);
        list->SetGraphicsRootSignature(context.getRenderRootSignature());

        // Resource Binding Tier 1 hardware requires *every* descriptor table the
        // root signature declares to be populated before a draw, even the ones
        // the shader never reads — an unset table drops the draw entirely rather
        // than failing loudly. The signature is shared and declares
        // maxTextureSlots of them, while a typical shader binds one, so the rest
        // are seeded with the null descriptor here; setFragmentTexture
        // overwrites the slots that carry a real texture.
        //
        // Tier 2+ hardware ignores unset tables, which is why this only ever
        // showed up on an Arm laptop: no text drew, and nothing was logged
        // without the D3D12 validation layer installed.
        //
        // Only the SRV tables need this. The root signature declares no sampler
        // tables at all any more - samplers are static samplers baked into it,
        // picked by the register the shader emitted its sampler at. See
        // TextureSampling.
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
    // Only where one was published — a frame on a device-less view never
    // acquired a recording and has no context to withdraw it from.
    if (impl->commands != nullptr)
        impl->context().setOpenRecording(nullptr);

    if (impl->commands == nullptr || impl->drawable == nullptr)
        return;

    auto* list = impl->commands->list.get();
    auto* backBuffer = impl->drawable->backBuffer;

    if (impl->offscreen)
    {
        // Off-screen snapshot: resolve any MSAA into the colour texture, leave it
        // in COPY_SOURCE for GPUView's read-back, then run the GPU to completion
        // (no swapchain to present). The colour texture was created in
        // RESOLVE_DEST when multisampling and RENDER_TARGET otherwise.
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

        auto& context = impl->context();

        impl->device->frameTimer().endFrame(list);
        impl->device->frameTimer().noteSubmitted(context.submit(impl->commands));

        context.waitIdle();
        return;
    }

    if (impl->useMsaa() && backBuffer != nullptr)
    {
        // The MSAA target lives in RENDER_TARGET state between frames; the
        // back buffer never left PRESENT (the pass rendered into the MSAA
        // target), so both transition just around the resolve.
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

    // The closing timestamp and the query resolve go onto the list while it is
    // still open; the fence value they will be read against only exists once it
    // has been executed.
    impl->device->frameTimer().endFrame(list);
    impl->device->frameTimer().noteSubmitted(impl->context().submit(impl->commands));

    if (impl->drawable->swapChain != nullptr)
        impl->drawable->swapChain->Present(1, 0);
}

// The submit half of the destructor, without the present and without the
// resolve: the recording goes, the frame stays and takes a fresh one. Resource
// states live on the resources rather than on the list, so nothing the passes
// so far established is lost, and the next beginPass binds the root state onto
// the new list the way it binds it onto every list.
void Frame::flush()
{
    if (impl->commands == nullptr)
        return;

    auto& context = impl->context();

    // Withdrawn before the submit, exactly as ~Frame withdraws it: an upload
    // must never be handed a list that is about to be closed.
    context.setOpenRecording(nullptr);
    context.submit(impl->commands);

    // The timer is not told, and needs no telling: its opening timestamp is
    // already on the queue and its closing one goes onto whichever list is open
    // when the frame ends. Both are queries on one heap, executed in order, so
    // unlike Metal's the total still means the whole frame.
    impl->open(context.acquire());
}

RenderPass Frame::beginPass(const RenderPassDescriptor& descriptor)
{
    if (impl->commands == nullptr || impl->drawable == nullptr
        || impl->drawable->backBuffer == nullptr)
        return RenderPass(nullptr);

    auto& context = impl->context();
    auto* list = impl->commands->list.get();

    impl->bindRootState(context, list);

    // The off-screen colour texture is created already in RENDER_TARGET; only a
    // swapchain back buffer starts in PRESENT and needs promoting here.
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

    // The stencil reference is command-list state here and encoder state on
    // Metal, so it survives a pass on this backend and does not on that one.
    // Reset to Metal's own default at every pass, or a pass that sets a
    // reference silently lends it to the next pass on the same frame and the
    // two backends draw differently. Tests/GPU/StencilTests.cpp pins it.
    list->OMSetStencilRef(0);

    if (descriptor.clear)
    {
        const auto& color = descriptor.clearColor;
        const float clearColor[4] = {color.r, color.g, color.b, color.a};
        list->ClearRenderTargetView(target, clearColor, 0, nullptr);
    }

    // Depth is cleared to the far plane (1.0) whenever a depth buffer is
    // bound, matching the Metal pass's unconditional depth clear - and the
    // stencil plane with it, to the value the descriptor names, whenever the
    // buffer has one.
    if (hasDepth)
        list->ClearDepthStencilView(impl->depth->view,
                                    depthClearFlags(impl->depth->stencil),
                                    1.0f,
                                    descriptor.clearStencil,
                                    0,
                                    nullptr);

    auto* encoder = new D3D12Encoder {impl->commands, {}};
    impl->timePass(*encoder, descriptor.label);

    // The pass carries the target's pixel size so it can clamp scissor rects.
    return RenderPass(encoder,
                      static_cast<int>(impl->drawable->width),
                      static_cast<int>(impl->drawable->height));
}

// Rendering into an app-owned texture: one attachment and no resolve. Depth is
// the target's own, from TextureDescriptor::depth, and rests in DEPTH_WRITE for
// its lifetime, so it costs a clear here and no barrier.
//
// Deliberately does not touch passBegun, which records whether the *back
// buffer* was moved out of PRESENT - a frame whose only passes were into
// textures must not have one transitioned back on the way out.
//
// The texture is moved into RENDER_TARGET here and moved back the moment
// something samples it, in RenderPass::setFragmentTexture, rather than at the
// end of the pass: a target written by one pass and read by the next then costs
// exactly the two barriers it needs, and one written and never read costs one.
RenderPass Frame::beginPass(const Texture& target,
                            const RenderPassDescriptor& descriptor)
{
    auto* data = static_cast<D3D12TextureData*>(target.nativeTexture());

    if (impl->commands == nullptr || data == nullptr || !target.isRenderTarget())
        return RenderPass(nullptr);

    auto& context = impl->context();
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

    // The stencil reference is command-list state here and encoder state on
    // Metal, so it survives a pass on this backend and does not on that one.
    // Reset to Metal's own default at every pass, or a pass that sets a
    // reference silently lends it to the next pass on the same frame and the
    // two backends draw differently. Tests/GPU/StencilTests.cpp pins it.
    list->OMSetStencilRef(0);

    if (descriptor.clear)
    {
        const auto& color = descriptor.clearColor;
        const float clearColor[4] = {color.r, color.g, color.b, color.a};
        list->ClearRenderTargetView(data->rtv, clearColor, 0, nullptr);
    }

    // Cleared to the far plane whenever there is one, matching both the
    // drawable pass here and the Metal pass's unconditional depth clear.
    if (hasDepth)
        list->ClearDepthStencilView(data->dsv,
                                    depthClearFlags(data->hasStencil()),
                                    1.0f,
                                    descriptor.clearStencil,
                                    0,
                                    nullptr);

    auto* encoder = new D3D12Encoder {impl->commands, {}};
    impl->timePass(*encoder, descriptor.label);

    return RenderPass(encoder, width, height);
}

// A compute pass on the frame's own recording. The graphics and compute root
// signatures occupy separate slots on a command list, so binding one here does
// not disturb the render state beginPass set - the two kinds of pass interleave
// on one list without either having to restore anything.
//
// A buffer this pass writes as a UAV and a later pass binds as vertex data is
// transitioned by RenderPass::setVertexBuffer: same recording, so the per-
// recording state tracking sees the UAV state and emits the barrier.
ComputePass Frame::beginCompute(std::string_view label)
{
    if (impl->commands == nullptr)
        return ComputePass(nullptr);

    bindComputeRootState(impl->context(), impl->commands->list.get());

    auto* encoder = new D3D12ComputeEncoder {impl->commands};

    // Same two queries as a render pass, on the same list, in the order the
    // work was recorded — a compute pass is not special here.
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
