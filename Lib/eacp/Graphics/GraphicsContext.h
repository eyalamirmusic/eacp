#pragma once

#include "Primitives.h"

namespace eacp::Graphics
{
class Context
{
public:
    virtual ~Context() = default;

    virtual void setFillColor(const Color& color) = 0;
    virtual void fillRect(const Rect& rect) = 0;
};
} // namespace eacp::Graphics
