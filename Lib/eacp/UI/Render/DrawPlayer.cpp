#include "DrawPlayer.h"

#include "Layer.h"

#include <algorithm>
#include <cmath>

namespace eacp::UI
{
DrawPlayer::DrawPlayer(ShapeBatch& shapesToUse,
                       MeshBatch& meshesToUse,
                       ImageBatch& imagesToUse,
                       LayerRenderer& layersToUse,
                       Text::TextRenderer& textToUse,
                       GPU::RenderPass& passToUse,
                       const Rect& surfaceToUse,
                       float backingScaleToUse)
    : shapes(shapesToUse)
    , meshes(meshesToUse)
    , images(imagesToUse)
    , layers(layersToUse)
    , text(textToUse)
    , pass(passToUse)
    , surface(surfaceToUse)
    , backingScale(backingScaleToUse)
    , clip(surfaceToUse)
    , appliedClip(surfaceToUse)
{
    // The renderers outlive the frame, so one that ended inside a clip would
    // hand the next one its mask. The pass is new every frame and takes its
    // scissor with it; this does not, so it is put back by hand. Free when there
    // was none, which is every frame of an interface that never clips.
    shapes.setClipMask({});
    meshes.setClipMask({});
    images.setClipMask({});
}

Rect DrawPlayer::offsetBy(const Rect& rect, Point origin)
{
    return {rect.x + origin.x, rect.y + origin.y, rect.w, rect.h};
}

GradientFill DrawPlayer::offsetBy(const GradientFill& gradient, Point origin)
{
    if (gradient.isEmpty())
        return gradient;

    auto result = gradient;

    result.toGradientSpace =
        GPUWidgets::AffineTransform::translation(-origin.x, -origin.y)
            .then(gradient.toGradientSpace);

    return result;
}

void DrawPlayer::drainInOrder()
{
    meshes.flush();
    shapes.flush();
    images.flush();
    text.flush(pass);
    text.begin();
}

void DrawPlayer::applyClip(bool changeScissor, bool changeMask)
{
    // Every queue has to be drawn under the clip it was issued in, so the meshes
    // and the glyphs go out alongside the quads. setScissorRect flushes the
    // shape queue itself; the text renderer has to be told, and then restarted
    // for the glyphs that follow.
    drainInOrder();

    if (changeScissor)
    {
        if (sameRect(clip, surface))
            shapes.clearScissorRect();
        else
            shapes.setScissorRect({clip.x * backingScale,
                                   clip.y * backingScale,
                                   clip.w * backingScale,
                                   clip.h * backingScale});

        appliedClip = clip;
    }

    if (changeMask)
    {
        // Every renderer, and not only the one about to draw: a document's
        // clipped run may hold shapes of either kind and pictures beside them,
        // and whichever is told last would otherwise draw the run before it
        // under this clip.
        shapes.setClipMask(clipMask);
        meshes.setClipMask(clipMask);
        images.setClipMask(clipMask);

        appliedClipMask = clipMask;
    }

    ++clipChanges;
}

void DrawPlayer::prepareToDraw(const Rect& surfaceBounds, Renderer renderer)
{
    // Unconditionally, and before the clip is even looked at: what decides the
    // order the renderers come out in is which of them flushed first, so every
    // one not about to be used has to be emptied whether or not anything else
    // about the state changed. Counted when one had anything in it, that being
    // exactly the draw this alternation cost.
    //
    // The layer renderer queues nothing -- it draws where it is called, a layer
    // being one quad of its own texture -- so it never appears here as something
    // to drain, only as the thing the other two are drained for.
    auto holding = (renderer != Renderer::Quads && !shapes.isEmpty())
                   || (renderer != Renderer::Meshes && !meshes.isEmpty())
                   || (renderer != Renderer::Images && !images.isEmpty());

    if (holding)
    {
        if (renderer != Renderer::Meshes)
            meshes.flush();

        if (renderer != Renderer::Quads)
            shapes.flush();

        if (renderer != Renderer::Images)
            images.flush();

        ++rendererSwitches;
    }

    // And the glyphs, for a layer alone. Text is otherwise left to composite
    // above the fills of its own clip region whatever order it was issued in --
    // which is what a component drawing its own background and then its own
    // caption wants -- but a layer is a picture placed *over* what came before
    // it, and a document fading a group over a heading means the heading to be
    // underneath.
    if (renderer == Renderer::Layers)
    {
        text.flush(pass);
        text.begin();
    }

    // A rect the primitive is wholly inside cuts nothing off it, so the change
    // can wait for a primitive that the two rects actually disagree about --
    // which is the elision that keeps a deep tree at a handful of draws. A mask
    // gets no such reprieve: it is coverage rather than a bound, so a fragment
    // anywhere in the primitive may be the one it cuts.
    auto changeScissor =
        !sameRect(appliedClip, clip)
        && !(contains(clip, surfaceBounds) && contains(appliedClip, surfaceBounds));

    auto changeMask = !sameClipMask(appliedClipMask, clipMask);

    if (changeScissor || changeMask)
        applyClip(changeScissor, changeMask);
}

void DrawPlayer::playShapes(const DrawList& list,
                            const DrawCommand& command,
                            Point origin)
{
    const auto& recorded = list.getShapes();

    for (auto i = command.first; i < command.first + command.count; ++i)
    {
        const auto& shape = recorded[i];
        auto gradient = offsetBy(shape.gradient, origin);

        if (shape.kind == ShapeDraw::Kind::Line)
        {
            auto from = Point {shape.from.x + origin.x, shape.from.y + origin.y};
            auto to = Point {shape.to.x + origin.x, shape.to.y + origin.y};

            auto bounds = Rect {std::min(from.x, to.x) - shape.thickness,
                                std::min(from.y, to.y) - shape.thickness,
                                std::abs(to.x - from.x) + shape.thickness * 2.f,
                                std::abs(to.y - from.y) + shape.thickness * 2.f};

            prepareToDraw(bounds);
            shapes.drawLine(from, to, shape.colour, shape.thickness, gradient);
            continue;
        }

        auto target = offsetBy(shape.rect, origin);

        if (shape.kind == ShapeDraw::Kind::Shadow)
        {
            auto shadow = ShadowShape {target,
                                       shape.cornerRadius,
                                       shape.shadowOffset,
                                       shape.blurRadius,
                                       shape.spread,
                                       shape.inset};

            // What the shadow actually covers, for the clip to be elided
            // against: the box it falls from for an inset one, and that box
            // moved, spread and blurred for an outer one.
            auto reach = shadow.inset ? 0.f : shadow.blurRadius + shadow.spread;
            auto offset = shadow.inset ? Point {} : shadow.offset;

            prepareToDraw({target.x + offset.x - reach,
                           target.y + offset.y - reach,
                           target.w + reach * 2.f,
                           target.h + reach * 2.f});

            shapes.fillShadow(shadow, shape.colour, gradient);
            continue;
        }

        prepareToDraw(target);

        if (shape.kind == ShapeDraw::Kind::Fill)
            shapes.fillRect(target, shape.colour, shape.cornerRadius, gradient);
        else if (shape.kind == ShapeDraw::Kind::Border)
            shapes.drawRect(
                target, shape.colour, shape.thickness, shape.cornerRadius, gradient);
        else
            shapes.fillMask(target, shape.colour, shape.maskUV, gradient);
    }
}

void DrawPlayer::playGlyphs(const DrawList& list,
                            const DrawCommand& command,
                            Point origin)
{
    const auto& run = list.getGlyphRuns()[command.first];
    const auto& glyphs = list.getGlyphs();

    // The run's own reach, for the clip to be elided against. Taken from the
    // glyphs rather than re-measured: they are the thing being drawn, and
    // measuring the string again is the walk this whole arrangement exists to
    // avoid. A glyph is snapped to a whole pixel on its way to the screen, so
    // the reach is allowed a point either way.
    auto bounds = offsetBy(glyphs[run.first].destination, origin);

    for (auto i = run.first + 1; i < run.first + run.count; ++i)
    {
        const auto& destination = glyphs[i].destination;

        auto left = std::min(bounds.x, destination.x + origin.x);
        auto top = std::min(bounds.y, destination.y + origin.y);
        auto right = std::max(bounds.right(), destination.right() + origin.x);
        auto bottom = std::max(bounds.bottom(), destination.bottom() + origin.y);

        bounds = {left, top, right - left, bottom - top};
    }

    prepareToDraw({bounds.x - 1.f, bounds.y - 1.f, bounds.w + 2.f, bounds.h + 2.f});

    for (auto i = run.first; i < run.first + run.count; ++i)
    {
        auto placed = glyphs[i];
        placed.pen = {placed.pen.x + origin.x, placed.pen.y + origin.y};
        placed.destination = offsetBy(placed.destination, origin);

        text.drawGlyph(placed, run.colour);
    }
}

void DrawPlayer::playMesh(const DrawList& list,
                          const DrawCommand& command,
                          Point origin)
{
    const auto& mesh = list.getMeshes()[command.first];

    prepareToDraw(offsetBy(mesh.bounds, origin), Renderer::Meshes);

    auto offset = Point {mesh.offset.x + origin.x, mesh.offset.y + origin.y};

    meshes.addMesh(
        mesh.vertices, offset, mesh.colour, offsetBy(mesh.gradient, origin));
}

void DrawPlayer::playImages(const DrawList& list,
                            const DrawCommand& command,
                            Point origin)
{
    const auto& recorded = list.getImages();

    for (auto i = command.first; i < command.first + command.count; ++i)
    {
        const auto& image = recorded[i];
        auto target = offsetBy(image.bounds, origin);

        prepareToDraw(target, Renderer::Images);

        images.draw(
            image.image->texture, target, image.uv, Color::white(image.opacity));
    }
}

void DrawPlayer::playLayer(const DrawList& list,
                           const DrawCommand& command,
                           Point origin)
{
    const auto& recorded = list.getLayers()[command.first];

    if (recorded.layer == nullptr || recorded.layer->isEmpty())
        return;

    auto target = offsetBy(recorded.bounds, origin);

    // What the scissor has to be judged against is where the quad actually
    // lands, which a transform may have moved anywhere -- so the bounds of the
    // four corners it was turned into, and not the untransformed rect the
    // renderer is handed to place the content in. The matrix is local to the
    // layer, so it is applied to a rect at the origin and put back afterwards.
    auto turned = recorded.layer->getTransform().apply(
        Rect {0.f, 0.f, target.w, target.h});

    prepareToDraw(offsetBy(turned, {target.x, target.y}), Renderer::Layers);

    // Straight to the pass rather than into a queue, and the clip goes with it
    // rather than being state the renderer holds: one layer is one draw, so
    // there is nothing to batch and nothing for a state change to break.
    layers.draw(pass, *recorded.layer, target, clipMask, shapes.getAtlas());
}

void DrawPlayer::play(const DrawList& list, Point origin, const Rect& clipToUse)
{
    if (list.isEmpty())
        return;

    // Where the walk got to, and no mask: a clip a component set for itself is
    // part of its own list and does not reach the next one. Nothing is applied
    // yet -- what is on the pass catches up at the first primitive that the two
    // actually disagree about.
    clip = clipToUse;
    clipMask = {};

    for (const auto& command: list.getCommands())
    {
        switch (command.kind)
        {
            case DrawCommand::Kind::Shapes:
                playShapes(list, command, origin);
                break;

            case DrawCommand::Kind::Glyphs:
                playGlyphs(list, command, origin);
                break;

            case DrawCommand::Kind::Mesh:
                playMesh(list, command, origin);
                break;

            case DrawCommand::Kind::Layer:
                playLayer(list, command, origin);
                break;

            case DrawCommand::Kind::Images:
                playImages(list, command, origin);
                break;

            case DrawCommand::Kind::Clip:
            {
                const auto& recorded = list.getClips()[command.first];

                clip = clipToUse.intersection(offsetBy(recorded.region, origin));

                clipMask = recorded.mask.isEmpty()
                               ? ClipMask {}
                               : ClipMask {offsetBy(recorded.mask.region, origin),
                                           recorded.mask.uv};
                break;
            }

            case DrawCommand::Kind::Fence:
                drainInOrder();
                ++clipChanges;
                break;
        }
    }
}

void DrawPlayer::flush()
{
    // Shapes first, then glyphs: within one clip region text composites above
    // the fills, which is what a component drawing its own background and then
    // its own caption wants.
    //
    // Only one of the three queues can hold anything by now -- drawing into
    // any of them empties the others -- so the order among them is whatever
    // order they were issued in, which is the one that matters.
    meshes.flush();
    shapes.flush();
    images.flush();
    text.flush(pass);
    text.begin();
}
} // namespace eacp::UI
