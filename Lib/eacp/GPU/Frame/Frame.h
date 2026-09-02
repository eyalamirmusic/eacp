#pragma once

#include "ComputePass.h"
#include "RenderPass.h"

#include <string_view>

namespace eacp::GPU
{
class Device;

// What a pass does with the depth and stencil planes it is handed - the load at
// its start and the store at its end, which are one decision because the second
// is only worth paying for when a later pass makes the first one Load.
//
// The two planes move together throughout, both APIs putting them in one
// attachment of one combined format, so this says nothing about depth that it
// does not also say about stencil.
enum class DepthAction
{
    // Clear on entry, discard on exit. What a pass drawing a scene wants: the
    // buffer is scratch space for the pass and nothing after it looks.
    //
    // The discard is not a detail. A tile-based GPU keeps the buffer in tile
    // memory and would otherwise write the whole thing out to memory at the end
    // of every pass, which is the target's size again in bandwidth for pixels
    // nothing reads.
    Clear,

    // Clear on entry, keep on exit, for a pass a later Resume will pick up
    // from.
    Keep,

    // Load what a Keep or Resume pass left, and keep it again.
    //
    // This is what a *suspended* pass needs. A texture cannot be sampled by the
    // pass rendering into it, so an app that wants to copy the frame it has
    // drawn so far - a refraction sampling what is behind it, Doom 3's
    // _currentRender - has to end its pass, copy, and open another. Without
    // this the second one comes back with an empty depth buffer and everything
    // drawn after the copy has nothing to be occluded by.
    //
    // `clear` is the colour's half of the same question and is separate,
    // because the two are not always asked together: the copy above wants the
    // colour kept and so does the depth, while a second *view* on one frame
    // wants the colour kept and the depth cleared.
    Resume
};

struct RenderPassDescriptor
{
    Graphics::Color clearColor = Graphics::Color::black();
    bool clear = true;

    // Names this pass for the GPU timer, which is also what turns timing on for
    // it: a labelled pass has its start and end timestamped by the hardware and
    // turns up in Device::lastFrameTimings(), while an unlabelled one is not
    // measured and costs nothing.
    //
    // A label rather than a flag because a frame has several passes and the
    // question worth asking is which of them costs what. Copied as the pass
    // begins, so a temporary is safe.
    //
    // Initialised here rather than left to aggregate initialisation, or every
    // existing `beginPass({colour})` in the tree warns under
    // -Wmissing-field-initializers for the field it does not know about.
    std::string_view label = {};

    // The value the stencil plane is cleared to, where the pass has one. Zero
    // is what a mask wants to start from and what a shadow volume counts up
    // from; a half-way value is what an algorithm that decrements below its
    // start needs, since the plane is unsigned.
    //
    // Read on a pass that clears - DepthAction::Clear or Keep - and ignored by
    // one that resumes, which by definition wants the count the pass before it
    // left. A pass with no stencil plane ignores it either way.
    unsigned char clearStencil = 0;

    // The depth and stencil planes' load and store. Clear by default, which is
    // what every pass did before this existed and what a pass drawing a scene
    // wants.
    DepthAction depthAction = DepthAction::Clear;
};

// Off-screen render target for snapshots: a colour texture the app owns instead
// of a swapchain drawable, resolved from msaaTexture when multisampling. A Frame
// built from one renders into it and, on destruction, commits and waits so the
// texture is ready to read back -- it never presents.
struct OffscreenTarget
{
    void* colorTexture = nullptr;
    void* msaaTexture = nullptr;

    // The combined depth-stencil buffer, when the view asked for either. What
    // the pass does with the stencil plane follows from the buffer's format, so
    // there is nothing further to pass here.
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
    //
    // A label times the pass, exactly as it does on a render pass — see
    // RenderPassDescriptor::label.
    ComputePass beginCompute(std::string_view label = {});

    // Sends everything recorded so far to the GPU and carries on recording, so
    // that the rest of the frame is encoded onto a second submission rather
    // than the same one.
    //
    // There is one reason to want that, and it is the reason this exists:
    // **reading back, inside the frame, something the frame itself drew.**
    // Texture::read and Buffer::read are only valid once the work that wrote
    // them has been committed, and a frame commits when it ends — so without
    // this a screenshot taken mid-frame reads the frame before it. Nothing else
    // needs it: two passes on one frame already see each other's results, which
    // is what beginPass above is at pains to say.
    //
    // **No pass may be open.** A command buffer takes one encoder at a time and
    // cannot be committed while one is live, so end the pass first — the same
    // rule beginCompute states for the pass that follows it.
    //
    // What it costs, beyond the submission: the frame's own GPU time on Metal
    // stops meaning the frame. That number is read off the command buffer, and
    // after a flush there are two, so it measures the part after the last one.
    // The per-pass timings are unaffected on both backends, and so is D3D12's
    // total, which is a pair of queries rather than a property of a list.
    void flush();

    bool isValid() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
