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

// The painter handed to Component::paint.
//
// An immediate-mode facade over a recording: a call resolves what it was asked
// for -- a colour, a gradient's row, a string's glyphs -- and appends it to a
// DrawList, and the frame replays the list into the batching renderers. A run of
// primitives goes out as one instanced draw when something forces a break, so a
// tree of a few hundred components normally costs a handful of draws rather than
// one per component, which is the whole reason the component tier is lightweight.
//
// Recorded rather than issued, because a component whose drawing has not changed
// should not be asked to draw it again: the list is kept, and paint() runs when
// the component says it has something new to say. See DrawList, and Component's
// repaint().
//
// Coordinates are logical points in the *calling component's* space, and that is
// what is recorded -- the list knows nothing about where its component sits, so
// one that moves replays what it has instead of painting again.
//
// Nothing here touches a render pass, which is what lets a component be painted
// with no frame in progress: a test can build a painter, paint into it and read
// the list back with no GPU at all.
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
    // `bounds` is the area being painted, in its own space -- a component's
    // local bounds. It is where the clip starts, so fillAll fills the component
    // and a clip can only ever narrow from there.
    Graphics(DrawList& listToUse,
             GradientRamps& rampsToUse,
             Text::TextRenderer& textToUse,
             const Rect& bounds);

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

    // Draws a layer the host has already rendered: the whole of its content at
    // once, faded by its opacity and cut by the clip in force.
    //
    // Which is the only way to fade a group as a unit rather than a shape at a
    // time -- see Layer for the difference, which is exactly the overlaps inside
    // it. The content was rendered into its own texture before this frame's pass
    // opened, so what this issues is one quad, and the fade is a uniform: an
    // animated opacity re-renders nothing.
    void drawLayer(const Layer& layer);

    // Draws an image the host has uploaded -- see ImageCache, which is where
    // one comes from -- stretched to `dest` and faded by `opacity`. In its own
    // colours whatever the current colour is: a picture is not a shape read in
    // a colour.
    //
    // A run of these out of one image is one instanced draw, and a change of
    // image costs what a change of clip costs. The image is drawn through the
    // clip in force, shape and all, so a photo under a rounded clip is cut by
    // the corners.
    //
    // Text composites above the images of its clip region whatever order the
    // two were issued in, exactly as it does above the shapes: a caption over
    // a picture costs nothing, and a picture over a caption is a layer's job.
    void drawImage(const ImageRef& image, const Rect& dest, float opacity = 1.f);

    // The `source` part of the image, in its own pixels, stretched to `dest`: a
    // crop, or one cell of a sheet.
    void drawImage(const ImageRef& image,
                   const Rect& source,
                   const Rect& dest,
                   float opacity = 1.f);

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

    // The face the text calls draw and measure in, stacked with the rest of the
    // state -- so a component drawing a heading sets one and its siblings are
    // unaffected, the tree walk restoring it on the way out.
    //
    // Faces cost nothing to mix. They share one glyph atlas and therefore one
    // texture, so a heading, a caption and a monospace log go out as one
    // instanced draw with no batch break between them; what a size costs is the
    // glyphs of that size, once.
    //
    // Measurement follows the font, which is the part that matters: measureText,
    // lineHeight and ascent all report the face in force, so a centred caption
    // centres against the glyphs that will actually be drawn.
    void setFont(const Font& font);

    // The same face at another size or weight, which is what a heading usually
    // is. Named separately because a component asking for "bigger" should not
    // have to name the family the host happens to be using.
    void setFontSize(float pointSize);
    void setFontStyle(FontStyle style);

    const Font& getFont() const { return state.font; }

    void translate(float x, float y);

    // Narrows the clip to `rect`, expressed in the current space. Never widens
    // it: a component cannot escape its parent's clip by asking.
    void reduceClipRegion(const Rect& rect);

    // Everything drawn after this lands over everything drawn before it, text
    // included. Within one clip region text composites above the fills
    // whatever order it was issued in -- which is what a component drawing
    // its own background and then its own caption wants, and the wrong thing
    // for a component that is an opaque object placed over other content: a
    // sticky header over a page scrolled under it, a menu dropped over a form.
    // Such a component says so here, before it draws, and pays a batch break
    // for it. What came before is drawn as it stands; what follows composites
    // among itself as usual.
    void paintOver();

    // Narrows the clip to a vector shape: everything drawn after this is
    // multiplied by the shape's own coverage as well as its own, until a
    // restoreState puts back what was in force before.
    //
    // It costs what a clip change costs -- one batch break, the same as a
    // scissor rect -- so it belongs around a run of shapes rather than around
    // each one. The shape has to be mask-backed to be usable as a clip
    // (Backing::Mask), a mesh carrying no coverage anything could sample: one
    // that is meshed narrows the clip to its own bounds and no further, which is
    // a picture clipped to a rectangle rather than one clipped to nothing.
    //
    // A shape the atlas had no room for has no bounds either, and narrows
    // nothing at all -- so a caller that knows where its region is should reduce
    // to that rectangle itself first, and then this refines it wherever there is
    // a mask to refine it with.
    //
    // Two shaped clips do not intersect, and that is the limit worth knowing:
    // a fragment reads one mask, so a second call replaces the first -- while
    // still narrowing the rectangle to the new shape's bounds, so the result is
    // never larger than either of them asked for. It is exact whenever the outer
    // clip is a rectangle, which is what a viewport clip is; where it is not,
    // composing the two regions into one before they arrive is the answer, and
    // the tier has nothing that does it yet.
    //
    // Text is unaffected. Glyphs are drawn by a renderer that samples no atlas,
    // so a string under a shaped clip is cut by the bounds and not by the shape.
    void reduceClipToShape(const PathShape& shape);

    bool hasClipShape() const { return !state.clipMask.isEmpty(); }

    // The clip, back in the current space.
    Rect getClipBounds() const;

    // Nothing drawn now can be seen. Worth testing before building expensive
    // geometry inside a paint().
    //
    // It answers about the clip this painting narrowed for itself, and not about
    // where the component ended up: a list is recorded once and replayed wherever
    // its component has moved to, so what an ancestor's clip cuts away is the
    // replay's business rather than the recording's. A subtree scrolled out of
    // view is skipped by the frame without being painted at all.
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

        // Already resolved against the ramps, so stacking a state is a copy of
        // plain floats. A Gradient's own stop list here would allocate once per
        // component in the tree, the walk saving and restoring around every one.
        GradientFill gradient;

        Font font;

        Point origin;
        Rect clip;

        // In surface space, like the clip rect beside it, so that stacking a
        // state carries the region rather than the shape it came from.
        ClipMask clipMask;
    };

    // The current origin applied, which is what a recorded coordinate is in: the
    // painting's own space rather than the surface's.
    Rect toLocal(const Rect& rect) const;
    Point toLocal(Point point) const;

    // Records the clip as it now stands, when it differs from the clip already
    // recorded. Called before every primitive, so a save/restore around a run
    // costs the two clips the run was actually drawn under and not one per state
    // change -- which is what a paint() that stacks state for no other reason
    // would otherwise pay.
    void recordClip();

    DrawList& list;
    GradientRamps& ramps;

    // One renderer for the whole tree, whatever faces it draws in: the atlas
    // behind it holds them all.
    Text::TextRenderer& text;

    State state;
    Vector<State> stack;

    // What the list has been told, which lags what the caller asked for until a
    // primitive makes the two matter.
    Rect recordedClip;
    ClipMask recordedClipMask;
};
} // namespace eacp::UI
