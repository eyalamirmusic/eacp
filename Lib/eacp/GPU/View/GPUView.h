#pragma once

#include "../Common.h"

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

    // How many frames the renderer may have on the go at once.
    //
    // The two backends mean different things by that, and only one of them is a
    // latency knob:
    //
    // On DXGI it is the depth of the present queue. Every frame queued is a
    // frame of delay between a hand moving and the picture answering, because
    // what the user is looking at was built that many refreshes ago. Left to
    // itself DXGI queues three; two lets the CPU prepare a frame while the GPU
    // draws the last and queues nothing further, so two is the Windows default.
    //
    // On Metal it is `maximumDrawableCount`: the size of the pool of buffers the
    // layer hands out to draw into, not a queue of finished frames waiting their
    // turn. A display-link-driven view presents exactly one frame per refresh
    // whichever way this is set, so shrinking the pool dequeues nothing — it
    // just means that when the renderer asks for a buffer there may not be a
    // free one, and `nextDrawable` blocks the calling thread until the display
    // hands one back. That wait lands squarely between sampling the input and
    // drawing with it, so a smaller pool measurably *raises* latency: on the
    // Maze view, sample-to-screen goes from 23ms at three to 32ms at two. Three
    // is therefore the Apple default and there is no reason to lower it.
    //
    // Clamped to what the backend allows (Metal will not take fewer than two).
    // Set before the first frame is drawn.
    void setFramesInFlight(int count);
    int framesInFlight() const;

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
