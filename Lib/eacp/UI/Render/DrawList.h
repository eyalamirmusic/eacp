#pragma once

#include "ClipMask.h"
#include "Gradient.h"

#include <eacp/GPUWidgets/GPUWidgets.h>
#include <eacp/Text/Text.h>

namespace eacp::UI
{
class Layer;

// One primitive a paint() issued, in the painting component's own points - the
// only exception being atlas coordinates, which the host re-records when an
// atlas is rebuilt. Parameters rather than finished ShapeBatch instances.
struct ShapeDraw
{
    enum class Kind
    {
        // A rounded box; a radius of zero is a plain rectangle.
        Fill,

        // The same box's outline, drawn inside its edges.
        Border,

        // From `from` to `to`, with round caps.
        Line,

        // A box painted through a rect of the coverage atlas.
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

    // Kind::Mask only.
    Rect maskUV;

    // Already resolved against the ramp table, and mapped from this list's own
    // space - replay composes the component's origin into it.
    GradientFill gradient;
};

// A run of glyphs one drawText produced, all in one colour.
struct GlyphRun
{
    int first = 0;
    int count = 0;
    Color colour;
};

// A tessellated shape. The triangles are copied, not pointed at: the list
// outlives the frame, and the shape may be re-tessellated.
struct MeshDraw
{
    Vector<GPUWidgets::MeshVertex> vertices;

    // Where the triangles sit in this list's space; replay adds its own.
    Point offset;

    // The shape's own reach, for the clip to be elided against.
    Rect bounds;

    Color colour;
    GradientFill gradient;
};

// A layer composited as one quad, pointed at rather than copied so that texture
// and opacity are read at replay. The layer must outlive the list.
struct LayerDraw
{
    const Layer* layer = nullptr;
    Rect bounds;
};

// The clip in force for everything issued after it, as an absolute state rather
// than a change. Recorded only when it differs from the last one recorded.
struct ClipDraw
{
    Rect region;
    ClipMask mask;
};

struct DrawCommand
{
    enum class Kind
    {
        Shapes,
        Glyphs,
        Mesh,
        Layer,
        Clip
    };

    Kind kind = Kind::Shapes;

    // A range into the matching vector for Shapes; an index into it for the
    // rest, whose entries carry their own ranges.
    int first = 0;
    int count = 0;
};

class DrawList
{
public:
    // Keeps the storage, so re-recording every frame allocates nothing.
    void clear();

    bool isEmpty() const { return commands.empty(); }

    void addShape(const ShapeDraw& shape);

    // Lay out directly into this, then close the run with the size taken first.
    Vector<Text::PlacedGlyph>& glyphStorage() { return glyphs; }
    void addGlyphRun(int first, const Color& colour);

    void addMesh(const Vector<GPUWidgets::MeshVertex>& mesh,
                 Point offset,
                 const Rect& bounds,
                 const Color& colour,
                 const GradientFill& gradient);

    void addLayer(const Layer& layer, const Rect& bounds);

    void addClip(const ClipDraw& clip);

    const Vector<DrawCommand>& getCommands() const { return commands; }
    const Vector<ShapeDraw>& getShapes() const { return shapes; }
    const Vector<Text::PlacedGlyph>& getGlyphs() const { return glyphs; }
    const Vector<GlyphRun>& getGlyphRuns() const { return glyphRuns; }
    const Vector<MeshDraw>& getMeshes() const { return meshes; }
    const Vector<LayerDraw>& getLayers() const { return layers; }
    const Vector<ClipDraw>& getClips() const { return clips; }

private:
    // Extends the last command when it is of `kind`, and starts one otherwise.
    void append(DrawCommand::Kind kind, int first);

    Vector<DrawCommand> commands;

    Vector<ShapeDraw> shapes;
    Vector<Text::PlacedGlyph> glyphs;
    Vector<GlyphRun> glyphRuns;
    Vector<MeshDraw> meshes;
    Vector<LayerDraw> layers;
    Vector<ClipDraw> clips;
};
} // namespace eacp::UI
