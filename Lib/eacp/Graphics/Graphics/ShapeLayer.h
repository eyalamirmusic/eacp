#pragma once

#include "../Primitives/Path.h"
#include "Layer.h"
#include <eacp/Core/Utils/Common.h>

namespace eacp::Graphics
{
class ShapeLayer : public Layer
{
public:
    ShapeLayer();

    void setPath(const Path& path);
    void clearPath();

    void setFillColor(const Color& color);
    void setStrokeColor(const Color& color);
    void setStrokeWidth(float width);

private:
    void* getNativeLayer() override;

    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics
