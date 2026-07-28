#pragma once

#include "../Common.h"
#include "../Render/PathShape.h"
#include "../Render/ShapeBatch.h"

#include <string_view>

namespace eacp::UI
{
enum class Justification
{
    Left,
    Centred,
    Right
};

// The painter handed to Component::paint.
//
// An immediate-mode facade over the batching shape and glyph renderers: a call
// queues a quad rather than issuing a draw, and a run of them goes out as one
// instanced draw when something forces a break. A tree of a few hundred
// components therefore normally costs a handful of draws rather than one per
// component, which is the whole reason the component tier is lightweight.
//
// Coordinates are logical points in the *calling component's* space. The tree
// walk translates the origin before each paint(), so a component always draws
// as though it sat at the origin, whatever its bounds are.
//
// State -- colour, origin, clip -- is stacked via saveState/restoreState, or the
// RAII ScopedState below. Only translation is offered rather than a full affine
// transform, and deliberately: the clip is a GPU scissor rect, which cannot
// express a rotated region, so a rotating transform here would silently stop
// clipping rather than fail. Rotation belongs with a real clip stack, which is
// stencil work.
class Graphics
{
public:
    Graphics(ShapeBatch& shapesToUse,
             Text::TextRenderer& textToUse,
             GPU::RenderPass& passToUse,
             const Rect& surfaceToUse,
             float backingScaleToUse);

    void setColour(const Color& colour);
    Color getColour() const { return state.colour; }

    // Fills the whole clip region, whatever the current origin is -- the usual
    // first line of a paint().
    void fillAll();
    void fillAll(const Color& colour);

    void fillRect(const Rect& rect);

    // The same, with the corners rounded by `cornerRadius` points. Costs the
    // same as a square one and joins the same batch: the renderer draws both
    // from one distance field, so rounding is a number rather than a mode.
    void fillRoundedRect(const Rect& rect, float cornerRadius);

    // An outline drawn inside the rect's edges.
    void drawRect(const Rect& rect, float thickness = 1.f);

    void
        drawRoundedRect(const Rect& rect, float cornerRadius, float thickness = 1.f);

    void drawLine(Point a, Point b, float thickness = 1.f);

    // Fills a vector shape the component built earlier, in the current colour.
    //
    // A path is not drawn from its geometry here: the coverage was computed by
    // a compute kernel before this frame's render pass opened, and what this
    // issues is a quad sampling it out of the shared atlas. So it joins the
    // same instanced draw as the rectangles around it, and costs the same as
    // one of them.
    //
    // Which is why the shape is a member of the component rather than a Path
    // passed in: see PathShape for where the rasterization actually happens and
    // why it cannot happen here.
    void fillPath(const PathShape& shape);

    // Draws with the pen on the baseline at the string's left edge, and returns
    // the advance so differently coloured runs can be chained along a line.
    float drawText(std::string_view text, Point baselineLeft);

    // Places the string in `area`: vertically centred on the box, horizontally
    // per `justification`. What a label or a button caption wants, and the
    // reason it exists separately is that getting the baseline right from
    // ascent/lineHeight is the step every call site otherwise gets subtly wrong.
    void drawText(std::string_view text,
                  const Rect& area,
                  Justification justification = Justification::Left);

    float measureText(std::string_view text) const;
    float lineHeight() const;
    float ascent() const;

    void translate(float x, float y);

    // Narrows the clip to `rect`, expressed in the current space. Never widens
    // it: a component cannot escape its parent's clip by asking.
    void reduceClipRegion(const Rect& rect);

    // The clip, back in the current space.
    Rect getClipBounds() const;

    // Nothing drawn now can be seen. Worth testing before building expensive
    // geometry -- the tree walk tests it to skip whole scrolled-away subtrees.
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

    // How many times the clip actually had to change while painting. That is
    // what a batch break costs here -- everything issued between two changes
    // goes out as one instanced draw per renderer -- so it is the number to
    // watch when a tree starts costing more than it should.
    int getClipChangeCount() const { return clipChanges; }

    // Draws whatever is still queued, in the order it was issued. Called at the
    // end of a frame; also what a caller changing pass state by hand needs.
    void flush();

private:
    struct State
    {
        Color colour = Color::white();
        Point origin;
        Rect clip;
    };

    Rect toSurface(const Rect& rect) const;
    Point toSurface(Point point) const;

    // Puts the right scissor on the pass for a primitive covering
    // `surfaceBounds` -- which usually means leaving it alone. A component
    // drawing inside its own bounds is cut by neither the clip it asked for nor
    // the one already on the pass, so it needs no change and no batch break;
    // only a primitive that genuinely overflows pays. That elision is what
    // keeps a deep tree at a handful of draws, since otherwise every component
    // would break the batch just by having its own bounds.
    void prepareToDraw(const Rect& surfaceBounds);

    void applyClip(const Rect& surfaceClip);

    ShapeBatch& shapes;
    Text::TextRenderer& text;
    GPU::RenderPass& pass;

    Rect surface;
    float backingScale = 1.f;

    State state;
    Vector<State> stack;

    // The clip currently on the pass, which lags the one the caller asked for
    // until a primitive forces them to agree.
    Rect appliedClip;
    int clipChanges = 0;
};
} // namespace eacp::UI
