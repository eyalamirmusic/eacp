#include "Frame.h"

#include "../Device/Device.h"
#include "../Vulkan/VulkanTypes.h"

// Linux/Vulkan placeholder. Stage 3 of the Linux plan replaces this file with
// the real thing: a recording held for the frame's lifetime, vkCmdBeginRendering
// per beginPass with the load and store ops DepthAction names, the full-target
// viewport and scissor set at pass begin, flush() as end-and-resubmit, and the
// off-screen constructor that waits instead of presenting. Stage 4 adds the
// swapchain half - the drawable, the present on destruction and the resolve.
//
// Until then a Frame is invalid and records nothing. Both constructors are
// still real constructors and both passes are still returned, because portable
// code above this - GPUView::render, the widget tier - is written against a
// Frame it was handed rather than against one it checked, and the passes it
// asks for drop their commands (RenderPass-Linux.cpp) rather than crashing.
//
// Device::beginFrame() is deliberately *not* called. It advances the frame
// counter that StreamingBuffers recycles on and starts the frame timer, and
// neither of those means anything for a frame that will not be recorded -
// counting one would make a loop of failed frames look like a renderer that is
// drawing. The real Frame calls it, as the other two backends do.

namespace eacp::GPU
{
struct Frame::Native
{
    Native(Device&, void*, void*, void*) {}
    Native(Device&, const OffscreenTarget&) {}
};

Frame::Frame(Device& device, void* drawable, void* msaaTexture, void* depthTexture)
    : impl(device, drawable, msaaTexture, depthTexture)
{
}

Frame::Frame(Device& device, const OffscreenTarget& target)
    : impl(device, target)
{
}

Frame::~Frame() = default;

void Frame::flush() {}

RenderPass Frame::beginPass(const RenderPassDescriptor&)
{
    return RenderPass(nullptr);
}

RenderPass Frame::beginPass(const Texture&, const RenderPassDescriptor&)
{
    return RenderPass(nullptr);
}

ComputePass Frame::beginCompute(std::string_view)
{
    // A compute pass on a Frame is the one thing here that could be recorded
    // for real - a dispatch needs no attachment. It is not, because the frame
    // it would be recorded onto does not exist: there is nothing to submit it
    // with and nothing to present. CommandBuffer::beginCompute is the path that
    // works today, and it is the one every compute test and PathBench take.
    return ComputePass(nullptr);
}

bool Frame::isValid() const
{
    return false;
}
} // namespace eacp::GPU
