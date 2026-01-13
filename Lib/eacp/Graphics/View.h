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
    virtual ~View();

    void repaint();
    void* getHandle();

    virtual void paint(Context&) {};
    virtual void mouseDown(const MouseEvent&) {}

    Rect getBounds() const;
    void setBounds(const Rect& bounds);

    void addSubview(View& view);
    void removeSubview(View& view);
    void removeFromParent();

private:
    std::vector<View*> subviews;
    View* parent = nullptr;

    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::Graphics