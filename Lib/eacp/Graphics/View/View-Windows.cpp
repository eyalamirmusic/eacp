#include "View.h"
#include "../Graphics/GraphicsContext.h"
#include "../Helpers/StringUtils-Windows.h"
#include "../Image/Image.h"
#include "../Layers/NativeLayer-Windows.h"

#include <eacp/Core/Threads/Async.h>
#include <eacp/Core/Utils/Singleton.h>

#include <cmath>
#include <memory>
#include <unordered_set>
#include <vector>

namespace eacp::Graphics
{

// Defined in Window-Windows.cpp: the HWND hosting `view`'s root.
HWND findHostHwndForView(View* view);

namespace
{
using Microsoft::WRL::ComPtr;

// Lets D2DContext drive a view's backing surface without naming the private
// View::Native type. Created lazily on the first draw, so container views that
// paint nothing allocate nothing.
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
        : target(&targetToUse)
    {
    }

    // Snapshot mode: the caller owns dcToUse (already BeginDraw'n), so this
    // neither clears nor calls EndDraw and many views can share one context.
    // `baseToUse` maps points -> device pixels.
    D2DContext(ID2D1DeviceContext* dcToUse, const D2D1::Matrix3x2F& baseToUse)
        : dc(dcToUse)
        , baseTransform(baseToUse)
        , adopted(true)
    {
        snapshotMode = true;
        dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), brush.GetAddressOf());
        applyColor();
        drawing = true;
    }

    ~D2DContext() override { finish(); }

    // False when the view has nothing to draw into (zero size or no
    // compositor), in which case every draw call becomes a no-op.
    bool ensureDrawing()
    {
        if (drawing)
            return true;
        if (failed)
            return false;

        dc = target->beginDraw(baseTransform);

        if (!dc)
        {
            failed = true;
            return false;
        }

        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), brush.GetAddressOf());

        // setColor() usually runs before the brush exists; adopt that colour now.
        applyColor();

        drawing = true;
        return true;
    }

    void finish()
    {
        if (!drawing)
            return;

        if (!adopted)
            target->endDraw();

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
        auto fitted = clampedCornerRadius(rect, radius);
        dc->FillRoundedRectangle(D2D1::RoundedRect(toD2DRect(rect), fitted, fitted),
                                 brush.Get());
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

        // position.y is a baseline; DWrite lays out from the top of the box.
        constexpr auto approximateAscentRatio = 0.8f;
        auto top = position.y - fontSize * approximateAscentRatio;
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

    BackingSurface* target = nullptr;
    ID2D1DeviceContext* dc = nullptr;
    ComPtr<ID2D1SolidColorBrush> brush;

    D2D1::Matrix3x2F baseTransform = D2D1::Matrix3x2F::Identity();
    D2D1::Matrix3x2F userTransform = D2D1::Matrix3x2F::Identity();

    Color currentColor {1.0f, 1.0f, 1.0f, 1.0f};
    float lineWidth = 1.0f;

    std::vector<SavedState> savedStates;
    bool drawing = false;
    bool failed = false;
    bool adopted = false;
};

// Bridges the TU-local dirty set to a view's painter without naming the private
// View::Native type.
struct PaintTarget
{
    virtual ~PaintTarget() = default;
    virtual void renderBackingStore() = 0;
    virtual HWND paintHost() const = 0;
};

// Views that called repaint() since their last paint. Main thread only, so no
// locking. Immortal because a View at namespace scope can outlive it and
// ~Native erases itself here.
std::unordered_set<PaintTarget*>& dirtyViews()
{
    return Singleton::getImmortal<std::unordered_set<PaintTarget*>>();
}
} // namespace

// Called from the host window's WM_PAINT.
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
        ensureVisual();
    }

    ~Native() override
    {
        dirtyViews().erase(this);
        paintSurface.Reset();
        paintVisual.Reset();
        detachFromParent();
        visual.Reset();
    }

    // Rebuilds after a device loss moved the composition generation: DComp
    // visuals do not survive their device. See DComp-Windows.h.
    bool ensureVisual()
    {
        auto current = getCompositionGeneration();

        if (generation != current)
        {
            generation = current;
            visual.Reset();
            visual3.Reset();
            paintVisual.Reset();
            paintSurface.Reset();
            surfacePixelWidth = 0;
            surfacePixelHeight = 0;
        }

        if (visual)
            return true;

        auto* device = getCompositionDevice();
        if (!device)
            return false;

        if (FAILED(device->CreateVisual(visual.GetAddressOf())))
        {
            visual.Reset();
            return false;
        }

        visual.As(&visual3);
        return true;
    }

    // Goes through the OS dirty-region machinery rather than a queued callAsync
    // because WM_PAINT is still delivered inside the modal resize/move loop.
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
        if (visual3)
        {
            visual3->SetOpacity(opacity);
            commitComposition();
        }
    }

    void attachToParent(IDCompositionVisual2* parentVisual)
    {
        if (parentVisual && visual)
        {
            insertVisualAtTop(parentVisual, visual.Get());
            parent = parentVisual;
            commitComposition();
        }
    }

    void detachFromParent()
    {
        if (parent && visual)
        {
            parent->RemoveVisual(visual.Get());
            parent.Reset();
            commitComposition();
        }
    }

    void updateVisualPosition()
    {
        if (visual)
        {
            visual->SetOffsetX(bounds.x);
            visual->SetOffsetY(bounds.y);
        }
    }

    IDCompositionVisual2* getVisual() { return visual.Get(); }

    Point getMousePosition() const
    {
        POINT pt;
        GetCursorPos(&pt);

        // Returns view-local logical points; GetCursorPos is in physical screen
        // pixels, so map to the client area and back out the DPI factor.
        auto host = findHostHwndForView(ownerView);
        if (!host)
            return Point(static_cast<float>(pt.x), static_cast<float>(pt.y));

        ScreenToClient(host, &pt);
        auto dpiScale = static_cast<float>(GetDpiForWindow(host)) / 96.f;
        auto local = Point(static_cast<float>(pt.x) / dpiScale,
                           static_cast<float>(pt.y) / dpiScale);

        for (auto* view = ownerView; view != nullptr; view = view->getParent())
        {
            auto viewBounds = view->getBounds();
            local.x -= viewBounds.x;
            local.y -= viewBounds.y;
        }

        return local;
    }

    void focus() { hasFocusFlag = true; }
    bool hasFocus() const { return hasFocusFlag; }

    // Falls back to the system DPI while the view has no host window.
    float hostDpiScale() const
    {
        if (auto host = findHostHwndForView(ownerView))
            return static_cast<float>(GetDpiForWindow(host)) / 96.f;

        return NativeLayerBase::systemDpiScale();
    }

    void renderBackingStore() override
    {
        D2DContext context(*this);

        // An existing surface must be cleared even on a frame that draws
        // nothing, or stale pixels linger.
        if (hasSurface())
            context.ensureDrawing();

        ownerView->paint(context);
        context.finish();
    }

    HWND paintHost() const override { return findHostHwndForView(ownerView); }

    bool hasSurface() const override { return paintSurface != nullptr; }

    ID2D1DeviceContext* beginDraw(D2D1::Matrix3x2F& baseTransform) override
    {
        if (!ensurePaintSurface())
            return nullptr;

        POINT offset {};
        paintDc.Reset();
        RECT updateRect = {0, 0, surfacePixelWidth, surfacePixelHeight};

        auto hr = paintSurface->BeginDraw(
            &updateRect, IID_PPV_ARGS(paintDc.GetAddressOf()), &offset);

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
        return paintDc.Get();
    }

    void endDraw() override
    {
        if (paintSurface && paintDc)
        {
            paintDc->SetTransform(D2D1::Matrix3x2F::Identity());
            paintSurface->EndDraw();
        }

        paintDc.Reset();
    }

    bool ensurePaintSurface()
    {
        if (!ensureVisual())
            return false;

        auto b = ownerView->getBounds();
        if (b.w <= 0 || b.h <= 0)
            return false;

        auto dpiScale = hostDpiScale();
        auto pixelWidth = static_cast<int>(b.w * dpiScale);
        auto pixelHeight = static_cast<int>(b.h * dpiScale);
        if (pixelWidth <= 0 || pixelHeight <= 0)
            return false;

        auto* device = getCompositionDevice();
        if (!device)
            return false;

        // Sits behind every child view and layer.
        if (!paintVisual)
        {
            if (FAILED(device->CreateVisual(paintVisual.GetAddressOf())))
            {
                paintVisual.Reset();
                return false;
            }

            insertVisualAtBottom(visual.Get(), paintVisual.Get());
        }

        if (!paintSurface || surfacePixelWidth != pixelWidth
            || surfacePixelHeight != pixelHeight)
        {
            paintSurface.Reset();

            auto hr = device->CreateSurface(static_cast<UINT>(pixelWidth),
                                            static_cast<UINT>(pixelHeight),
                                            DXGI_FORMAT_B8G8R8A8_UNORM,
                                            DXGI_ALPHA_MODE_PREMULTIPLIED,
                                            paintSurface.GetAddressOf());

            if (FAILED(hr))
            {
                handleDeviceLossIfNeeded(hr);
                paintSurface.Reset();
                return false;
            }

            paintVisual->SetContent(paintSurface.Get());
            surfacePixelWidth = pixelWidth;
            surfacePixelHeight = pixelHeight;
        }

        // DComp draws the surface's physical pixels 1:1 in the visual's local
        // space, and the root already scales the tree by the DPI factor.
        if (dpiScale > 0.f)
            paintVisual->SetTransform(
                D2D1::Matrix3x2F::Scale(1.f / dpiScale, 1.f / dpiScale));

        return true;
    }

    View* ownerView;
    Rect bounds;
    bool hasFocusFlag = false;

    ComPtr<IDCompositionVisual2> visual;
    ComPtr<IDCompositionVisual3> visual3;
    ComPtr<IDCompositionVisual2> parent;

    ComPtr<IDCompositionVisual2> paintVisual;
    ComPtr<IDCompositionSurface> paintSurface;
    ComPtr<ID2D1DeviceContext> paintDc;

    uint64_t generation = 0;
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
    return impl->getVisual();
}

void View::repaint()
{
    impl->repaint();
}

void View::setOpacity(float opacityToUse)
{
    opacity = opacityToUse;
    impl->setOpacity(opacityToUse);
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

// Stored but not applied: Windows re-asks on every WM_SETCURSOR, so applying it
// needs the host window to resolve the view under the pointer there.
void View::setMouseCursor(MouseCursor cursor)
{
    currentCursor = cursor;
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
    return impl->getVisual();
}

namespace
{
// Off-screen View->Image snapshot. Direct2D bitmaps share the on-screen
// surfaces' top-left origin, so no vertical flip is needed. Compositing runs
// premultiplied; the read-back un-premultiplies for Image's straight alpha.
struct OffscreenComposite
{
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<ID2D1Bitmap1> target;
    int pixelWidth = 0;
    int pixelHeight = 0;

    bool valid() const { return dc && target; }
};

// Returns an open (BeginDraw'n, cleared) composite the caller draws into and
// then hands to readbackComposite; invalid for a non-positive size or no device.
OffscreenComposite makeOffscreenComposite(const Rect& bounds, float scale)
{
    auto composite = OffscreenComposite {};
    composite.pixelWidth = static_cast<int>(std::lround(bounds.w * scale));
    composite.pixelHeight = static_cast<int>(std::lround(bounds.h * scale));

    if (composite.pixelWidth <= 0 || composite.pixelHeight <= 0)
        return composite;

    auto* device = getD2DDevice();
    if (!device)
        return composite;

    if (FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                           composite.dc.GetAddressOf())))
        return {};

    auto properties =
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                  D2D1_ALPHA_MODE_PREMULTIPLIED));

    auto size = D2D1::SizeU(static_cast<UINT>(composite.pixelWidth),
                            static_cast<UINT>(composite.pixelHeight));

    if (FAILED(composite.dc->CreateBitmap(
            size, nullptr, 0, &properties, composite.target.GetAddressOf())))
        return {};

    composite.dc->SetTarget(composite.target.Get());
    composite.dc->BeginDraw();
    composite.dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    return composite;
}

// `dest` is in points.
void drawImageIntoContext(ID2D1DeviceContext* dc,
                          const Image& image,
                          const Rect& dest,
                          const D2D1::Matrix3x2F& transform,
                          float opacity)
{
    if (!image.isValid())
        return;

    auto width = image.width();
    auto height = image.height();
    const auto& pixels = image.pixels();

    // Straight RGBA -> premultiplied BGRA, what a B8G8R8A8 D2D bitmap wants.
    auto bgra = std::vector<std::uint32_t>(static_cast<std::size_t>(width)
                                           * static_cast<std::size_t>(height));

    for (std::size_t i = 0; i < bgra.size(); ++i)
    {
        auto r = pixels[i * 4 + 0];
        auto g = pixels[i * 4 + 1];
        auto b = pixels[i * 4 + 2];
        auto a = pixels[i * 4 + 3];

        auto premul = [&](std::uint8_t c) -> std::uint32_t
        { return static_cast<std::uint32_t>((c * a + 127) / 255); };

        bgra[i] = (static_cast<std::uint32_t>(a) << 24) | (premul(r) << 16)
                  | (premul(g) << 8) | premul(b);
    }

    auto properties = D2D1::BitmapProperties(D2D1::PixelFormat(
        DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    ComPtr<ID2D1Bitmap> bitmap;
    if (FAILED(dc->CreateBitmap(
            D2D1::SizeU(static_cast<UINT>(width), static_cast<UINT>(height)),
            bgra.data(),
            static_cast<UINT32>(width) * 4,
            properties,
            bitmap.GetAddressOf())))
        return;

    dc->SetTransform(transform);
    auto d = D2D1::RectF(dest.x, dest.y, dest.x + dest.w, dest.y + dest.h);
    dc->DrawBitmap(bitmap.Get(), d, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

// Flattens a subtree so it fades as one. Caller must balance with PopLayer.
void pushOpacityLayer(ID2D1DeviceContext* dc,
                      const D2D1::Matrix3x2F& transform,
                      float opacity)
{
    auto params = D2D1::LayerParameters1();
    params.opacity = opacity;
    dc->SetTransform(transform);
    dc->PushLayer(params, nullptr);
}

// `transform` maps points -> device pixels. Web content is drawn later, by the
// caller.
void compositeView(ID2D1DeviceContext* dc,
                   View& view,
                   const D2D1::Matrix3x2F& transform,
                   float scale)
{
    auto grouped = view.getOpacity() < 1.0f;
    if (grouped)
        pushOpacityLayer(dc, transform, view.getOpacity());

    {
        auto painter = D2DContext(dc, transform);
        view.paint(painter);
        painter.finish();
    }

    for (auto* layer: view.getLayers())
    {
        auto* native = static_cast<NativeLayerBase*>(layer->getNativeLayer());
        if (native == nullptr || native->hidden)
            continue;

        auto layerTransform =
            D2D1::Matrix3x2F::Translation(native->position.x, native->position.y)
            * transform;

        auto faded = native->opacity < 1.0f;
        if (faded)
            pushOpacityLayer(dc, layerTransform, native->opacity);

        native->drawInto(dc, layerTransform, scale);

        if (faded)
            dc->PopLayer();
    }

    drawImageIntoContext(
        dc, view.renderNativeContent(scale), view.getLocalBounds(), transform, 1.0f);

    for (auto* child: view.getSubviews())
    {
        auto bounds = child->getBounds();
        auto childTransform =
            D2D1::Matrix3x2F::Translation(bounds.x, bounds.y) * transform;

        dc->SetTransform(childTransform);
        dc->PushAxisAlignedClip(D2D1::RectF(0, 0, bounds.w, bounds.h),
                                D2D1_ANTIALIAS_MODE_ALIASED);
        compositeView(dc, *child, childTransform, scale);
        dc->PopAxisAlignedClip();
    }

    if (grouped)
        dc->PopLayer();
}

// Closes the context and un-premultiplies BGRA back to Image's straight RGBA.
Image readbackComposite(OffscreenComposite& composite)
{
    if (!composite.valid())
        return {};

    if (FAILED(composite.dc->EndDraw()))
        return {};

    auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_PREMULTIPLIED));

    auto size = D2D1::SizeU(static_cast<UINT>(composite.pixelWidth),
                            static_cast<UINT>(composite.pixelHeight));

    ComPtr<ID2D1Bitmap1> readable;
    if (FAILED(composite.dc->CreateBitmap(
            size, nullptr, 0, &properties, readable.GetAddressOf())))
        return {};

    auto dstPoint = D2D1::Point2U(0, 0);
    auto srcRect = D2D1::RectU(0,
                               0,
                               static_cast<UINT>(composite.pixelWidth),
                               static_cast<UINT>(composite.pixelHeight));

    if (FAILED(
            readable->CopyFromBitmap(&dstPoint, composite.target.Get(), &srcRect)))
        return {};

    D2D1_MAPPED_RECT mapped = {};
    if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped)))
        return {};

    auto image = Image {};
    auto* out =
        image.prepareForOverwrite(composite.pixelWidth, composite.pixelHeight);

    if (out == nullptr)
    {
        readable->Unmap();
        return {};
    }

    for (auto y = 0; y < composite.pixelHeight; ++y)
    {
        auto* row = mapped.bits + static_cast<std::size_t>(y) * mapped.pitch;

        for (auto x = 0; x < composite.pixelWidth; ++x)
        {
            auto* src = row + static_cast<std::size_t>(x) * 4;
            auto b = src[0];
            auto g = src[1];
            auto r = src[2];
            auto a = src[3];

            auto* dst =
                out + (static_cast<std::size_t>(y) * composite.pixelWidth + x) * 4;

            if (a == 0)
            {
                dst[0] = dst[1] = dst[2] = dst[3] = 0;
            }
            else
            {
                auto straight = [&](std::uint8_t c) -> std::uint8_t
                { return static_cast<std::uint8_t>((c * 255 + a / 2) / a); };

                dst[0] = straight(r);
                dst[1] = straight(g);
                dst[2] = straight(b);
                dst[3] = a;
            }
        }
    }

    readable->Unmap();
    return image;
}

// `offset` is in the root's coordinate space; `opacity` is the product of the
// group opacities down to this view.
struct AsyncTarget
{
    View* view = nullptr;
    Point offset;
    float opacity = 1.0f;
};

void collectAsyncContent(View& view,
                         Point offset,
                         float opacity,
                         Vector<AsyncTarget>& out)
{
    auto effectiveOpacity = opacity * view.getOpacity();

    if (view.hasAsyncContent())
        out.push_back({&view, offset, effectiveOpacity});

    for (auto* child: view.getSubviews())
    {
        auto bounds = child->getBounds();
        collectAsyncContent(*child,
                            {offset.x + bounds.x, offset.y + bounds.y},
                            effectiveOpacity,
                            out);
    }
}

// Shared across the pending web snapshots; the promise resolves once the last
// one lands.
struct AsyncComposite
{
    OffscreenComposite composite;
    Threads::AsyncPromise<Image> promise;
    int remaining = 0;
};
} // namespace

Image View::renderToImage(float scale)
{
    auto resolvedScale = scale > 0.0f ? scale : impl->hostDpiScale();

    auto composite = makeOffscreenComposite(getLocalBounds(), resolvedScale);
    if (!composite.valid())
        return {};

    compositeView(composite.dc.Get(),
                  *this,
                  D2D1::Matrix3x2F::Scale(resolvedScale, resolvedScale),
                  resolvedScale);

    return readbackComposite(composite);
}

Threads::Async<Image> View::renderToImageAsync(float scale)
{
    auto resolvedScale = scale > 0.0f ? scale : impl->hostDpiScale();

    auto state = std::make_shared<AsyncComposite>();
    auto result = state->promise.get();

    state->composite = makeOffscreenComposite(getLocalBounds(), resolvedScale);
    if (!state->composite.valid())
    {
        state->promise.resolve({});
        return result;
    }

    auto rootTransform = D2D1::Matrix3x2F::Scale(resolvedScale, resolvedScale);
    compositeView(state->composite.dc.Get(), *this, rootTransform, resolvedScale);

    auto targets = Vector<AsyncTarget> {};
    collectAsyncContent(*this, {0.f, 0.f}, 1.0f, targets);

    if (targets.empty())
    {
        state->promise.resolve(readbackComposite(state->composite));
        return result;
    }

    state->remaining = targets.size();

    for (auto& target: targets)
    {
        auto* webView = target.view;
        auto dest = Rect {target.offset.x,
                          target.offset.y,
                          webView->getBounds().w,
                          webView->getBounds().h};
        auto webOpacity = target.opacity;

        webView->captureAsyncContent(
            resolvedScale,
            [state, dest, webOpacity, rootTransform](const Image& webImage)
            {
                drawImageIntoContext(state->composite.dc.Get(),
                                     webImage,
                                     dest,
                                     rootTransform,
                                     webOpacity);

                if (--state->remaining == 0)
                    state->promise.resolve(readbackComposite(state->composite));
            });
    }

    return result;
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
