#pragma once

#include "DrawList.h"
#include "LayerRenderer.h"
#include "MeshBatch.h"
#include "ShapeBatch.h"

namespace eacp::UI
{
// Draws recorded DrawLists into a pass, in tree order. Holds the state that
// belongs to the frame rather than to a component: which renderer is queueing,
// what clip is on the pass, and where each list's space sits in the surface.
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

    // `origin` and `clip` are both in surface points. A clip the list recorded
    // for itself narrows further and does not survive the call.
    void play(const DrawList& list, Point origin, const Rect& clip);

    // Draws whatever is still queued, in the order it was issued.
    void flush();

    // Each is a batch break.
    int getClipChangeCount() const { return clipChanges; }
    int getRendererSwitchCount() const { return rendererSwitches; }

private:
    // Which renderer is about to draw. They share one pass, which draws in flush
    // order, so whatever is queued in the others has to go out first.
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

    // Usually leaves the scissor alone: a primitive inside both the asked-for
    // clip and the applied one needs no change and no batch break.
    void prepareToDraw(const Rect& surfaceBounds,
                       Renderer renderer = Renderer::Quads);

    // One batch break however many of scissor and mask changed.
    void applyClip(bool changeScissor, bool changeMask);

    static Rect offsetBy(const Rect& rect, Point origin);

    // A translation on the input side of the matrix, a recorded gradient mapping
    // a point of the list's own space.
    static GradientFill offsetBy(const GradientFill& gradient, Point origin);

    ShapeBatch& shapes;
    MeshBatch& meshes;
    LayerRenderer& layers;
    Text::TextRenderer& text;

    GPU::RenderPass& pass;

    Rect surface;
    float backingScale = 1.f;

    // What the list being played asked for.
    Rect clip;
    ClipMask clipMask;

    // What is actually on the pass, which lags the above until a primitive
    // forces the two to agree. The mask, being coverage rather than a bound,
    // catches up whenever it differs at all.
    Rect appliedClip;
    ClipMask appliedClipMask;

    int clipChanges = 0;
    int rendererSwitches = 0;
};
} // namespace eacp::UI
