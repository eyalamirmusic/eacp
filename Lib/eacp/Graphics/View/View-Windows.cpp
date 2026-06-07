#include <eacp/Core/Utils/WinInclude.h>

#include "View.h"
#include "../Graphics/GraphicsContext.h"
#include "../Layers/NativeLayer-Windows.h"

#include <unordered_set>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Composition.h>

namespace wuc = winrt::Windows::UI::Composition;

namespace eacp::Graphics
{

wuc::Compositor getWinRTCompositor();

// Defined in Window-Windows.cpp: the HWND hosting `view`'s root.
HWND findHostHwndForView(View* view);

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

// Views that called repaint() since their last paint. Windows merges the
// per-view InvalidateRect calls into a single WM_PAINT for the host window;
// the handler then paints exactly the views that asked. Main-thread only,
// so no locking is needed.
std::unordered_set<View*>& dirtyViews()
{
    static auto views = std::unordered_set<View*> {};
    return views;
}
} // namespace

// Paints every dirty view hosted by `host`, then clears them. Driven from
// that window's WM_PAINT, mirroring how macOS setNeedsDisplay drives
// drawRect:. A plain View paints into a NullContext (no-op on the retained
// composition backend); GPUView renders its swapchain.
void paintDirtyViewsForHost(HWND host)
{
    auto toPaint = Vector<View*> {};

    for (auto* view: dirtyViews())
        if (findHostHwndForView(view) == host)
            toPaint.add(view);

    for (auto* view: toPaint)
        dirtyViews().erase(view);

    auto context = NullContext {};

    for (auto* view: toPaint)
        view->paint(context);
}

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
        dirtyViews().erase(ownerView);
        detachFromParent();
        visual = nullptr;
    }

    // Mirrors macOS setNeedsDisplay: mark the view dirty and let Windows
    // coalesce a single WM_PAINT for the host window. The paint itself runs
    // from that WM_PAINT (paintDirtyViewsForHost) — unlike a queued
    // callAsync it rides the OS dirty-region machinery, which is delivered
    // even inside the modal resize/move loop, so animation never wedges.
    void repaint()
    {
        dirtyViews().insert(ownerView);

        if (auto host = findHostHwndForView(ownerView))
            InvalidateRect(host, nullptr, FALSE);
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
