#pragma once

#include "../Common.h"
#include "../Render/MeshBatch.h"
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
             MeshBatch& meshesToUse,
             GradientRamps& rampsToUse,
             Text::TextRenderer& textToUse,
             GPU::RenderPass& passToUse,
             const Rect& surfaceToUse,
             float backingScaleToUse);

    void setColour(const Color& colour);
    Color getColour() const { return state.colour; }

    // Fills what follows with a gradient rather than the flat colour, until
    // clearGradient or a restoreState puts the colour back. Everything that
    // fills takes it -- a rect, a rounded rect, a border, a line and a path --
    // because to the renderer they are one primitive read several ways, and a
    // gradient is a property of how it is coloured rather than of which it is.
    //
    // The geometry is read in the space in force *here*, not at the draw call:
    // the ramp is claimed and the axis resolved once, so that a component
    // filling twenty shapes from one gradient costs one lookup. Which is also
    // why it is a call and not an argument -- see GradientRamps for where the
    // colours go and why they are shared.
    //
    // A gradient the ramps had no room for falls back to the current colour, so
    // a shape drawn through this always has something to be drawn in.
    void setGradient(const Gradient& gradient);
    void clearGradient();
    bool hasGradient() const { return !state.gradient.isEmpty(); }

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
    //
    // A shape too large to be worth a mask arrives as triangles instead, and
    // then it is its own geometry that is drawn. Everything above still holds
    // except the batch it joins - so a run of them is still one draw, and an
    // alternation between the two kinds is two.
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

    // Draws the text that follows through `renderer` rather than the host's.
    //
    // A ComponentHost carries one font: a TextRenderer bakes its family at
    // construction and rebuilds its atlas when the size changes, so a family and
    // a size are properties of the host and only FontStyle varies per call. That
    // is right for an interface, whose whole point is that it looks like one
    // thing, and wrong for a document, which mixes both in one tree - an SVG with
    // an 18pt heading over 11pt labels is two fonts and there is no third
    // renderer to ask.
    //
    // So a caller that needs one keeps its own renderer, per family and size,
    // and swaps it in around the run. Measurement follows it, which is the part
    // that matters: measureText, lineHeight and ascent all report the swapped-in
    // font, so a centred string centres against the glyphs that will actually be
    // drawn.
    //
    // It costs a batch break each way. Every renderer has its own glyph atlas
    // and therefore its own texture, so the outgoing one's queue has to be drawn
    // before the incoming one's - which is the same cost a clip change pays, and
    // the reason to swap once per run rather than once per string.
    void setTextRenderer(Text::TextRenderer& renderer);
    void resetTextRenderer();
    Text::TextRenderer& getTextRenderer() const { return *text; }

    struct ScopedTextRenderer
    {
        ScopedTextRenderer(Graphics& graphicsToUse, Text::TextRenderer& renderer)
            : graphics(graphicsToUse)
            , previous(graphicsToUse.getTextRenderer())
        {
            graphics.setTextRenderer(renderer);
        }

        ~ScopedTextRenderer() { graphics.setTextRenderer(previous); }

        ScopedTextRenderer(const ScopedTextRenderer&) = delete;
        ScopedTextRenderer& operator=(const ScopedTextRenderer&) = delete;

        Graphics& graphics;
        Text::TextRenderer& previous;
    };

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

    // How many times painting alternated between the two shape renderers, each
    // one a draw. Zero for an interface, whose shapes are all masks, and zero
    // for a document whose large shapes happen to be drawn together; a document
    // interleaving large and small ones pays one per alternation, which is the
    // only cost the mesh route adds to the picture.
    int getRendererSwitchCount() const { return rendererSwitches; }

    // Draws whatever is still queued, in the order it was issued. Called at the
    // end of a frame; also what a caller changing pass state by hand needs.
    void flush();

private:
    struct State
    {
        Color colour = Color::white();

        // Already resolved against the ramps, so stacking a state is a copy of
        // plain floats. A Gradient's own stop list here would allocate once per
        // component in the tree, the walk saving and restoring around every one.
        GradientFill gradient;

        Point origin;
        Rect clip;
    };

    // Which of the two shape renderers is about to be drawn into. They share one
    // pass, and a pass draws in flush order rather than in call order, so
    // whatever is queued in the other one has to go out first -- otherwise a
    // document stacking a meshed shape over a masked one would come out with the
    // masked one on top, wherever in the document it was.
    enum class Renderer
    {
        Quads,
        Meshes
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
    void prepareToDraw(const Rect& surfaceBounds,
                       Renderer renderer = Renderer::Quads);

    void applyClip(const Rect& surfaceClip);

    ShapeBatch& shapes;
    MeshBatch& meshes;
    GradientRamps& ramps;

    // The host's renderer, and the one in force. They differ only inside a
    // ScopedTextRenderer.
    Text::TextRenderer& hostText;
    Text::TextRenderer* text;

    GPU::RenderPass& pass;

    Rect surface;
    float backingScale = 1.f;

    State state;
    Vector<State> stack;

    // The clip currently on the pass, which lags the one the caller asked for
    // until a primitive forces them to agree.
    Rect appliedClip;
    int clipChanges = 0;
    int rendererSwitches = 0;
};
} // namespace eacp::UI
