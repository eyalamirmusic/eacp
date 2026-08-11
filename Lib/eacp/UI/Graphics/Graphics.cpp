#include "Graphics.h"

#include "../Render/Layer.h"

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

    state.font = text.getFont();
}

void Graphics::setColour(const Color& colour)
{
    state.colour = colour;
}

namespace
{
// Maps the unit segment (0,0)-(1,0) onto a linear gradient's ends, or the unit
// circle onto a radial one. The linear case's unused second axis is the first
// turned a quarter, keeping the matrix invertible for distinct ends.
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

    // The origin goes on the end, placing the gradient in the caller's points.
    auto placement = placementOf(gradient)
                         .then(gradient.transform)
                         .then(GPUWidgets::AffineTransform::translation(
                             state.origin.x, state.origin.y));

    constexpr auto smallestInvertibleDeterminant = 1e-12f;
    auto isDegenerate =
        std::abs(placement.getDeterminant()) < smallestInvertibleDeterminant;

    if (isDegenerate)
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

float Graphics::drawText(std::string_view textToDraw, Point baselineLeft)
{
    recordClip();

    // One walk of the string for both the glyphs and the advance.
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

    // Measured only where the answer is used.
    if (justification != Justification::Left)
    {
        auto width = text.measure(textToDraw, state.font);

        x = justification == Justification::Centred
                ? area.x + (area.w - width) * 0.5f
                : area.right() - width;
    }

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

void Graphics::reduceClipToShape(const PathShape& shape)
{
    auto bounds = shape.getBounds();

    // Never rasterized, or the atlas had no room: leave the caller's clip alone
    // rather than narrowing to nothing.
    if (bounds.isEmpty())
    {
        state.clipMask = {};
        return;
    }

    // Narrowed by the bounds whether or not the mask is usable: that is all a
    // meshed clip comes to, and all the text pipeline can be cut by.
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
