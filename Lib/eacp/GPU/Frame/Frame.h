#pragma once

#include "RenderPass.h"

namespace eacp::GPU
{
class Device;

struct RenderPassDescriptor
{
    Graphics::Color clearColor = Graphics::Color::black();
    bool clear = true;
};

// Off-screen render target for snapshots: a colour texture the app owns instead
// of a swapchain drawable, resolved from msaaTexture when multisampling. A Frame
// built from one renders into it and, on destruction, commits and waits so the
// texture is ready to read back -- it never presents.
struct OffscreenTarget
{
    void* colorTexture = nullptr;
    void* msaaTexture = nullptr;
    void* depthTexture = nullptr;
};

// One renderable frame: owns the drawable being rendered to plus its command
// buffer. Presents the drawable and commits the command buffer on destruction.
// Created by GPUView each tick and handed to GPUView::render. When msaaTexture
// is non-null the pass renders into it and resolves into the drawable.
class Frame
{
public:
    Frame(Device& device, void* drawable, void* msaaTexture, void* depthTexture);
    Frame(Device& device, const OffscreenTarget& target);
    ~Frame();

    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    RenderPass beginPass(const RenderPassDescriptor& descriptor = {});

    // Renders into an app-owned texture instead of the frame's own target: the
    // app-facing render-to-texture, of which OffscreenTarget above is the
    // snapshot-only ancestor.
    //
    // It is a pass on the *same* frame rather than a frame of its own, which is
    // the whole point. A shader that runs several passes over one image - a
    // Shadertoy's Buffer A through D, a blur's two axes, anything that reads
    // back what a previous pass wrote - wants them on one command buffer, in
    // order, with nothing waiting in between. Passes on a frame are already
    // ordered by the queue, so a texture written by an earlier one is legal to
    // sample in a later one and neither backend needs a fence to say so.
    //
    // The texture must have been created with TextureDescriptor::renderTarget;
    // anything else yields a pass that records nothing. Multisampling and depth
    // are deliberately absent - what this is for is a full-screen pass over a
    // whole texture, and neither has a meaning there.
    //
    // A texture cannot be sampled by the same pass that renders into it. That
    // is what two of them and a swap is for - see the ping-pong a feedback
    // buffer needs.
    RenderPass beginPass(const Texture& target,
                         const RenderPassDescriptor& descriptor = {});

    bool isValid() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
