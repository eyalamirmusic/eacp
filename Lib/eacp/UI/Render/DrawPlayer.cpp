#include "DrawPlayer.h"

#include "Layer.h"

#include <algorithm>
#include <cmath>

namespace eacp::UI
{
DrawPlayer::DrawPlayer(ShapeBatch& shapesToUse,
                       MeshBatch& meshesToUse,
                       LayerRenderer& layersToUse,
                       Text::TextRenderer& textToUse,
                       GPU::RenderPass& passToUse,
                       const Rect& surfaceToUse,
                       float backingScaleToUse)
    : shapes(shapesToUse)
    , meshes(meshesToUse)
    , layers(layersToUse)
    , text(textToUse)
    , pass(passToUse)
    , surface(surfaceToUse)
    , backingScale(backingScaleToUse)
    , clip(surfaceToUse)
    , appliedClip(surfaceToUse)
{
    // The renderers outlive the frame, so one that ended inside a clip would
    // hand the next frame its mask. Unlike the scissor, which the pass carries.
    shapes.setClipMask({});
    meshes.setClipMask({});
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

void DrawPlayer::applyClip(bool changeScissor, bool changeMask)
{
    // Every queue has to be drawn under the clip it was issued in.
    meshes.flush();
    shapes.flush();
    text.flush(pass);
    text.begin();

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
        // Both, not only the one about to draw: a clipped run may hold shapes
        // of either kind.
        shapes.setClipMask(clipMask);
        meshes.setClipMask(clipMask);

        appliedClipMask = clipMask;
    }

    ++clipChanges;
}

void DrawPlayer::prepareToDraw(const Rect& surfaceBounds, Renderer renderer)
{
    // Draw order is flush order, so every renderer not about to be used has to
    // be emptied first. The layer renderer queues nothing and never drains here.
    auto holding = (renderer != Renderer::Quads && !shapes.isEmpty())
                   || (renderer != Renderer::Meshes && !meshes.isEmpty());

    if (holding)
    {
        if (renderer != Renderer::Meshes)
            meshes.flush();

        if (renderer != Renderer::Quads)
            shapes.flush();

        ++rendererSwitches;
    }

    // Glyphs too, for a layer alone: a layer is placed over what came before it,
    // where text is otherwise left to composite above the fills around it.
    if (renderer == Renderer::Layers)
    {
        text.flush(pass);
        text.begin();
    }

    // A rect the primitive is wholly inside cuts nothing off it, so the change
    // can wait. A mask is coverage rather than a bound and gets no such elision.
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

    // The run's own reach, for the clip to be elided against.
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

    prepareToDraw(bounds);

    for (auto i = run.first; i < run.first + run.count; ++i)
    {
        auto placed = glyphs[i];
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

void DrawPlayer::playLayer(const DrawList& list,
                           const DrawCommand& command,
                           Point origin)
{
    const auto& recorded = list.getLayers()[command.first];

    if (recorded.layer == nullptr || recorded.layer->isEmpty())
        return;

    auto target = offsetBy(recorded.bounds, origin);

    prepareToDraw(target, Renderer::Layers);

    // Straight to the pass, clip and all: one layer is one draw.
    layers.draw(pass, *recorded.layer, target, clipMask, shapes.getAtlas());
}

void DrawPlayer::play(const DrawList& list, Point origin, const Rect& clipToUse)
{
    if (list.isEmpty())
        return;

    // No mask: a clip a component set for itself does not reach the next list.
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
        }
    }
}

void DrawPlayer::flush()
{
    // Shapes first, then glyphs: within one clip region text composites above
    // the fills. Only one of the two shape queues can hold anything by now.
    meshes.flush();
    shapes.flush();
    text.flush(pass);
    text.begin();
}
} // namespace eacp::UI
