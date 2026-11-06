#pragma once

/** @file Layer: the base of the retained shape and text layers. */

#include "../Primitives/Primitives.h"

namespace eacp::Graphics
{
class View;

/// A retained native layer (CALayer, DirectComposition visual) drawn above a
/// view's paint(). Attaching does not transfer ownership. No subclass
/// exists on Linux or Android.
class Layer
{
public:
    virtual ~Layer() = default;

    void attachTo(void* nativeLayer);
    void attachTo(View& view);

    void detachFromLayer();
    void detachFromView();

    void setBounds(const Rect& bounds);
    void setPosition(const Point& position);

    void setHidden(bool hidden);
    void setOpacity(float opacity);

    virtual void* getNativeLayer() = 0;

private:
    View* parent = nullptr;
};
} // namespace eacp::Graphics
