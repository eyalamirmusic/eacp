#pragma once

#include "../Primitives/Primitives.h"

namespace eacp::Graphics
{
struct Layer
{
    virtual ~Layer() = default;

    void attachToLayer(void* nativeLayer);
    void detachFromLayer();

    void setBounds(const Rect& bounds);
    void setPosition(const Point& position);

    void setHidden(bool hidden);
    void setOpacity(float opacity);

    virtual void* getNativeLayer() = 0;
};
} // namespace eacp::Graphics
