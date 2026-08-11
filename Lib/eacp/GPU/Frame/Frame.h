#pragma once

#include "ComputePass.h"
#include "RenderPass.h"

#include <string_view>

namespace eacp::GPU
{
class Device;

struct RenderPassDescriptor
{
    Graphics::Color clearColor = Graphics::Color::black();
    bool clear = true;

    // Setting a label turns on GPU timing for this pass and names it in
    // Device::lastFrameTimings(). Copied as the pass begins, so a temporary is
    // safe.
    std::string_view label = {};
};

// Snapshot target: a Frame built from one commits and waits on destruction so
// the texture is ready to read back, and never presents.
struct OffscreenTarget
{
    void* colorTexture = nullptr;
    void* msaaTexture = nullptr;
    void* depthTexture = nullptr;
};

// One renderable frame, created by GPUView each tick. Presents the drawable and
// commits the command buffer on destruction. A non-null msaaTexture is rendered
// into and resolved into the drawable.
class Frame
{
public:
    Frame(Device& device, void* drawable, void* msaaTexture, void* depthTexture);
    Frame(Device& device, const OffscreenTarget& target);
    ~Frame();

    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    RenderPass beginPass(const RenderPassDescriptor& descriptor = {});

    // On this same frame, so a later pass may sample what this one wrote with no
    // fence. target needs TextureDescriptor::renderTarget and brings its own
    // depth; always single-sampled, so pipelines here need sampleCount 1.
    RenderPass beginPass(const Texture& target,
                         const RenderPassDescriptor& descriptor = {});

    // Ordered with this frame's render passes, so a buffer written here is legal
    // to bind in a later one without reaching the CPU. Only one encoder may be
    // open at a time: end this pass before beginning the reader.
    ComputePass beginCompute(std::string_view label = {});

    bool isValid() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
