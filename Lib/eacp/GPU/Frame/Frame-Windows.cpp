#include "Frame.h"

#include "../Device/Device.h"

// Windows/DirectX stub. See Device-Windows.cpp.

namespace eacp::GPU
{
struct Frame::Native
{
};

Frame::Frame(Device&, void*)
    : impl()
{
}

Frame::~Frame() = default;

RenderPass Frame::beginPass(const RenderPassDescriptor&)
{
    return RenderPass(nullptr);
}

bool Frame::isValid() const
{
    return false;
}
} // namespace eacp::GPU
