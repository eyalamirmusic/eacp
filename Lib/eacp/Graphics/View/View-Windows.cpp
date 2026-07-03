#include <eacp/Core/Utils/WinInclude.h>

#include "View.h"
#include "../Graphics/D2DContext-Windows.h"
#include "../Graphics/GraphicsContext.h"
#include "../Layers/NativeLayer-Windows.h"
#include "../Helpers/StringUtils-Windows.h"

#include <unordered_set>
#include <vector>

#include <dwrite.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Composition.h>
#include <windows.ui.composition.interop.h>

namespace wuc = winrt::Windows::UI::Composition;

namespace eacp::Graphics
{

wuc::Compositor getWinRTCompositor();

// Defined in Window-Windows.cpp: the HWND hosting `view`'s root.
HWND findHostHwndForView(View* view);

namespace
{
using Microsoft::WRL::ComPtr;

// Bridges the TU-local dirty set to a view's painter without naming the private
// View::Native type. Each View::Native implements this.
struct PaintTarget
{
    virtual ~PaintTarget() = default;
    virtual void renderBackingStore() = 0;
    virtual HWND paintHost() const = 0;
};

// Views that called repaint() since their last paint. Windows merges the
// per-view InvalidateRect calls into a single WM_PAINT for the host window;
// the handler then paints exactly the views that asked. Main-thread only,
// so no locking is needed.
std::unordered_set<PaintTarget*>& dirtyViews()
{
    static auto views = std::unordered_set<PaintTarget*> {};
    return views;
}
} // namespace

// Paints every dirty view hosted by `host`, then clears them. Driven from
// that window's WM_PAINT, mirroring how macOS setNeedsDisplay drives
// drawRect:. A plain View paints into its Direct2D backing surface; GPUView
// renders its swapchain.
void paintDirtyViewsForHost(HWND host)
{
    auto toPaint = Vector<PaintTarget*> {};

    for (auto* target: dirtyViews())
        if (target->paintHost() == host)
            toPaint.add(target);

    for (auto* target: toPaint)
        dirtyViews().erase(target);

    for (auto* target: toPaint)
        target->renderBackingStore();
}

struct View::Native
    : PaintTarget
    , BackingSurface
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

    ~Native() override
    {
        dirtyViews().erase(this);
        paintBrush = nullptr;
        paintSurface = nullptr;
        paintVisual = nullptr;
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
        dirtyViews().insert(this);

        if (auto host = findHostHwndForView(ownerView))
            InvalidateRect(host, nullptr, FALSE);
    }

    Rect getBounds() const { return bounds; }

    void setBounds(const Rect& newBounds)
    {
        bounds = newBounds;
        updateVisualPosition();
        ownerView->resized();

        // Repaint at the new size, so a view that draws shows up on its initial
        // layout and on resize without waiting for an external repaint(). Views
        // that paint nothing allocate no surface, so this stays cheap.
        repaint();
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

    void setOpacity(float opacity)
    {
        if (visual)
            visual.Opacity(opacity);
    }

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

        // Views work in logical, window-client coordinates (that is how mouse
        // events are delivered — WndProc divides by the DPI scale). GetCursorPos
        // is in physical screen pixels, so convert to client space and back out
        // the DPI factor; otherwise callers that mix this with getBounds() (e.g.
        // drag handling) move at DPI-times speed on high-DPI displays.
        auto host = findHostHwndForView(ownerView);
        if (!host)
            return Point(static_cast<float>(pt.x), static_cast<float>(pt.y));

        ScreenToClient(host, &pt);
        auto dpiScale = static_cast<float>(GetDpiForWindow(host)) / 96.f;
        return Point(static_cast<float>(pt.x) / dpiScale,
                     static_cast<float>(pt.y) / dpiScale);
    }

    void focus() { hasFocusFlag = true; }
    bool hasFocus() const { return hasFocusFlag; }

    // The DPI scale of the window hosting this view, so paint surfaces stay
    // crisp on a monitor whose scaling differs from the system DPI. Falls back
    // to the system DPI while the view is not yet parented into a window.
    float hostDpiScale() const
    {
        if (auto host = findHostHwndForView(ownerView))
            return static_cast<float>(GetDpiForWindow(host)) / 96.f;

        return NativeLayerBase::systemDpiScale();
    }

    // --- PaintTarget: invoked from the host window's WM_PAINT ----------------
    void renderBackingStore() override
    {
        D2DContext context(*this);

        // A surface that already exists must be cleared even on a frame that
        // draws nothing, so stale pixels do not linger. The first-ever paint is
        // opened lazily by the context's first draw call instead.
        if (hasSurface())
            context.ensureDrawing();

        ownerView->paint(context);
        context.finish();
    }

    HWND paintHost() const override { return findHostHwndForView(ownerView); }

    // --- BackingSurface: the Direct2D drawing surface paint() renders into ---
    bool hasSurface() const override { return paintSurface != nullptr; }

    ID2D1DeviceContext* beginDraw(D2D1::Matrix3x2F& baseTransform) override
    {
        if (!ensurePaintSurface())
            return nullptr;

        auto interop = paintSurface.as<
            ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>();
        if (!interop)
            return nullptr;

        POINT offset {};
        paintDc = nullptr;
        RECT updateRect = {0, 0, surfacePixelWidth, surfacePixelHeight};

        auto hr =
            interop->BeginDraw(&updateRect, IID_PPV_ARGS(paintDc.put()), &offset);
        if (FAILED(hr) || !paintDc)
        {
            handleDeviceLossIfNeeded(hr);
            return nullptr;
        }

        auto dpiScale = hostDpiScale();
        baseTransform =
            D2D1::Matrix3x2F::Scale(dpiScale, dpiScale)
            * D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x),
                                            static_cast<float>(offset.y));
        return paintDc.get();
    }

    void endDraw() override
    {
        if (paintSurface && paintDc)
        {
            paintDc->SetTransform(D2D1::Matrix3x2F::Identity());

            if (auto interop =
                    paintSurface.as<ABI::Windows::UI::Composition::
                                        ICompositionDrawingSurfaceInterop>())
                interop->EndDraw();
        }

        paintDc = nullptr;
    }

    // Lazily creates the backing SpriteVisual (behind every child view and
    // layer) and a drawing surface sized to the view's bounds. Recreated when
    // the bounds change, mirroring NativeLayerBase::createSurface for layers.
    bool ensurePaintSurface()
    {
        if (!visual)
            return false;

        auto b = ownerView->getBounds();
        if (b.w <= 0 || b.h <= 0)
            return false;

        auto dpiScale = hostDpiScale();
        auto pixelWidth = static_cast<int>(b.w * dpiScale);
        auto pixelHeight = static_cast<int>(b.h * dpiScale);
        if (pixelWidth <= 0 || pixelHeight <= 0)
            return false;

        auto graphicsDevice = getCompositionGraphicsDevice();
        auto compositor = getWinRTCompositor();
        if (!graphicsDevice || !compositor)
            return false;

        if (!paintVisual)
        {
            paintVisual = compositor.CreateSpriteVisual();
            visual.Children().InsertAtBottom(paintVisual);
        }

        if (!paintSurface || surfacePixelWidth != pixelWidth
            || surfacePixelHeight != pixelHeight)
        {
            try
            {
                paintSurface = graphicsDevice.CreateDrawingSurface(
                    {static_cast<float>(pixelWidth),
                     static_cast<float>(pixelHeight)},
                    wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                    wgdx::DirectXAlphaMode::Premultiplied);
            }
            catch (const winrt::hresult_error& e)
            {
                // The post-recovery redraw re-enters here with a live device.
                handleDeviceLossIfNeeded(e.code());
                paintSurface = nullptr;
                return false;
            }

            if (!paintSurface)
                return false;

            paintBrush = compositor.CreateSurfaceBrush(paintSurface);
            paintVisual.Brush(paintBrush);
            surfacePixelWidth = pixelWidth;
            surfacePixelHeight = pixelHeight;
        }

        paintVisual.Size({b.w, b.h});
        return true;
    }

    View* ownerView;
    Rect bounds;
    bool hasFocusFlag = false;
    wuc::ContainerVisual visual {nullptr};
    wuc::ContainerVisual parent {nullptr};

    wuc::SpriteVisual paintVisual {nullptr};
    wuc::CompositionDrawingSurface paintSurface {nullptr};
    wuc::CompositionSurfaceBrush paintBrush {nullptr};
    winrt::com_ptr<ID2D1DeviceContext> paintDc;
    int surfacePixelWidth = 0;
    int surfacePixelHeight = 0;
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

void View::setOpacity(float opacity)
{
    impl->setOpacity(opacity);
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
