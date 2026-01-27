#pragma once

#include "../Primitives/Primitives.h"

namespace eacp::Graphics
{

struct NativeLayerBase
{
    virtual ~NativeLayerBase() = default;

    void setBounds(const Rect& boundsToUse) { bounds = boundsToUse; }
    void setPosition(const Point& pos) { position = pos; }
    void setHidden(bool hiddenState) { hidden = hiddenState; }
    void setOpacity(float op) { opacity = op; }

    Rect bounds;
    Point position;
    float opacity = 1.0f;
    bool hidden = false;
};

} // namespace eacp::Graphics
