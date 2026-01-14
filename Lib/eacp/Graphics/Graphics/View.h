#pragma once

#include <eacp/Core/Utils/Common.h>
#include "GraphicsContext.h"
#include "Layer.h"

namespace eacp::Graphics
{

struct MouseEvent
{
    Point pos;
};

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

    void addLayer(Layer& layer);
    void removeLayer(Layer& layer);

private:
    std::vector<View*> subviews;
    std::vector<Layer*> layers;
    View* parent = nullptr;

    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::Graphics