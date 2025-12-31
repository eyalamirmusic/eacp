#pragma once

#include "../Utils/Common.h"
#include "GraphicsContext.h"

namespace eacp::Graphics
{

struct MouseEvent
{
    Point pos;
};

class View
{
public:
    View();

    void repaint();
    void* getHandle();

    virtual void paint(Context&) {};
    virtual void mouseDown(const MouseEvent&) {}

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::Graphics