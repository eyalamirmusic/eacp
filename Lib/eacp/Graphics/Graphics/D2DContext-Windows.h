#pragma once

#include <eacp/Core/Utils/WinInclude.h>

#include "GraphicsContext.h"
#include "../Helpers/StringUtils-Windows.h"

#include <vector>

#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

namespace eacp::Graphics
{

// Immediate-mode 2D drawing for Context on Windows. macOS backs paint()
// with a CGContext; here D2DContext issues Direct2D calls into whatever
// surface the target provides — a view's composition drawing surface for
// View::paint, or an offscreen bitmap for renderToImage.

// Lets the context drive a drawing surface without naming its owner's
// private type. The owner creates/sizes its surface lazily on the first
// draw, so container views that paint nothing allocate nothing.
struct BackingSurface
{
    virtual ~BackingSurface() = default;
    virtual ID2D1DeviceContext* beginDraw(D2D1::Matrix3x2F& baseTransform) = 0;
    virtual void endDraw() = 0;
    virtual bool hasSurface() const = 0;
};

inline constexpr float radiansToDegrees = 57.29577951308232f;

class D2DContext final : public Context
{
public:
    explicit D2DContext(BackingSurface& targetToUse)
        : target(targetToUse)
    {
    }

    ~D2DContext() override { finish(); }

    // Opens the underlying surface for drawing if it has not been already.
    // Returns false when the target has nothing to draw into (zero size or no
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
        dc->FillRoundedRectangle(D2D1::RoundedRect(toD2DRect(rect), radius, radius),
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
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;

    D2D1::Matrix3x2F baseTransform = D2D1::Matrix3x2F::Identity();
    D2D1::Matrix3x2F userTransform = D2D1::Matrix3x2F::Identity();

    Color currentColor {1.0f, 1.0f, 1.0f, 1.0f};
    float lineWidth = 1.0f;

    std::vector<SavedState> savedStates;
    bool drawing = false;
    bool failed = false;
};

} // namespace eacp::Graphics
