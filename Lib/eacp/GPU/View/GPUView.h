#pragma once

#include "../Common.h"

namespace eacp::GPU
{
class Frame;

// A View that renders with the GPU; override render() to draw. Rendering is
// on-demand via the normal repaint() invalidation path, submitting no GPU work
// while nothing is dirty. Continuous mode instead renders every vsync.
class GPUView : public Graphics::View
{
public:
    GPUView();
    ~GPUView() override;

    virtual void render(Frame&) {}

    // Called once per display refresh while continuous mode is on, before
    // render(). Scale animation by the frame's delta time.
    virtual void update(Threads::FrameTime) {}

    void resized() override;
    void backingScaleChanged() override;

    // Defaults to 4. Must match RenderPipelineDescriptor::sampleCount, so set
    // it before building the pipeline.
    int sampleCount() const;
    void setSampleCount(int count);

    // Pair with RenderPipelineDescriptor::depth on the pipeline.
    void setDepth(bool enabled);
    bool hasDepth() const;

    void setContinuous(bool continuous);
    bool isContinuous() const;

    // On DXGI the present queue depth, each frame of which is input latency
    // (default 2). On Metal maximumDrawableCount, a pool whose shrinking only
    // blocks nextDrawable and raises latency (default 3). Set before drawing.
    void setFramesInFlight(int count);
    int framesInFlight() const;

    // Device pixels per logical point. All public geometry is in logical points;
    // anything sized in real pixels - a glyph atlas, setScissorRect - needs this.
    float backingScale() const;

    // Fired when the backing scale changes, so pixel-sized resources can be
    // rebuilt. Not called for the initial scale; read backingScale() for that.
    std::function<void(float)> onBackingScaleChanged = [](float) {};

    // Fired after the GPU device was lost and replaced (Windows only). The
    // view's own targets are already rebuilt; recreate app-owned Buffers and
    // pipelines here, resources from the lost device no longer rendering.
    std::function<void()> onDeviceRestored = [] {};

protected:
    // Renders and presents one frame synchronously on the calling (main)
    // thread, putting externally-driven content on glass a refresh sooner than
    // waiting for a display link tick.
    void renderNow();

private:
    // Subclasses override render(), not this.
    void paint(Graphics::Context&) final;

    // Off-screen render + read-back, renderInContext: not reaching GPU content.
    Graphics::Image renderNativeContent(float scale) final;

    // Zero-copy render straight into the target's GPU surface, no read-back.
    bool renderNativeContentToTarget(void* nativeTarget, float scale) final;

    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
