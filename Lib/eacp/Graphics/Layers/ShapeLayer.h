#pragma once

#include "../Common.h"

#include "../Primitives/Path.h"
#include "Layer.h"

namespace eacp::Graphics
{
class ShapeLayer : public Layer
{
public:
    ShapeLayer();
    ~ShapeLayer() override;

    void setPath(const Path& path);
    void clearPath();

    void setFillColor(const Color& color);
    void setFillGradient(const LinearGradient& gradient);
    void setStrokeColor(const Color& color);
    void setStrokeWidth(float width);

    void* getNativeLayer() override;

private:
    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics
