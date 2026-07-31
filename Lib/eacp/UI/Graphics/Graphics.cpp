#include "Graphics.h"

#include <algorithm>
#include <cmath>

namespace eacp::UI
{
Graphics::Graphics(ShapeBatch& shapesToUse,
                   MeshBatch& meshesToUse,
                   Text::TextRenderer& textToUse,
                   GPU::RenderPass& passToUse,
                   const Rect& surfaceToUse,
                   float backingScaleToUse)
    : shapes(shapesToUse)
    , meshes(meshesToUse)
    , hostText(textToUse)
    , text(&textToUse)
    , pass(passToUse)
    , surface(surfaceToUse)
    , backingScale(backingScaleToUse)
    , appliedClip(surfaceToUse)
{
    state.clip = surface;
}

void Graphics::setColour(const Color& colour)
{
    state.colour = colour;
}

Rect Graphics::toSurface(const Rect& rect) const
{
    return {rect.x + state.origin.x, rect.y + state.origin.y, rect.w, rect.h};
}

Point Graphics::toSurface(Point point) const
{
    return {point.x + state.origin.x, point.y + state.origin.y};
}

void Graphics::applyClip(const Rect& surfaceClip)
{
    // Every queue has to be drawn under the clip it was issued in, so the meshes
    // and the glyphs go out alongside the quads. setScissorRect flushes the
    // sprite queue itself; the text renderer has to be told, and then restarted
    // for the glyphs that follow.
    meshes.flush();
    shapes.flush();
    text->flush(pass);
    text->begin();

    if (sameRect(surfaceClip, surface))
        shapes.clearScissorRect();
    else
        shapes.setScissorRect({surfaceClip.x * backingScale,
                               surfaceClip.y * backingScale,
                               surfaceClip.w * backingScale,
                               surfaceClip.h * backingScale});

    appliedClip = surfaceClip;
    ++clipChanges;
}

void Graphics::prepareToDraw(const Rect& surfaceBounds, Renderer renderer)
{
    // Unconditionally, and before the clip is even looked at: what decides the
    // order two renderers come out in is which of them flushed first, so the one
    // not about to be used has to be emptied whether or not anything else about
    // the state changed. Counted when it had anything in it, that being exactly
    // the draw this alternation cost.
    auto quads = renderer == Renderer::Quads;

    if (quads ? !meshes.isEmpty() : !shapes.isEmpty())
    {
        if (quads)
            meshes.flush();
        else
            shapes.flush();

        ++rendererSwitches;
    }

    if (sameRect(appliedClip, state.clip))
        return;

    if (contains(state.clip, surfaceBounds) && contains(appliedClip, surfaceBounds))
        return;

    applyClip(state.clip);
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
    shapes.fillRect(target, state.colour, cornerRadius);
}

void Graphics::drawRect(const Rect& rect, float thickness)
{
    drawRoundedRect(rect, 0.f, thickness);
}

void Graphics::drawRoundedRect(const Rect& rect, float cornerRadius, float thickness)
{
    auto target = toSurface(rect);
    prepareToDraw(target);
    shapes.drawRect(target, state.colour, thickness, cornerRadius);
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
    shapes.drawLine(from, to, state.colour, thickness);
}

void Graphics::fillPath(const PathShape& shape)
{
    if (shape.isEmpty())
        return;

    auto target = toSurface(shape.getBounds());

    if (shape.isMeshed())
    {
        prepareToDraw(target, Renderer::Meshes);
        meshes.addMesh(shape.getMesh(), state.origin, state.colour);
        return;
    }

    prepareToDraw(target);
    shapes.fillMask(target, state.colour, shape.getMaskUV());
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
