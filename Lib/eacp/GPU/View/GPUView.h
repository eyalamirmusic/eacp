#pragma once

#include <eacp/Graphics/View/View.h>

namespace eacp::GPU
{
class Frame;

// A View that renders with the GPU. Backed by a CAMetalLayer (on Metal) added
// as a sublayer of the standard View backing layer, so it lives inside the
// normal eacp::Graphics::View hierarchy and reuses its window, events and
// sizing. Override render() to draw; it is driven by a display link.
class GPUView : public Graphics::View
{
public:
    GPUView();
    ~GPUView() override;

    virtual void render(Frame&) {}

    void resized() override;

private:
    void renderTick();

    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
