#pragma once

/** @file The 2D drawing interface View::paint receives; Apple and Windows only
    (EACP_HAS_CONTEXT). On Linux and Android paint() never runs: draw with UI
    components (UI::ComponentHost) or GPU::GPUView / UI::GPUComponent there. */

#include "../Primitives/Path.h"
#include "../Primitives/Font.h"

namespace eacp::Graphics
{
/// Immediate-mode 2D drawing in points, y down, from the View's top-left.
/// Core Graphics on Apple, Direct2D on Windows. Only valid inside paint().
class Context
{
public:
    virtual ~Context() = default;

    /// True when this context draws an off-screen View snapshot (renderToImage)
    /// rather than a live on-screen frame. Lets paint() skip work that only makes
    /// sense on screen -- a GPUView renders its content via renderNativeContent
    /// and must not present a live frame while being snapshotted.
    constexpr bool isSnapshot() const { return snapshotMode; }

    /// Pushes transform, colour and line width; pair with restoreState().
    virtual void saveState() = 0;
    virtual void restoreState() = 0;

    virtual void translate(float x, float y) = 0;
    virtual void scale(float x, float y) = 0;
    /// Radians, about the current origin.
    virtual void rotate(float angleRadians) = 0;

    virtual void setColor(const Color& color) = 0;

    virtual void fillRect(const Rect& rect) = 0;
    virtual void fillRoundedRect(const Rect& rect, float radius) = 0;

    virtual void setLineWidth(float width) = 0;
    virtual void strokeRect(const Rect& rect) = 0;
    virtual void drawLine(const Point& start, const Point& end) = 0;

    virtual void fillPath(const Path& p) = 0;
    virtual void strokePath(const Path& p) = 0;

    virtual void drawText(const std::string& text,
                          const Point& position,
                          const Font& font) = 0;

protected:
    bool snapshotMode = false;
};
} // namespace eacp::Graphics