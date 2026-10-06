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
    explicit MacOSContext(CGContextRef contextToUse, bool forSnapshot = false);
    ~MacOSContext() override;

    void saveState() override;
    void restoreState() override;

    void translate(float x, float y) override;
    void scale(float x, float y) override;
    void rotate(float angle) override;

    void fillRect(const Rect& r) override;
    void setColor(const Color& color) override;
    void fillRoundedRect(const Rect& r, float radius) override;

    void setCurrentPath(const Path& p);

    void fillPath(const Path& p) override;
    void setLineWidth(float width) override;
    void strokeRect(const Rect& r) override;
    void strokePath(const Path& p) override;
    void drawLine(const Point& start, const Point& end) override;

    void drawText(const std::string& text,
                  const Point& position,
                  const Font& font) override;

private:
    CGContextRef context;
    Color currentColor;
};
} // namespace eacp::Graphics
