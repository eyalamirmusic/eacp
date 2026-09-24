#include "Device.h"

#include <eacp/Core/Threads/ThreadUtils.h>

#include <cassert>

// Portable Device members. The platform backends (Device-macOS.mm /
// Device-Windows.cpp) own construction and the native handles; anything that
// only builds on the public API lives here so it compiles once for every
// platform.

namespace eacp::GPU
{
bool Device::ThreadOwner::isCurrent() const
{
    return followsMainThread ? Threads::isMainThread()
                             : std::this_thread::get_id() == id;
}

// The thread rule the class comment states, checked in one place for all three
// backends. Out of line rather than inline so that <cassert> and the
// main-thread query stay out of a header most of the module includes.
//
// The whole body is behind NDEBUG rather than only the assert, because the
// question itself is not free: for Device::shared() it is Threads::isMainThread,
// which is an out-of-line [NSThread isMainThread] on Apple, and this sits on
// every buffer creation, read, update and submission. A release build is left
// with an empty function.
void Device::assertOwningThread() const
{
#ifndef NDEBUG
    assert(threadOwner().isCurrent()
           && "eacp: a GPU::Device and everything made from it belong to the "
              "thread that made it - give each thread its own");
#endif
}

Texture Device::makeTexture(const Graphics::Image& image)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = image.width();
    descriptor.height = image.height();
    descriptor.format = TextureFormat::RGBA8Unorm;

    return makeTexture(descriptor, image.pixels().data());
}

void Device::beginFrame()
{
    // Every Frame on every backend starts here, which makes this the one place
    // that covers the render and compute passes a Frame goes on to open.
    assertOwningThread();

    ++frameCount;

    // The timer takes its slot from the counter, the same way StreamingBuffers
    // takes its pool from it - one advance, driven by whoever built the Frame,
    // and nothing for either of them to be told separately.
    timer.beginFrame(frameCount, *this);
}
} // namespace eacp::GPU
