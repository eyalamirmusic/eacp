#pragma once

#include <eacp/Graphics/Helpers/DisplayLink.h>
#include <eacp/Graphics/View/View.h>

#include <functional>

namespace eacp::GPU
{
class Frame;

// A View that renders with the GPU. Backed by a CAMetalLayer (on Metal) added
// as a sublayer of the standard View backing layer, so it lives inside the
// normal eacp::Graphics::View hierarchy and reuses its window, events and
// sizing. Override render() to draw.
//
// Rendering is on-demand and uses the normal View invalidation path: call the
// inherited repaint() (e.g. after changing state) and render() runs on the next
// draw cycle. While nothing is dirty no GPU work is submitted at all. Enable
// continuous mode for animation, where render() runs every display refresh
// (vsync) via an internal display link.
class GPUView : public Graphics::View
{
public:
    GPUView();
    ~GPUView() override;

    virtual void render(Frame&) {}

    // Called once per display refresh while continuous mode is on; the view
    // then redraws via render(). Advance animation state here, scaled by the
    // frame's delta time, so motion stays smooth and rate-independent.
    virtual void update(Threads::FrameTime) {}

    void resized() override;

    // Multisample (MSAA) count used for rendering; defaults to 4 for smooth
    // edges. Feed this into your RenderPipelineDescriptor::sampleCount so the
    // pipeline matches the render target. Set before building the pipeline.
    int sampleCount() const;
    void setSampleCount(int count);

    // Enables a depth buffer for this view so 3D content occludes correctly. Pair
    // with RenderPipelineDescriptor::depth on the pipeline. Off by default.
    void setDepth(bool enabled);
    bool hasDepth() const;

    void setContinuous(bool continuous);
    bool isContinuous() const;

    // Fired after the GPU device was lost and replaced (driver update, GPU
    // reset — Windows only). The view's swapchain and MSAA/depth targets are
    // already rebuilt; recreate app-owned Buffers and pipelines here, since
    // resources created on the lost device no longer render.
    std::function<void()> onDeviceRestored = [] {};

private:
    // Internal: drives the GPU render from the View draw cycle. Subclasses
    // override render(), not this.
    void paint(Graphics::Context&) final;
    void renderNow();

    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
