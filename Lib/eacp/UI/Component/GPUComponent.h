#pragma once

#include "../Render/Layer.h"
#include "Component.h"

#include <eacp/Graphics/Helpers/DisplayLink.h>
#include <eacp/Graphics/Helpers/FramePacer.h>

namespace eacp::UI
{
// Custom GPU rendering inside a component tree: a GPUView's render(Frame&),
// drawn into a texture of the component's own and composited by the host in its
// frame like any other drawing - clipped by its ancestors, under and over its
// siblings, faded and transformed through its layer.
//
// The frame handed to render() is the host's own, redirected at the texture
// (GPU::Frame::Redirect): beginPass() with no texture, pixelSize(),
// logicalSize(), backingScale() and drawableFormat() all answer for the
// component, while beginCompute, the texture passes, copies and flush() are the
// host frame's, in order with the rest of it and ahead of the pass that
// composites the result. So a GPUView subclass ports by changing its base.
//
// render() runs on the host's frame when the component was repainted, resized
// or moved to another scale, or on every display refresh while continuous - and
// only while some of it is on screen. A component scrolled out of its parent or
// clipped away entirely is culled: nothing renders, and it renders again the
// frame it comes back. A hidden one is skipped the same way and keeps its
// texture.
//
// Continuous mode ticks off the host's one display link. update() keeps running
// while the component is culled or hidden, as long as it is in a host, so its
// state does not stall; such a tick asks nothing of the host, so an off-screen
// component costs its update() and no frame. isOnScreen() is how an update()
// with nothing to do off screen returns early.
class GPUComponent : public Component
{
public:
    GPUComponent();
    ~GPUComponent() override;

    virtual void render(GPU::Frame&);

    // Once a display refresh while continuous, paced by setMaxFps, before the
    // render() it brings about. The delta spans any ticks the pacing skipped.
    virtual void update(Threads::FrameTime);

    // Samples per pixel of the texture, which a pipeline drawing here has to
    // set as its sampleCount. 1 by default, unlike a GPUView's 4: a texture per
    // component adds up, and a grid of them multisampled with depth runs to
    // gigabytes. Changing it remakes the texture.
    int sampleCount() const;
    void setSampleCount(int count);

    // A depth buffer on the texture. Turning it off drops the stencil too.
    void setDepth(bool enabled);
    bool hasDepth() const;

    // A stencil plane, which brings the depth buffer with it.
    void setStencil(bool enabled);
    bool hasStencil() const;

    // Whether the texture's alpha reaches what is beneath the component. Off by
    // default, which is opaque, as a GPUView is: the alpha a pass leaves is
    // ignored and the component is a solid rectangle. On, the colour is taken
    // as premultiplied, so pipelines that are not opaque blend with
    // GPU::BlendMode::AlphaBlendOntoTransparent. Not a re-render.
    void setTransparent(bool shouldBeTransparent);
    bool isTransparent() const;

    void setContinuous(bool shouldBeContinuous);
    bool isContinuous() const;

    // Caps how often continuous mode updates and renders; zero (the default)
    // follows the host's clock. See ComponentHost::setAnimationMaxFps for the
    // cap on the clock itself.
    void setMaxFps(int fps);
    int maxFps() const;

    // Applied as the texture is composited, so neither is a re-render.
    void setOpacity(float newOpacity);
    float getOpacity() const;
    void setTransform(const GPUWidgets::AffineTransform& newTransform);

    // The scale render() draws at: the host's display, or a snapshot's.
    float backingScale() const;

    GPU::PixelFormat colorFormat() const;

    // Whether the last frame found nothing of this component inside its
    // ancestors' clips.
    bool wasCulledLastFrame() const;

    // Whether any of it was drawn in the last frame: showing, and not culled.
    bool isOnScreen() const;

    // The part of the component's local bounds its ancestors left visible in
    // the last frame. Empty when culled or not showing.
    Rect getVisibleBounds() const;

    // How many times render() has been called.
    int getRenderCount() const;

    const Layer& getLayer() const;

    // Fired before the first render() at a new backing scale - a move to
    // another display, or a snapshot at another scale. Not for the first.
    std::function<void(float)> onBackingScaleChanged = [](float) {};

    // Fired after the GPU device was lost and replaced (Windows only), once the
    // host has rebuilt its own resources: recreate buffers, textures and
    // pipelines made on the old device here. A render follows.
    std::function<void()> onDeviceRestored = [] {};

protected:
    // Renders the host's frame right now rather than at the next display
    // refresh, for a component driven by something else - a camera frame
    // arriving. Where the host has nothing to present into, the render is left
    // to the next frame instead. Nothing renders while the component is off
    // screen.
    void renderNow();

private:
    friend class ComponentHost;

    void paint(Graphics& g) final;
    bool advanceAnimation(double seconds) final;

    void renderFrame(GPU::Frame& frame);
    void resumeIfContinuous();
    void setVisibility(const Rect& visibleLocalBounds);

    Layer layer;
    Threads::FramePacer pacer;

    bool continuous = false;
    double clock = 0.0;
    double sinceLastUpdate = 0.0;

    bool culled = false;
    Rect visibleBounds;

    float renderedScale = 0.f;
    int renders = 0;
};
} // namespace eacp::UI
