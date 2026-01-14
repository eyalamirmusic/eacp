#pragma once

#include "../Primitives/Path.h"
#include <eacp/Core/Utils/Common.h>

namespace eacp::Graphics
{

class View;

struct AnimationTransaction
{
    AnimationTransaction();
    ~AnimationTransaction();
};

class ShapeLayer
{
public:
    ShapeLayer();

    void setPath(const Path& path);
    void clearPath();

    void setFillColor(const Color& color);
    void setStrokeColor(const Color& color);
    void setStrokeWidth(float width);

    void setBounds(const Rect& bounds);
    void setPosition(const Point& position);

    void setHidden(bool hidden);
    void setOpacity(float opacity);

private:
    friend class View;
    void attachToLayer(void* nativeLayer);
    void detachFromLayer();

    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics
