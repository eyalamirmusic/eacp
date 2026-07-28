#pragma once

#include "ComputePass.h"
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
    // anything else yields a pass that records nothing.
    //
    // Depth is the target's, from TextureDescriptor::depth: a target created
    // with one gets it attached, cleared to the far plane and discarded, and a
    // target created without one runs the pass with no depth test at all. Ask
    // for it whenever the pass draws a 3D scene rather than a full-screen quad,
    // and match it with RenderPipelineDescriptor::depth on the pipeline.
    //
    // Multisampling is still deliberately absent. A texture target has nothing
    // to resolve into - the texture *is* what a resolve would produce - and the
    // pass is single-sampled, so a pipeline used here needs sampleCount 1 even
    // when the same shader draws multisampled into the drawable.
    //
    // A texture cannot be sampled by the same pass that renders into it. That
    // is what two of them and a swap is for - see the ping-pong a feedback
    // buffer needs.
    RenderPass beginPass(const Texture& target,
                         const RenderPassDescriptor& descriptor = {});

    // Records a compute pass onto this frame's command buffer, in order with
    // its render passes and with nothing waiting in between.
    //
    // This is what lets a kernel's output feed the very draw that consumes it,
    // in the same frame, without the result ever reaching the CPU: a particle
    // integrator writing the buffer the next pass draws as per-instance data, a
    // histogram a fragment shader then reads. The alternative —
    // Device::makeCommandBuffer — is a separate submission whose commit() has to
    // finish before the frame can use a byte of what it wrote.
    //
    // Ordering is the queue's, exactly as it is for two render passes on one
    // frame (see beginPass above), so a buffer written here is legal to bind in
    // a later pass and neither backend needs a fence to say so.
    //
    // A command buffer has one open encoder at a time: let the returned pass end
    // (drop it out of scope, or call end()) before beginning the pass that reads
    // what it wrote.
    ComputePass beginCompute();

    bool isValid() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
