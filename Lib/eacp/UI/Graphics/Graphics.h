#pragma once

#include "../Common.h"
#include "../Render/DrawList.h"
#include "../Render/GradientRamps.h"
#include "../Render/PathShape.h"

#include <string_view>

namespace eacp::UI
{
enum class Justification
{
    Left,
    Centred,
    Right
};

// The painter handed to Component::paint: an immediate-mode facade recording
// into a DrawList, which the frame replays. Coordinates are logical points in
// the calling component's space; no affine transform, the clip being a scissor.
class Graphics
{
public:
    // `bounds` is the area being painted in its own space, and where the clip
    // starts - clips can only ever narrow from there.
    Graphics(DrawList& listToUse,
             GradientRamps& rampsToUse,
             Text::TextRenderer& textToUse,
             const Rect& bounds);

    void setColour(const Color& colour);
    Color getColour() const { return state.colour; }

    // In force until clearGradient or restoreState. The axis is resolved in the
    // space in force here, not at the draw call. A gradient the ramps had no
    // room for silently falls back to the current colour.
    void setGradient(const Gradient& gradient);
    void clearGradient();
    bool hasGradient() const { return !state.gradient.isEmpty(); }

    // Fills the whole clip region, whatever the current origin is.
    void fillAll();
    void fillAll(const Color& colour);

    void fillRect(const Rect& rect);

    // `cornerRadius` in points.
    void fillRoundedRect(const Rect& rect, float cornerRadius);

    // An outline drawn inside the rect's edges.
    void drawRect(const Rect& rect, float thickness = 1.f);

    void
        drawRoundedRect(const Rect& rect, float cornerRadius, float thickness = 1.f);

    void drawLine(Point a, Point b, float thickness = 1.f);

    // Issues a quad sampling coverage the compute pass rasterized before this
    // frame's render pass opened - the shape must be a member of the component
    // so the host can find it. A mesh-backed shape draws its own geometry.
    void fillPath(const PathShape& shape);

    // Draws the layer's already-rendered texture faded by its opacity, which is
    // the only way to fade a group as a unit rather than a shape at a time.
    void drawLayer(const Layer& layer);

    // Draws with the pen on the baseline at the string's left edge, and returns
    // the advance so runs can be chained along a line.
    float drawText(std::string_view text, Point baselineLeft);

    // Vertically centred on `area`, horizontally per `justification`.
    void drawText(std::string_view text,
                  const Rect& area,
                  Justification justification = Justification::Left);

    float measureText(std::string_view text) const;
    float lineHeight() const;
    float ascent() const;

    // Stacked with the rest of the state. measureText, lineHeight and ascent all
    // report the face in force.
    void setFont(const Font& font);

    // The face in force at another size or weight.
    void setFontSize(float pointSize);
    void setFontStyle(FontStyle style);

    const Font& getFont() const { return state.font; }

    void translate(float x, float y);

    // Narrows the clip to `rect`, expressed in the current space. Never widens
    // it: a component cannot escape its parent's clip by asking.
    void reduceClipRegion(const Rect& rect);

    // Multiplies later drawing by the shape's coverage, until restoreState. Only
    // narrows to the bounds for a mesh-backed shape, or for text; a second call
    // replaces the first mask rather than intersecting with it.
    void reduceClipToShape(const PathShape& shape);

    bool hasClipShape() const { return !state.clipMask.isEmpty(); }

    // The clip, back in the current space.
    Rect getClipBounds() const;

    // About this painting's own clip only, not about where the component ended
    // up - ancestor clipping happens at replay.
    bool isClipEmpty() const;

    void saveState();
    void restoreState();

    struct ScopedState
    {
        explicit ScopedState(Graphics& graphicsToUse)
            : graphics(graphicsToUse)
        {
            graphics.saveState();
        }

        ~ScopedState() { graphics.restoreState(); }

        ScopedState(const ScopedState&) = delete;
        ScopedState& operator=(const ScopedState&) = delete;

        Graphics& graphics;
    };

private:
    struct State
    {
        Color colour = Color::white();

        // Already resolved against the ramps, so stacking a state copies floats.
        GradientFill gradient;

        Font font;

        Point origin;
        Rect clip;

        // In surface space, like the clip rect beside it.
        ClipMask clipMask;
    };

    // Applies the current origin, giving the painting's own space.
    Rect toLocal(const Rect& rect) const;
    Point toLocal(Point point) const;

    // Called before every primitive; records only when the clip has changed.
    void recordClip();

    DrawList& list;
    GradientRamps& ramps;

    // One renderer for the whole tree, whatever faces it draws in.
    Text::TextRenderer& text;

    State state;
    Vector<State> stack;

    // What the list has been told, which lags the caller until a primitive.
    Rect recordedClip;
    ClipMask recordedClipMask;
};
} // namespace eacp::UI
