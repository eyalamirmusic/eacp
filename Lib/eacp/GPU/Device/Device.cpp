#include "Device.h"

#include "../Frame/Frame.h"

#include <eacp/Core/Threads/ThreadUtils.h>
#include <eacp/Graphics/Image/Image.h>

#include <cassert>
#include <mutex>
#include <unordered_map>

// Portable Device members. The platform backends (Device-macOS.mm /
// Device-Windows.cpp) own construction and the native handles; anything that
// only builds on the public API lives here so it compiles once for every
// platform.

namespace eacp::GPU
{
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
    const auto onOwningThread = mainThreadOwned
                                    ? Threads::isMainThread()
                                    : Threads::currentThreadId() == owningThread;

    assert(onOwningThread
           && "eacp: a GPU::Device and everything made from it belong to the "
              "thread that made it - give each thread its own");
#endif
}

Buffer Device::makeBuffer(const void* data,
                          std::int64_t bytes,
                          BufferUsage usage,
                          BufferStorage storage)
{
    assertOwningThread();

    return {*this, data, bytes, usage, storage};
}

Buffer Device::makeBuffer(std::int64_t bytes, BufferUsage usage)
{
    assertOwningThread();

    return {*this, nullptr, bytes, usage};
}

Buffer Device::makeBufferOverMemory(ExternalMemory memory, BufferUsage usage)
{
    assertOwningThread();

    return {*this, std::move(memory), usage};
}

Texture Device::makeTexture(const TextureDescriptor& descriptor, const void* pixels)
{
    return {*this, descriptor, pixels};
}

Texture Device::makeTextureFromSubresources(
    const TextureDescriptor& descriptor,
    std::span<const TextureSubresource> subresources)
{
    return {*this, descriptor, subresources};
}

Texture Device::wrapPixelBuffer(void* nativePixelBuffer)
{
    return {*this, nativePixelBuffer};
}

struct Device::SamplerCache
{
    std::mutex mutex;
    std::unordered_map<SamplerDescriptor, Sampler, SamplerDescriptorHash> samplers;
};

std::shared_ptr<Device::SamplerCache> Device::makeSamplerCache()
{
    return std::make_shared<SamplerCache>();
}

namespace
{
constexpr TextureSampling samplingConfigurationsInOrder[] = {
    {TextureFilter::Nearest, TextureAddressMode::Clamp},
    {TextureFilter::Nearest, TextureAddressMode::Repeat},
    {TextureFilter::Linear, TextureAddressMode::Clamp},
    {TextureFilter::Linear, TextureAddressMode::Repeat},
};
} // namespace

Sampler Device::makeSampler(const SamplerDescriptor& descriptor)
{
    if (!isValid())
        return {};

    auto lock = std::scoped_lock(samplers->mutex);
    auto& cache = samplers->samplers;

    if (auto found = cache.find(descriptor); found != cache.end())
        return found->second;

    auto native = static_cast<void*>(nullptr);

    for (const auto& sampling: samplingConfigurationsInOrder)
        if (SamplerDescriptor::from(sampling) == descriptor)
            native = nativeSampler(sampling);

    if (native == nullptr)
        native = createNativeSampler(descriptor);

    if (native == nullptr)
        return {};

    auto sampler = Sampler {native, descriptor};
    cache.emplace(descriptor, sampler);
    return sampler;
}

ShaderLibrary Device::makeShaderLibrary(const ShaderSource& source)
{
    return {*this, source};
}

RenderPipeline Device::makeRenderPipeline(const RenderPipelineDescriptor& descriptor)
{
    return {*this, descriptor};
}

ComputePipeline Device::makeComputePipeline(const ShaderLibrary& library)
{
    return {*this, library};
}

CommandBuffer Device::makeCommandBuffer()
{
    assertOwningThread();

    return CommandBuffer {*this};
}

const FrameTimings& Device::lastFrameTimings() const
{
    return timer.lastTimings();
}

std::uint64_t Device::frameIndex() const
{
    return frameCount;
}

bool Device::isFrameOpen() const
{
    return openFrame != nullptr;
}

FrameTimer& Device::frameTimer()
{
    return timer;
}

int Device::buffersCreated() const
{
    return bufferCount;
}

void Device::noteBufferCreated()
{
    ++bufferCount;
}

void Device::followMainThread()
{
    mainThreadOwned = true;
}

bool Device::supportsPassTimings() const
{
    return timer.isSupported();
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

    // A nested frame's passes belong to the frame it is nested in.
    if (openFrames > 1)
        return;

    ++frameCount;

    // The timer takes its slot from the counter, the same way StreamingBuffers
    // takes its pool from it - one advance, driven by whoever built the Frame,
    // and nothing for either of them to be told separately.
    timer.beginFrame(frameCount, *this);
}

FrameNesting Device::enterFrame(Frame& frame)
{
    assertOwningThread();

    auto nesting = FrameNesting {openFrame};

    if (nesting.outer != nullptr)
    {
        nesting.outer->flush();
        nesting.timerSlot = timer.suspend();
    }

    openFrame = &frame;
    ++openFrames;

    return nesting;
}

void Device::leaveFrame(const FrameNesting& nesting)
{
    openFrame = nesting.outer;
    --openFrames;

    if (nesting.outer == nullptr)
        return;

    timer.resume(nesting.timerSlot);
    nesting.outer->resumeRecording();
}
} // namespace eacp::GPU
