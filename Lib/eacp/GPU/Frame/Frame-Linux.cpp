#include "Frame.h"

#include "../Device/Device.h"
#include "../Linux/GPUBackend-Linux.h"

namespace eacp::GPU
{
namespace
{
// The pass carries the target size it was opened on, so the Frame does not
// have to know it to hand one out.
RenderPass makeRenderPass(std::unique_ptr<RenderPassBackend> pass)
{
    if (pass == nullptr)
        return RenderPass(nullptr);

    const auto width = pass->targetWidth();
    const auto height = pass->targetHeight();

    return RenderPass(pass.release(), width, height);
}
} // namespace

struct Frame::Native
{
    Native(Device& device, void* drawable)
        : backend(getDeviceBackend(device).makeFrame(device, drawable))
    {
    }

    Native(Device& device, const OffscreenTarget& target)
        : backend(getDeviceBackend(device).makeFrame(device, target))
    {
    }

    std::unique_ptr<FrameBackend> backend;
};

// The msaa and depth textures a drawable frame is handed elsewhere are the
// drawable's own companions here, so neither reaches the backend.
Frame::Frame(Device& device,
             void* drawable,
             void*,
             void*,
             float backingScaleToUse)
    : impl(device, drawable)
    , scale(backingScaleToUse)
{
    device.beginFrame();
    impl->backend->beginTiming();
}

Frame::Frame(Device& device, const OffscreenTarget& target, float backingScaleToUse)
    : impl(device, target)
    , scale(backingScaleToUse)
{
    device.beginFrame();
    impl->backend->beginTiming();
}

// Everything the frame recorded is submitted, presented and waited for as the
// backend goes.
Frame::~Frame() = default;

Graphics::Point Frame::pixelSize() const
{
    return impl->backend->pixelSize();
}

void Frame::flush()
{
    impl->backend->flush();
}

RenderPass Frame::beginPass(const RenderPassDescriptor& descriptor)
{
    return makeRenderPass(impl->backend->beginPass(descriptor));
}

RenderPass Frame::beginPass(const Texture& target,
                            const RenderPassDescriptor& descriptor)
{
    return makeRenderPass(impl->backend->beginPass(target, descriptor));
}

ComputePass Frame::beginCompute(std::string_view label, DispatchOrder order)
{
    return ComputePass(impl->backend->beginCompute(label, order).release(), order);
}

bool Frame::isValid() const
{
    return impl->backend->isValid();
}
} // namespace eacp::GPU
