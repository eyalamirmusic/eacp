#pragma once

#include "../Primitives/Primitives.h"

namespace eacp::Graphics
{
class View;

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
