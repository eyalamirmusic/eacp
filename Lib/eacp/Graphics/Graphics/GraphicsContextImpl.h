#pragma once

#include "../Primitives/GraphicUtils.h"
#include "GraphicsContext.h"
#include "../Primitives/Path.h"
#include "../Primitives/Font.h"

namespace eacp::Threads
{
template <typename T>
class Async;
}

namespace eacp::Graphics
{
class View;
class Image;

// Composites view and its descendants into an off-screen bitmap sized
// bounds * scale and returns it as a straight-alpha Image: each view's paint()
// chrome, then its attached shape/text layers, its GPU (Metal) content, then its
// child views. Embedded web content does not draw (it is async). scale is pixels
// per point; a non-positive size yields an invalid Image. Shared by macOS and iOS.
Image renderLayerToImage(View& view, const Rect& bounds, float scale);

// As renderLayerToImage, plus embedded WebView content folded in once each
// descendant's async snapshot lands. Resolves on the main thread. Shared by
// macOS and iOS.
Threads::Async<Image>
    renderViewToImageAsync(View& view, const Rect& bounds, float scale);

class MacOSContext final : public Context
{
public:
    explicit MacOSContext(CGContextRef contextToUse, bool forSnapshot = false)
        : context(contextToUse)
        , currentColor {1.0f, 1.0f, 1.0f, 1.0f}
    {
        snapshotMode = forSnapshot;
        saveState();
    }

    ~MacOSContext() override { restoreState(); }

    void saveState() override { CGContextSaveGState(context); }
    void restoreState() override { CGContextRestoreGState(context); }

    void translate(float x, float y) override
    {
        CGContextTranslateCTM(context, x, y);
    }

    void scale(float x, float y) override { CGContextScaleCTM(context, x, y); }
    void rotate(float angle) override { CGContextRotateCTM(context, angle); }

    void fillRect(const Rect& r) override
    {
        CGContextFillRect(context, toCGRect(r));
    }

    void setColor(const Color& color) override
    {
        currentColor = color;
        CGContextSetRGBFillColor(context, color.r, color.g, color.b, color.a);
        CGContextSetRGBStrokeColor(context, color.r, color.g, color.b, color.a);
    }

    void fillRoundedRect(const Rect& r, float radius) override
    {
        auto p = Path();
        p.addRoundedRect(r, radius);
        fillPath(p);
    }

    void setCurrentPath(const Path& p)
    {
        CGContextAddPath(context, (CGPathRef) p.getHandle());
    }

    void fillPath(const Path& p) override
    {
        setCurrentPath(p);
        CGContextFillPath(context);
    }

    void setLineWidth(float width) override
    {
        CGContextSetLineWidth(context, width);
    }

    void strokeRect(const Rect& r) override
    {
        CGContextStrokeRect(context, toCGRect(r));
    }

    void strokePath(const Path& p) override
    {
        setCurrentPath(p);
        CGContextStrokePath(context);
    }

    void drawLine(const Point& start, const Point& end) override
    {
        auto p = Path();
        p.moveTo(start);
        p.lineTo(end);
        strokePath(p);
    }

    void drawText(const std::string& text,
                  const Point& position,
                  const Font& font) override;

private:
    CGContextRef context;
    Color currentColor;
};
} // namespace eacp::Graphics
