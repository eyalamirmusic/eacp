#pragma once

#include "ClipMask.h"
#include "Gradient.h"
#include "ImageTexture.h"

#include <eacp/GPUWidgets/GPUWidgets.h>
#include <eacp/Text/Text.h>

namespace eacp::UI
{
class Layer;

// What one paint() produced: the primitives it issued, in the order it issued
// them, in the painting component's own points.
//
// Recorded rather than drawn, so that a component whose drawing has not changed
// is replayed instead of being asked again. That is the whole difference from
// what this tier used to do -- every paint() in the tree ran on every frame,
// because the only record of what a component drew was the frame it drew it
// into, and a frame is thrown away.
//
// What that buys is not the primitives, which are cheap; it is everything a
// paint() does *around* them. A string is laid out glyph by glyph, a gradient is
// resolved against the ramp table, a widget works out its own colours and
// positions. None of it changes while the component does not, and all of it used
// to happen sixty times a second.
//
// **Recorded in the component's own points**, which is what makes a move cheap:
// replay adds the component's origin as it goes, so a component that is dragged
// or scrolled replays the list it already has rather than painting again. The
// only things in here that are not local are the atlas coordinates -- a mask uv
// and a glyph's source rect -- and those move only when an atlas is rebuilt,
// which the host notices and answers by re-recording the tree.
//
// The parameters rather than the finished instances, deliberately: building a
// ShapeInstance is ShapeBatch's business, and a second copy of that arithmetic
// living here is a second place for the antialiasing margin and the corner
// clamp to be got wrong.
struct ShapeDraw
{
    enum class Kind
    {
        // A rounded box. A radius of zero is a plain rectangle, which is the
        // same primitive and the same batch.
        Fill,

        // The same box's outline, drawn inside its edges.
        Border,

        // A line of any orientation, from `from` to `to`, with round caps.
        Line,

        // A box painted through a rect of the coverage atlas: what a vector path
        // draws as.
        Mask
    };

    Kind kind = Kind::Fill;

    // The box, for everything but a line.
    Rect rect;

    Point from;
    Point to;

    Color colour;

    float cornerRadius = 0.f;
    float thickness = 0.f;

    // Where in the coverage atlas this shape's own mask is, for Kind::Mask.
    Rect maskUV;

    // Already resolved against the ramp table, and expressed as a map from this
    // list's *own* space -- so replay composes the component's origin into it
    // rather than the recording having to know where the component sits.
    GradientFill gradient;
};

// A run of glyphs one drawText produced, all in one colour.
struct GlyphRun
{
    int first = 0;
    int count = 0;
    Color colour;
};

// A tessellated shape: the triangles, copied rather than pointed at, because a
// list outlives the frame it was recorded in and a shape that has since been
// re-tessellated would leave the pointer aimed at nothing.
struct MeshDraw
{
    Vector<GPUWidgets::MeshVertex> vertices;

    // Where the triangles sit in this list's space. They are authored in the
    // points of the component that owns the shape, so a paint() that translated
    // before filling has an offset to carry and the replay adds its own to it.
    Point offset;

    // The shape's own reach, for the clip to be elided against. The triangles
    // carry it too, but a bound the recording already knows is cheaper than one
    // the replay works out per frame.
    Rect bounds;

    Color colour;
    GradientFill gradient;
};

// A layer composited as one quad. The layer itself is pointed at, its texture
// and opacity being read at replay -- so an animated opacity re-records nothing.
struct LayerDraw
{
    const Layer* layer = nullptr;
    Rect bounds;
};

// An image drawn as one quad of its texture: which part of it, where, and how
// faded. The texture is shared with the recording rather than pointed at, which
// is what lets a list outlive the component that painted it -- see
// ImageTexture. A run of them sharing a texture is one instanced draw.
struct ImageDraw
{
    ImageRef image;
    Rect bounds;

    // The part of the image drawn, in normalised coordinates.
    Rect uv;

    float opacity = 1.f;
};

// The clip in force for everything issued after it, as an absolute state rather
// than a change: what a paint() narrowed the region to, and the mask it narrowed
// it with. Recorded whenever it differs from the last one recorded, which is the
// same laziness the renderers apply -- a paint() that saves and restores around
// a run costs two of these however much state the save carried.
struct ClipDraw
{
    Rect region;
    ClipMask mask;
};

// One entry of the list: what kind of thing, and where its data is. Consecutive
// primitives of one kind share a command, so replaying a component is a walk
// over a handful of these rather than over its primitives.
struct DrawCommand
{
    enum class Kind
    {
        Shapes,
        Glyphs,
        Mesh,
        Layer,
        Images,
        Clip
    };

    Kind kind = Kind::Shapes;

    // A range into the matching vector for Shapes and Images; an index into it
    // for the rest, whose entries carry their own ranges.
    int first = 0;
    int count = 0;
};

class DrawList
{
public:
    // Keeps the storage. A component re-recording its own drawing writes over
    // what was there, so a repaint every frame allocates nothing after the first
    // one -- which is the case that has to be cheap, an animation being exactly
    // that.
    void clear();

    bool isEmpty() const { return commands.empty(); }

    void addShape(const ShapeDraw& shape);

    // The glyphs go straight into the list's own storage, so a layout writes
    // where it will be replayed from rather than into a scratch buffer that is
    // then copied. The pair is one call in practice: take the size, lay out,
    // and close the run.
    Vector<Text::PlacedGlyph>& glyphStorage() { return glyphs; }
    void addGlyphRun(int first, const Color& colour);

    void addMesh(const Vector<GPUWidgets::MeshVertex>& mesh,
                 Point offset,
                 const Rect& bounds,
                 const Color& colour,
                 const GradientFill& gradient);

    void addLayer(const Layer& layer, const Rect& bounds);

    void addImage(const ImageDraw& image);

    void addClip(const ClipDraw& clip);

    const Vector<DrawCommand>& getCommands() const { return commands; }
    const Vector<ShapeDraw>& getShapes() const { return shapes; }
    const Vector<Text::PlacedGlyph>& getGlyphs() const { return glyphs; }
    const Vector<GlyphRun>& getGlyphRuns() const { return glyphRuns; }
    const Vector<MeshDraw>& getMeshes() const { return meshes; }
    const Vector<LayerDraw>& getLayers() const { return layers; }
    const Vector<ImageDraw>& getImages() const { return images; }
    const Vector<ClipDraw>& getClips() const { return clips; }

private:
    // Extends the last command when it is of `kind`, and starts one otherwise.
    // Which is what collapses a component's twenty rectangles into one command,
    // and what keeps a component that alternates rectangles and glyphs honest
    // about the order it drew them in.
    void append(DrawCommand::Kind kind, int first);

    Vector<DrawCommand> commands;

    Vector<ShapeDraw> shapes;
    Vector<Text::PlacedGlyph> glyphs;
    Vector<GlyphRun> glyphRuns;
    Vector<MeshDraw> meshes;
    Vector<LayerDraw> layers;
    Vector<ImageDraw> images;
    Vector<ClipDraw> clips;
};
} // namespace eacp::UI
