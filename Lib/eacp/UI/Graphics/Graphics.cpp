#include "Graphics.h"

#include <algorithm>
#include <cmath>

namespace eacp::UI
{
Graphics::Graphics(ShapeBatch& shapesToUse,
                   MeshBatch& meshesToUse,
                   LayerRenderer& layersToUse,
                   GradientRamps& rampsToUse,
                   Text::TextRenderer& textToUse,
                   GPU::RenderPass& passToUse,
                   const Rect& surfaceToUse,
                   float backingScaleToUse)
    : shapes(shapesToUse)
    , meshes(meshesToUse)
    , layers(layersToUse)
    , ramps(rampsToUse)
    , hostText(textToUse)
    , text(&textToUse)
    , pass(passToUse)
    , surface(surfaceToUse)
    , backingScale(backingScaleToUse)
    , appliedClip(surfaceToUse)
{
    state.clip = surface;

    // The renderers outlive the painter, so a frame that ended inside a clip
    // would hand the next one its mask. The pass is new every frame and takes
    // its scissor with it; this does not, so it is put back by hand. Free when
    // there was none, which is every frame of an interface that never clips.
    shapes.setClipMask({});
    meshes.setClipMask({});
}

void Graphics::setColour(const Color& colour)
{
    state.colour = colour;
}

namespace
{
// The gradient's own space onto the space it is drawn in: what maps the unit
// segment from (0, 0) to (1, 0) onto a linear gradient's two ends, or the unit
// circle onto a radial one.
//
// The linear case's second axis is the first one turned a quarter, which makes
// the matrix a similarity and therefore invertible for any gradient with two
// distinct ends. Nothing reads the second coordinate afterwards, so what it is
// does not matter -- only that the matrix does not collapse.
GPUWidgets::AffineTransform placementOf(const Gradient& gradient)
{
    if (gradient.kind == Gradient::Kind::Radial)
        return {gradient.radius,
                0.f,
                0.f,
                gradient.radius,
                gradient.start.x,
                gradient.start.y};

    auto alongX = gradient.end.x - gradient.start.x;
    auto alongY = gradient.end.y - gradient.start.y;

    return {alongX, alongY, -alongY, alongX, gradient.start.x, gradient.start.y};
}
} // namespace

void Graphics::setGradient(const Gradient& gradient)
{
    auto rampV = ramps.rowFor(gradient);

    if (rampV < 0.f)
    {
        clearGradient();
        return;
    }

    // Placed and inverted here rather than at each draw, which is the whole
    // reason this is a state call: a gradient set once and filled twenty times
    // is one lookup and one inversion.
    //
    // The origin goes on the end, so the caller places the gradient in its own
    // component's points like everything else it draws.
    auto placement = placementOf(gradient)
                         .then(gradient.transform)
                         .then(GPUWidgets::AffineTransform::translation(
                             state.origin.x, state.origin.y));

    // A gradient whose ends coincide, or whose radius is zero, or that a
    // transform flattened: there is no space to map a fragment into, so it draws
    // as the flat colour rather than as a division by nothing.
    if (std::abs(placement.getDeterminant()) < 1e-12f)
    {
        clearGradient();
        return;
    }

    state.gradient.kind = gradient.kind == Gradient::Kind::Radial
                              ? GradientFill::Kind::Radial
                              : GradientFill::Kind::Linear;

    state.gradient.toGradientSpace = placement.inverted();
    state.gradient.spread = gradient.spread;
    state.gradient.rampV = rampV;
}

void Graphics::clearGradient()
{
    state.gradient = {};
}

Rect Graphics::toSurface(const Rect& rect) const
{
    return {rect.x + state.origin.x, rect.y + state.origin.y, rect.w, rect.h};
}

Point Graphics::toSurface(Point point) const
{
    return {point.x + state.origin.x, point.y + state.origin.y};
}

void Graphics::applyClip(bool changeScissor, bool changeMask)
{
    // Every queue has to be drawn under the clip it was issued in, so the meshes
    // and the glyphs go out alongside the quads. setScissorRect flushes the
    // sprite queue itself; the text renderer has to be told, and then restarted
    // for the glyphs that follow.
    meshes.flush();
    shapes.flush();
    text->flush(pass);
    text->begin();

    if (changeScissor)
    {
        if (sameRect(state.clip, surface))
            shapes.clearScissorRect();
        else
            shapes.setScissorRect({state.clip.x * backingScale,
                                   state.clip.y * backingScale,
                                   state.clip.w * backingScale,
                                   state.clip.h * backingScale});

        appliedClip = state.clip;
    }

    if (changeMask)
    {
        // Both renderers, and not only the one about to draw: a document's
        // clipped run may hold shapes of either kind, and whichever is told
        // second would otherwise draw the run before it under this clip.
        shapes.setClipMask(state.clipMask);
        meshes.setClipMask(state.clipMask);

        appliedClipMask = state.clipMask;
    }

    ++clipChanges;
}

void Graphics::prepareToDraw(const Rect& surfaceBounds, Renderer renderer)
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
                   || (renderer != Renderer::Meshes && !meshes.isEmpty());

    if (holding)
    {
        if (renderer != Renderer::Meshes)
            meshes.flush();

        if (renderer != Renderer::Quads)
            shapes.flush();

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
        text->flush(pass);
        text->begin();
    }

    // A rect the primitive is wholly inside cuts nothing off it, so the change
    // can wait for a primitive that the two rects actually disagree about --
    // which is the elision that keeps a deep tree at a handful of draws. A mask
    // gets no such reprieve: it is coverage rather than a bound, so a fragment
    // anywhere in the primitive may be the one it cuts.
    auto changeScissor = !sameRect(appliedClip, state.clip)
                         && !(contains(state.clip, surfaceBounds)
                              && contains(appliedClip, surfaceBounds));

    auto changeMask = !sameClipMask(appliedClipMask, state.clipMask);

    if (changeScissor || changeMask)
        applyClip(changeScissor, changeMask);
}

void Graphics::fillAll()
{
    fillRect(getClipBounds());
}

void Graphics::fillAll(const Color& colour)
{
    setColour(colour);
    fillAll();
}

void Graphics::fillRect(const Rect& rect)
{
    fillRoundedRect(rect, 0.f);
}

void Graphics::fillRoundedRect(const Rect& rect, float cornerRadius)
{
    auto target = toSurface(rect);
    prepareToDraw(target);
    shapes.fillRect(target, state.colour, cornerRadius, state.gradient);
}

void Graphics::drawRect(const Rect& rect, float thickness)
{
    drawRoundedRect(rect, 0.f, thickness);
}

void Graphics::drawRoundedRect(const Rect& rect, float cornerRadius, float thickness)
{
    auto target = toSurface(rect);
    prepareToDraw(target);
    shapes.drawRect(target, state.colour, thickness, cornerRadius, state.gradient);
}

void Graphics::drawLine(Point a, Point b, float thickness)
{
    auto from = toSurface(a);
    auto to = toSurface(b);

    auto bounds = Rect {std::min(from.x, to.x) - thickness,
                        std::min(from.y, to.y) - thickness,
                        std::abs(to.x - from.x) + thickness * 2.f,
                        std::abs(to.y - from.y) + thickness * 2.f};

    prepareToDraw(bounds);
    shapes.drawLine(from, to, state.colour, thickness, state.gradient);
}

void Graphics::fillPath(const PathShape& shape)
{
    if (shape.isEmpty())
        return;

    auto target = toSurface(shape.getBounds());

    if (shape.isMeshed())
    {
        prepareToDraw(target, Renderer::Meshes);
        meshes.addMesh(shape.getMesh(), state.origin, state.colour, state.gradient);
        return;
    }

    prepareToDraw(target);
    shapes.fillMask(target, state.colour, shape.getMaskUV(), state.gradient);
}

void Graphics::drawLayer(const Layer& layer)
{
    if (layer.isEmpty())
        return;

    auto target = toSurface(layer.getBounds());

    prepareToDraw(target, Renderer::Layers);

    // Straight to the pass rather than into a queue, and the clip goes with it
    // rather than being state the renderer holds: one layer is one draw, so
    // there is nothing to batch and nothing for a state change to break.
    layers.draw(pass, layer, target, state.clipMask, shapes.getAtlas());
}

float Graphics::drawText(std::string_view textToDraw, Point baselineLeft)
{
    auto pen = toSurface(baselineLeft);
    auto width = text->measure(textToDraw);

    prepareToDraw({pen.x, pen.y - ascent(), width, lineHeight()});

    return text->draw(textToDraw, pen, state.colour);
}

void Graphics::drawText(std::string_view textToDraw,
                        const Rect& area,
                        Justification justification)
{
    auto width = text->measure(textToDraw);

    auto x = area.x;

    if (justification == Justification::Centred)
        x = area.x + (area.w - width) * 0.5f;
    else if (justification == Justification::Right)
        x = area.right() - width;

    // Centre the line box on the area, then step down to the baseline, so a
    // caption sits optically centred rather than hanging off the top edge.
    auto baseline = area.y + (area.h - lineHeight()) * 0.5f + ascent();

    drawText(textToDraw, {x, baseline});
}

float Graphics::measureText(std::string_view textToMeasure) const
{
    return text->measure(textToMeasure);
}

float Graphics::lineHeight() const
{
    return text->lineHeight();
}

float Graphics::ascent() const
{
    return text->ascent();
}

// Both queues drain before the swap, for the same reason a clip change drains
// them: what was issued under the old renderer has to be drawn from the old
// renderer's atlas, and after this call there is no way back to it. The shapes
// go too, so a fill queued before a run of text still ends up underneath it.
void Graphics::setTextRenderer(Text::TextRenderer& renderer)
{
    if (text == &renderer)
        return;

    meshes.flush();
    shapes.flush();
    text->flush(pass);

    text = &renderer;

    // The incoming renderer has its own viewport and scale, and a document's
    // renderer is built once and used across resizes, so it is told rather than
    // assumed to know.
    text->setViewport({surface.w, surface.h}, backingScale);
    text->begin();
}

void Graphics::resetTextRenderer()
{
    setTextRenderer(hostText);
}

void Graphics::translate(float x, float y)
{
    state.origin.x += x;
    state.origin.y += y;
}

void Graphics::reduceClipRegion(const Rect& rect)
{
    state.clip = state.clip.intersection(toSurface(rect));
}

void Graphics::reduceClipToShape(const PathShape& shape)
{
    auto bounds = shape.getBounds();

    // A shape that has no coverage has no bounds either -- it was never
    // rasterized, or the atlas had no room for it -- and a clip that knows
    // nothing about where it is cannot narrow anything. Whatever rectangle the
    // caller reduced to before this stands, which is how a refused clip comes
    // out as the region's own box rather than as nothing at all.
    if (bounds.isEmpty())
    {
        state.clipMask = {};
        return;
    }

    // Narrowed by the bounds whether or not the mask itself can be used: they
    // are what a meshed clip comes to, and they are what the glyphs are cut by,
    // there being no way to sample a mask from the text pipeline.
    reduceClipRegion(bounds);

    if (shape.isEmpty() || shape.isMeshed())
    {
        state.clipMask = {};
        return;
    }

    state.clipMask = {toSurface(bounds), shape.getMaskUV()};
}

Rect Graphics::getClipBounds() const
{
    return {state.clip.x - state.origin.x,
            state.clip.y - state.origin.y,
            state.clip.w,
            state.clip.h};
}

bool Graphics::isClipEmpty() const
{
    return state.clip.isEmpty();
}

void Graphics::saveState()
{
    stack.add(state);
}

void Graphics::restoreState()
{
    if (stack.empty())
        return;

    state = stack.back();
    stack.erase(stack.end() - 1);
}

void Graphics::flush()
{
    // Shapes first, then glyphs: within one clip region text composites above
    // the fills, which is what a component drawing its own background and then
    // its own caption wants. See the module note on interleaving.
    //
    // Only one of the two shape queues can hold anything by now -- drawing into
    // either empties the other -- so the order between those two is whatever
    // order they were issued in, which is the one that matters.
    meshes.flush();
    shapes.flush();
    text->flush(pass);
    text->begin();
}
} // namespace eacp::UI
