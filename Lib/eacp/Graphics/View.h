#pragma once

#include "../Utils/Common.h"
#include "GraphicsContext.h"

namespace eacp::Graphics
{

struct MouseEvent
{
    Point pos;
};

class ShapeLayer;

class View
{
    using ChildViews = std::initializer_list<std::reference_wrapper<View>>;
public:
    View();
    virtual ~View();

    void repaint();
    void* getHandle();

    virtual void paint(Context&) {};
    virtual void mouseDown(const MouseEvent&) {}
    virtual void resized();

    Rect getBounds() const;
    Rect getLocalBounds() const;
    void setBounds(const Rect& bounds);

    void addChildren(ChildViews views);
    void addSubview(View& view);
    void removeSubview(View& view);
    void removeFromParent();

    void addShapeLayer(ShapeLayer& layer);
    void removeShapeLayer(ShapeLayer& layer);

private:
    std::vector<View*> subviews;
    std::vector<ShapeLayer*> shapeLayers;
    View* parent = nullptr;

    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::Graphics