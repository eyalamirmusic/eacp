#pragma once

#include "DrawList.h"
#include "LayerRenderer.h"
#include "MeshBatch.h"
#include "ShapeBatch.h"

namespace eacp::UI
{
// Draws recorded lists into a pass.
//
// The other half of the component tier's frame: the tree walk records what each
// component draws (see DrawList), and this puts the lists on the GPU in tree
// order. It holds everything that is a property of the *frame* rather than of a
// component -- which renderer is queueing, what clip is actually on the pass,
// and where each list's own space sits in the surface -- so that a list can be
// recorded once, at the origin, and replayed wherever its component has moved
// to.
//
// Which is also why the batching lives here and not in the painter. A run of
// quads goes out as one instanced draw, and what breaks the run is a clip change
// or a switch between the two shape renderers; both are decided by comparing one
// component's primitives against what the last one left on the pass, and only
// something walking the whole tree can do that.
class DrawPlayer
{
public:
    DrawPlayer(ShapeBatch& shapesToUse,
               MeshBatch& meshesToUse,
               LayerRenderer& layersToUse,
               Text::TextRenderer& textToUse,
               GPU::RenderPass& passToUse,
               const Rect& surfaceToUse,
               float backingScaleToUse);

    // Replays `list` with its coordinates offset by `origin`, clipped to `clip`
    // -- both in surface points.
    //
    // The clip is where the walk got to: the component's bounds narrowed by
    // every ancestor's. A clip the list recorded for itself narrows it further
    // and does not survive the call, so a component's own clipping cannot escape
    // into the component after it.
    void play(const DrawList& list, Point origin, const Rect& clip);

    // Draws whatever is still queued, in the order it was issued.
    void flush();

    // How many times the clip actually had to change, and how many times drawing
    // alternated between the two shape renderers. Each is a batch break, so
    // between them they are what a frame costs beyond its primitives. See
    // ComponentHost, which reports both.
    int getClipChangeCount() const { return clipChanges; }
    int getRendererSwitchCount() const { return rendererSwitches; }

private:
    // Which of the two shape renderers is about to be drawn into. They share one
    // pass, and a pass draws in flush order rather than in call order, so
    // whatever is queued in the other one has to go out first -- otherwise a
    // document stacking a meshed shape over a masked one would come out with the
    // masked one on top, wherever in the document it was.
    enum class Renderer
    {
        Quads,
        Meshes,
        Layers
    };

    void playShapes(const DrawList& list, const DrawCommand& command, Point origin);
    void playGlyphs(const DrawList& list, const DrawCommand& command, Point origin);
    void playMesh(const DrawList& list, const DrawCommand& command, Point origin);
    void playLayer(const DrawList& list, const DrawCommand& command, Point origin);

    // Puts the right scissor on the pass for a primitive covering
    // `surfaceBounds` -- which usually means leaving it alone. A component
    // drawing inside its own bounds is cut by neither the clip it asked for nor
    // the one already on the pass, so it needs no change and no batch break;
    // only a primitive that genuinely overflows pays. That elision is what keeps
    // a deep tree at a handful of draws, since otherwise every component would
    // break the batch just by having its own bounds.
    void prepareToDraw(const Rect& surfaceBounds,
                       Renderer renderer = Renderer::Quads);

    // Puts the clip the list asked for onto the renderers: the scissor rect, the
    // clip mask, or both. One break however many of them changed, since what it
    // costs is draining the queues and that happens once.
    void applyClip(bool changeScissor, bool changeMask);

    static Rect offsetBy(const Rect& rect, Point origin);

    // The map into the gradient's own space, composed with where the list sits.
    // A recorded gradient maps a point of the *list's* space, so a fragment's
    // surface position has to have the origin taken off it before it is mapped
    // -- which is a translation on the input side of the matrix rather than an
    // offset on its output.
    static GradientFill offsetBy(const GradientFill& gradient, Point origin);

    ShapeBatch& shapes;
    MeshBatch& meshes;
    LayerRenderer& layers;
    Text::TextRenderer& text;

    GPU::RenderPass& pass;

    Rect surface;
    float backingScale = 1.f;

    // What the list being played asked for, which lags what is on the pass until
    // a primitive forces the two to agree.
    Rect clip;
    ClipMask clipMask;

    // What is actually on the pass. The scissor lags because a change can be
    // elided for a primitive already inside both rects; the mask lags too, but
    // catches up whenever it differs at all -- it is coverage rather than a
    // bound, so there is no "inside" to be safely elided.
    Rect appliedClip;
    ClipMask appliedClipMask;

    int clipChanges = 0;
    int rendererSwitches = 0;
};
} // namespace eacp::UI
