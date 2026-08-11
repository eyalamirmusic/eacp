#pragma once

#include "../Primitives/Path.h"
#include "../Primitives/Font.h"

namespace eacp::Graphics
{
class Context
{
public:
    virtual ~Context() = default;

    // True for an off-screen renderToImage snapshot rather than a live frame,
    // so paint() can skip work that only makes sense on screen.
    bool isSnapshot() const { return snapshotMode; }

    virtual void saveState() = 0;
    virtual void restoreState() = 0;

    virtual void translate(float x, float y) = 0;
    virtual void scale(float x, float y) = 0;
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