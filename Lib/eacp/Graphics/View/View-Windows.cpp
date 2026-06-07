#include <eacp/Core/Utils/WinInclude.h>

#include "View.h"
#include "../Graphics/GraphicsContext.h"
#include "../Layers/NativeLayer-Windows.h"

#include <eacp/Core/Threads/EventLoop.h>

#include <memory>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Composition.h>

namespace wuc = winrt::Windows::UI::Composition;

namespace eacp::Graphics
{

wuc::Compositor getWinRTCompositor();

namespace
{
// The composition backend has no immediate-mode drawing surface for a plain
// View, so the cross-platform paint(Context&) hook is driven with a context
// whose operations are no-ops. Views that draw 2D content use Layers; GPUView
// renders through its own swapchain and ignores the context entirely (just as it
// ignores the CoreGraphics context on macOS).
class NullContext final : public Context
{
public:
    void saveState() override {}
    void restoreState() override {}
    void translate(float, float) override {}
    void scale(float, float) override {}
    void rotate(float) override {}
    void setColor(const Color&) override {}
    void fillRect(const Rect&) override {}
    void fillRoundedRect(const Rect&, float) override {}
    void setLineWidth(float) override {}
    void strokeRect(const Rect&) override {}
    void drawLine(const Point&, const Point&) override {}
    void fillPath(const Path&) override {}
    void strokePath(const Path&) override {}
    void drawText(const std::string&, const Point&, const Font&) override {}
};
} // namespace

struct View::Native
{
    Native(View* owner)
        : ownerView(owner)
    {
        auto compositor = getWinRTCompositor();
        if (compositor)
        {
            visual = compositor.CreateContainerVisual();
        }
    }

    ~Native()
    {
        *alive = false;
        detachFromParent();
        visual = nullptr;
    }

    // Mirrors macOS setNeedsDisplay: marks the view dirty and dispatches a
    // single coalesced paint on the next event-loop tick (multiple repaint()
    // calls in one tick collapse into one paint). The alive token guards
    // against the view being destroyed before the queued paint runs.
    void repaint()
    {
        if (repaintScheduled)
            return;

        repaintScheduled = true;
        auto token = alive;

        Threads::callAsync(
            [this, token]
            {
                if (!*token)
                    return;

                repaintScheduled = false;
                auto context = NullContext {};
                ownerView->paint(context);
            });
    }

    Rect getBounds() const { return bounds; }

    void setBounds(const Rect& newBounds)
    {
        bounds = newBounds;
        updateVisualPosition();
        ownerView->resized();
    }

    void addSubview(View& view)
    {
        if (visual && view.impl->visual)
        {
            view.impl->attachToParent(getVisual());
            view.impl->updateVisualPosition();
        }
    }

    void removeSubview(View& view) { view.impl->detachFromParent(); }

    void attachToParent(wuc::ContainerVisual parentVisual)
    {
        if (parentVisual && visual)
        {
            parentVisual.Children().InsertAtTop(visual);
            parent = parentVisual;
        }
    }

    void detachFromParent()
    {
        if (parent && visual)
        {
            parent.Children().Remove(visual);
            parent = nullptr;
        }
    }

    void updateVisualPosition()
    {
        if (visual)
        {
            visual.Offset({bounds.x, bounds.y, 0.0f});
        }
    }

    wuc::ContainerVisual getVisual() { return visual; }

    Point getMousePosition() const
    {
        POINT pt;
        GetCursorPos(&pt);
        return Point(static_cast<float>(pt.x), static_cast<float>(pt.y));
    }

    void focus() { hasFocusFlag = true; }
    bool hasFocus() const { return hasFocusFlag; }

    View* ownerView;
    Rect bounds;
    bool hasFocusFlag = false;
    bool repaintScheduled = false;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    wuc::ContainerVisual visual {nullptr};
    wuc::ContainerVisual parent {nullptr};
};

View::View()
    : impl(this)
{
}

View::~View()
{
    for (auto* layer: getLayers())
        layer->detachFromLayer();

    removeFromParent();
}

void* View::getHandle()
{
    return &impl->visual;
}

void View::repaint()
{
    impl->repaint();
}

Rect View::getBounds() const
{
    return impl->getBounds();
}

void View::setBounds(const Rect& bounds)
{
    impl->setBounds(bounds);
}

Point View::getMousePosition() const
{
    return impl->getMousePosition();
}

void View::focus()
{
    impl->focus();
}

bool View::hasFocus() const
{
    return impl->hasFocus();
}

void* View::getNativeLayer()
{
    return &impl->visual;
}

void View::viewAdded(View& view)
{
    impl->addSubview(view);
}

void View::viewRemoved(View& view)
{
    impl->removeSubview(view);
}
} // namespace eacp::Graphics
