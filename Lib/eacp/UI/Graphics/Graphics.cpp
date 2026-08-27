#include "Graphics.h"

#include "../Render/Layer.h"

#include <algorithm>
#include <cmath>

namespace eacp::UI
{
Graphics::Graphics(DrawList& listToUse,
                   GradientRamps& rampsToUse,
                   Text::TextRenderer& textToUse,
                   const Rect& bounds)
    : list(listToUse)
    , ramps(rampsToUse)
    , text(textToUse)
    , recordedClip(bounds)
{
    state.clip = bounds;

    // The host's face is where every component starts, so a tree that never
    // mentions a font looks like one thing.
    state.font = text.getFont();
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

Rect Graphics::toLocal(const Rect& rect) const
{
    return {rect.x + state.origin.x, rect.y + state.origin.y, rect.w, rect.h};
}

Point Graphics::toLocal(Point point) const
{
    return {point.x + state.origin.x, point.y + state.origin.y};
}

void Graphics::recordClip()
{
    if (sameRect(recordedClip, state.clip)
        && sameClipMask(recordedClipMask, state.clipMask))
        return;

    list.addClip({state.clip, state.clipMask});

    recordedClip = state.clip;
    recordedClipMask = state.clipMask;
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
    recordClip();

    auto shape = ShapeDraw {};
    shape.kind = ShapeDraw::Kind::Fill;
    shape.rect = toLocal(rect);
    shape.colour = state.colour;
    shape.cornerRadius = cornerRadius;
    shape.gradient = state.gradient;

    list.addShape(shape);
}

void Graphics::drawRect(const Rect& rect, float thickness)
{
    drawRoundedRect(rect, 0.f, thickness);
}

void Graphics::drawRoundedRect(const Rect& rect, float cornerRadius, float thickness)
{
    recordClip();

    auto shape = ShapeDraw {};
    shape.kind = ShapeDraw::Kind::Border;
    shape.rect = toLocal(rect);
    shape.colour = state.colour;
    shape.cornerRadius = cornerRadius;
    shape.thickness = thickness;
    shape.gradient = state.gradient;

    list.addShape(shape);
}

void Graphics::drawLine(Point a, Point b, float thickness)
{
    recordClip();

    auto shape = ShapeDraw {};
    shape.kind = ShapeDraw::Kind::Line;
    shape.from = toLocal(a);
    shape.to = toLocal(b);
    shape.colour = state.colour;
    shape.thickness = thickness;
    shape.gradient = state.gradient;

    list.addShape(shape);
}

void Graphics::fillPath(const PathShape& shape)
{
    if (shape.isEmpty())
        return;

    recordClip();

    auto target = toLocal(shape.getBounds());

    if (shape.isMeshed())
    {
        list.addMesh(
            shape.getMesh(), state.origin, target, state.colour, state.gradient);
        return;
    }

    auto masked = ShapeDraw {};
    masked.kind = ShapeDraw::Kind::Mask;
    masked.rect = target;
    masked.colour = state.colour;
    masked.maskUV = shape.getMaskUV();
    masked.gradient = state.gradient;

    list.addShape(masked);
}

void Graphics::drawLayer(const Layer& layer)
{
    if (layer.isEmpty())
        return;

    recordClip();

    list.addLayer(layer, toLocal(layer.getBounds()));
}

void Graphics::drawImage(const ImageRef& image, const Rect& dest, float opacity)
{
    if (image == nullptr)
        return;

    drawImage(image,
              {0.f, 0.f, (float) image->width, (float) image->height},
              dest,
              opacity);
}

void Graphics::drawImage(const ImageRef& image,
                         const Rect& source,
                         const Rect& dest,
                         float opacity)
{
    if (image == nullptr || image->width <= 0 || image->height <= 0 || dest.w <= 0.f
        || dest.h <= 0.f || opacity <= 0.f)
        return;

    recordClip();

    auto width = (float) image->width;
    auto height = (float) image->height;

    auto uv = Rect {
        source.x / width, source.y / height, source.w / width, source.h / height};

    list.addImage({image, toLocal(dest), uv, std::clamp(opacity, 0.f, 1.f)});
}

float Graphics::drawText(std::string_view textToDraw, Point baselineLeft)
{
    recordClip();

    // One walk of the string for both the glyphs and the advance, and it happens
    // here rather than at every frame that draws them -- which is the largest
    // single thing recording buys. A label used to be laid out three times a
    // frame: once to measure for the justification, once to measure for the clip
    // rect, and once to emit.
    auto& glyphs = list.glyphStorage();
    auto first = glyphs.size();

    auto advance =
        text.layoutInto(glyphs, textToDraw, toLocal(baselineLeft), state.font);

    list.addGlyphRun(first, state.colour);

    return advance;
}

void Graphics::drawText(std::string_view textToDraw,
                        const Rect& area,
                        Justification justification)
{
    auto x = area.x;

    // Measured only where the answer is used. A left-justified caption is the
    // common case and its pen is the box's own edge, so the walk that would work
    // out how wide it is has nothing to decide.
    if (justification != Justification::Left)
    {
        auto width = text.measure(textToDraw, state.font);

        x = justification == Justification::Centred
                ? area.x + (area.w - width) * 0.5f
                : area.right() - width;
    }

    // Centre the line box on the area, then step down to the baseline, so a
    // caption sits optically centred rather than hanging off the top edge.
    auto baseline = area.y + (area.h - lineHeight()) * 0.5f + ascent();

    drawText(textToDraw, {x, baseline});
}

float Graphics::measureText(std::string_view textToMeasure) const
{
    return text.measure(textToMeasure, state.font);
}

float Graphics::lineHeight() const
{
    return text.lineHeight(state.font);
}

float Graphics::ascent() const
{
    return text.ascent(state.font);
}

void Graphics::setFont(const Font& font)
{
    state.font = font;
}

void Graphics::setFontSize(float pointSize)
{
    state.font.pointSize = pointSize;
}

void Graphics::setFontStyle(FontStyle style)
{
    state.font.style = style;
}

void Graphics::translate(float x, float y)
{
    state.origin.x += x;
    state.origin.y += y;
}

void Graphics::reduceClipRegion(const Rect& rect)
{
    state.clip = state.clip.intersection(toLocal(rect));
}

void Graphics::paintOver()
{
    list.addFence();
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

    state.clipMask = {toLocal(bounds), shape.getMaskUV()};
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
} // namespace eacp::UI
