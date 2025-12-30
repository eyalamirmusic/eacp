#pragma once

#include <memory>
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
    struct Impl;
    std::shared_ptr<Impl> impl;
};
} // namespace eacp::Graphics