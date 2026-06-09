#include <eacp/Core/Utils/WinInclude.h>

#include "View.h"
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

// Immediate-mode 2D drawing for View::paint(Context&) on Windows. macOS backs
// paint() with a CGContext; here each painting View owns a Direct2D drawing
// surface composited behind its children, and this context issues D2D calls
// into it. It mirrors the retained Layer path (ShapeLayer/TextLayer) but driven
// imperatively from the cross-platform paint() callback.

// Lets the context drive a view's backing surface without naming the private
// View::Native type. The view creates/sizes its surface lazily on the first
// draw, so container views that paint nothing allocate nothing.
struct BackingSurface
{
    virtual ~BackingSurface() = default;
    virtual ID2D1DeviceContext* beginDraw(D2D1::Matrix3x2F& baseTransform) = 0;
    virtual void endDraw() = 0;
    virtual bool hasSurface() const = 0;
};

constexpr float radiansToDegrees = 57.29577951308232f;

class D2DContext final : public Context
{
public:
    explicit D2DContext(BackingSurface& targetToUse)
        : target(targetToUse)
    {
    }

    ~D2DContext() override { finish(); }

    // Opens the underlying surface for drawing if it has not been already.
    // Returns false when the view has nothing to draw into (zero size or no
    // compositor), in which case every draw call becomes a no-op.
    bool ensureDrawing()
    {
        if (drawing)
            return true;
        if (failed)
            return false;

        dc = target.beginDraw(baseTransform);

        if (!dc)
        {
            failed = true;
            return false;
        }

        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), brush.GetAddressOf());

        // setColor() is usually called before the first draw op, while the brush
        // does not yet exist; adopt that colour now so the first fill is not
        // stuck on the default white.
        applyColor();

        drawing = true;
        return true;
    }

    void finish()
    {
        if (!drawing)
            return;

        target.endDraw();
        brush.Reset();
        dc = nullptr;
        drawing = false;
    }

    void saveState() override
    {
        savedStates.push_back({userTransform, currentColor, lineWidth});
    }

    void restoreState() override
    {
        if (savedStates.empty())
            return;

        auto& state = savedStates.back();
        userTransform = state.transform;
        currentColor = state.color;
        lineWidth = state.lineWidth;
        savedStates.pop_back();
        applyColor();
    }

    void translate(float x, float y) override
    {
        userTransform = D2D1::Matrix3x2F::Translation(x, y) * userTransform;
    }

    void scale(float x, float y) override
    {
        userTransform = D2D1::Matrix3x2F::Scale(x, y) * userTransform;
    }

    void rotate(float angleRadians) override
    {
        userTransform = D2D1::Matrix3x2F::Rotation(angleRadians * radiansToDegrees)
                        * userTransform;
    }

    void setColor(const Color& color) override
    {
        currentColor = color;
        applyColor();
    }

    void fillRect(const Rect& rect) override
    {
        if (!ensureDrawing())
            return;

        applyTransform();
        dc->FillRectangle(toD2DRect(rect), brush.Get());
    }

    void fillRoundedRect(const Rect& rect, float radius) override
    {
        if (!ensureDrawing())
            return;

        applyTransform();
        dc->FillRoundedRectangle(
            D2D1::RoundedRect(toD2DRect(rect), radius, radius), brush.Get());
    }

    void setLineWidth(float width) override { lineWidth = width; }

    void strokeRect(const Rect& rect) override
    {
        if (!ensureDrawing())
            return;

        applyTransform();
        dc->DrawRectangle(toD2DRect(rect), brush.Get(), lineWidth);
    }

    void drawLine(const Point& start, const Point& end) override
    {
        if (!ensureDrawing())
            return;

        applyTransform();
        dc->DrawLine(D2D1::Point2F(start.x, start.y),
                     D2D1::Point2F(end.x, end.y),
                     brush.Get(),
                     lineWidth);
    }

    void fillPath(const Path& path) override
    {
        auto* geometry = static_cast<ID2D1Geometry*>(path.getHandle());
        if (!geometry || !ensureDrawing())
            return;

        applyTransform();
        dc->FillGeometry(geometry, brush.Get());
    }

    void strokePath(const Path& path) override
    {
        auto* geometry = static_cast<ID2D1Geometry*>(path.getHandle());
        if (!geometry || !ensureDrawing())
            return;

        applyTransform();
        dc->DrawGeometry(geometry, brush.Get(), lineWidth);
    }

    void drawText(const std::string& text,
                  const Point& position,
                  const Font& font) override
    {
        if (text.empty())
            return;

        auto* format = static_cast<IDWriteTextFormat*>(font.getHandle());
        if (!format || !ensureDrawing())
            return;

        applyTransform();

        auto wide = toWideString(text);
        auto fontSize = format->GetFontSize();

        // CoreGraphics positions text on the baseline; DWrite lays it out from
        // the top of the layout box. Lift the box by roughly the ascent so both
        // backends place a line of text at the same spot.
        auto top = position.y - fontSize * 0.8f;
        auto layout = D2D1::RectF(
            position.x, top, position.x + 100000.0f, top + fontSize * 4.0f);

        dc->DrawText(wide.c_str(),
                     static_cast<UINT32>(wide.size()),
                     format,
                     layout,
                     brush.Get());
    }

private:
    struct SavedState
    {
        D2D1::Matrix3x2F transform;
        Color color;
        float lineWidth;
    };

    static D2D1_RECT_F toD2DRect(const Rect& rect)
    {
        return D2D1::RectF(rect.x, rect.y, rect.x + rect.w, rect.y + rect.h);
    }

    void applyTransform() { dc->SetTransform(userTransform * baseTransform); }

    void applyColor()
    {
        if (brush)
            brush->SetColor(D2D1::ColorF(
                currentColor.r, currentColor.g, currentColor.b, currentColor.a));
    }

    BackingSurface& target;
    ID2D1DeviceContext* dc = nullptr;
    ComPtr<ID2D1SolidColorBrush> brush;

    D2D1::Matrix3x2F baseTransform = D2D1::Matrix3x2F::Identity();
    D2D1::Matrix3x2F userTransform = D2D1::Matrix3x2F::Identity();

    Color currentColor {1.0f, 1.0f, 1.0f, 1.0f};
    float lineWidth = 1.0f;

    std::vector<SavedState> savedStates;
    bool drawing = false;
    bool failed = false;
};

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

struct View::Native : PaintTarget, BackingSurface
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
            return nullptr;

        auto dpiScale = NativeLayerBase::getDpiScale();
        baseTransform = D2D1::Matrix3x2F::Scale(dpiScale, dpiScale)
                        * D2D1::Matrix3x2F::Translation(
                            static_cast<float>(offset.x),
                            static_cast<float>(offset.y));
        return paintDc.get();
    }

    void endDraw() override
    {
        if (paintSurface && paintDc)
        {
            paintDc->SetTransform(D2D1::Matrix3x2F::Identity());

            if (auto interop = paintSurface.as<ABI::Windows::UI::Composition::
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

        auto dpiScale = NativeLayerBase::getDpiScale();
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
            paintSurface = graphicsDevice.CreateDrawingSurface(
                {static_cast<float>(pixelWidth), static_cast<float>(pixelHeight)},
                wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                wgdx::DirectXAlphaMode::Premultiplied);

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
