#pragma once

#include "Primitives.h"
#include <eacp/Core/Utils/Common.h>

namespace eacp::Graphics
{

class Path
{
public:
    Path();

    void clear();

    void moveTo(const Point& target);
    void lineTo(const Point& target);

    void quadTo(float cx, float cy, float x, float y);
    void cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y);

    void close();

    void addRect(const Rect& rect);
    void addRoundedRect(const Rect& rect, float radius);
    void addEllipse(const Rect& rect);

    void* getHandle() const;

private:
    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics