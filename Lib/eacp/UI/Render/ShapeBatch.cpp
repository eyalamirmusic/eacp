#include "ShapeBatch.h"

#include "ClipShader.h"
#include "GradientShader.h"

#include <algorithm>
#include <cmath>

namespace eacp::UI
{
namespace
{
// How far past its box a shape's quad reaches, in points, so the coverage ramp
// has somewhere to land. One point is enough at any scale: the ramp itself is a
// device pixel wide, and a point is never smaller than one.
constexpr auto antialiasMargin = 1.f;

constexpr ShapeVertex shapeUnitQuad[] = {
    {{0.f, 0.f}},
    {{1.f, 0.f}},
    {{1.f, 1.f}},
    {{0.f, 0.f}},
    {{1.f, 1.f}},
    {{0.f, 1.f}},
};

Point normalized(Point value)
{
    auto length = std::sqrt(value.x * value.x + value.y * value.y);

    if (length <= 0.f)
        return {1.f, 0.f};

    return {value.x / length, value.y / length};
}
} // namespace

struct ShapeBatch::Program final : GPU::ShaderProgram
{
    Program()
    {
        // Nearest, because a mask is drawn at exactly the size it was
        // rasterized: one texel lands on one device pixel, and filtering a 1:1
        // mapping can only blur coverage that is already exact.
        maskAtlas.sampling = {GPU::TextureFilter::Nearest,
                              GPU::TextureAddressMode::Clamp};

        // Linear, because a ramp is the opposite case: 256 texels stretched
        // across whatever the shape is, so the filtering is what stops a
        // gradient reading as the 256 bands it is stored as.
        gradientRamps.sampling = {GPU::TextureFilter::Linear,
                                  GPU::TextureAddressMode::Clamp};
        compile();
    }

    void define() override
    {
        auto corner = vertexInput(&ShapeVertex::corner);

        auto origin = instanceInput(&ShapeInstance::origin, 1);
        auto edgeX = instanceInput(&ShapeInstance::edgeX, 1);
        auto edgeY = instanceInput(&ShapeInstance::edgeY, 1);
        auto halfSize = instanceInput(&ShapeInstance::halfSize, 1);
        auto halfExtent = instanceInput(&ShapeInstance::halfExtent, 1);
        auto color = instanceInput(&ShapeInstance::color, 1);
        auto shape = instanceInput(&ShapeInstance::shape, 1);
        auto mask = instanceInput(&ShapeInstance::mask, 1);
        auto gradient = instanceInput(&ShapeInstance::gradient, 1);
        auto gradientRamp = instanceInput(&ShapeInstance::gradientRamp, 1);
        auto blur = instanceInput(&ShapeInstance::blur, 1);
        auto caster = instanceInput(&ShapeInstance::caster, 1);

        auto position = origin + corner.x() * edgeX + corner.y() * edgeY;
        auto clipX = position.x() / screenSize.x() * 2.f - 1.f;
        auto clipY = 1.f - position.y() / screenSize.y() * 2.f;
        setPosition(float4(clipX, clipY, 0.f, 1.f));

        // Where this fragment sits inside the box, in points, measured from its
        // centre. The quad spans the grown extent, so this runs past halfSize by
        // the margin -- which is exactly the band the ramp needs.
        auto local = varying((corner - 0.5f) * (halfExtent * 2.f));

        auto fragHalfSize = varying(halfSize);
        auto fragColor = varying(color);
        auto fragShape = varying(shape);

        // Where this fragment is in the space the gradient was placed in, which
        // is the same space the box is expressed in.
        auto fragPosition = varying(position);
        auto fragGradient = varying(gradient);
        auto fragGradientRamp = varying(gradientRamp);
        auto fragBlur = varying(blur);
        auto fragCaster = varying(caster);

        // Where in the atlas this fragment reads. A zero-sized rect collapses
        // to one texel however big the quad is, which is what an unmasked shape
        // is given.
        auto maskUV = varying(mask.xy() + corner * mask.zw());

        auto radius = fragShape.x();
        auto borderWidth = fragShape.y();

        // The rounded-box field: distance to the box shrunk by the radius, then
        // let back out by it. Negative inside, positive outside, and correct in
        // both -- which is what lets the same number serve the fill and the ring.
        auto roundedBox = [](auto point, auto halfSize, auto cornerRadius)
        {
            auto q = abs(point) - (halfSize - float2(cornerRadius, cornerRadius));
            auto outside = length(max(0.f, q));
            auto inside = min(0.f, max(q.x(), q.y()));

            return outside + inside - cornerRadius;
        };

        auto distance = roundedBox(local, fragHalfSize, radius);

        // The same field read as a ring, for a border: distance from the band
        // that sits just inside the edge.
        auto halfBorder = borderWidth * 0.5f;
        auto ring = abs(distance + halfBorder) - halfBorder;

        // Selected rather than branched, because a border and a fill differ only
        // in which distance is measured -- and carrying both is cheaper than two
        // pipelines and the batch break between them.
        auto shapeDistance = mix(distance, ring, fragShape.z());

        // Half a device pixel each side of the edge, which is the widest ramp
        // that still reads as a hard edge rather than a blur.
        auto hardEdge = clamp(0.5f - shapeDistance * pixelScale, 0.f, 1.f);

        // A shadow's edge, over a ramp as wide as twice its blur: the same
        // distance read through a curve instead of a line, which is what makes
        // it a blur rather than a fade. Smoothstep across twice the blur is
        // within a percent of a Gaussian of half of it, which is the blur CSS
        // and every design tool mean by the word.
        auto ramp = [&](auto edgeDistance)
        {
            return smoothstep(
                0.f, 1.f, clamp(0.5f - edgeDistance * fragBlur.x(), 0.f, 1.f));
        };

        // Along the box's own axes, and multiplied: a blurred rectangle is
        // exactly what it blurs to in x times what it blurs to in y, the
        // Gaussian being separable. Read off the field alone the corners come
        // out too bright, every direction out of one looking like an edge.
        auto axes = ramp(abs(local.x()) - fragHalfSize.x())
                    * ramp(abs(local.y()) - fragHalfSize.y());

        // The smaller of the two, which is the one that is right: the axes are
        // exact for a square box at any blur, the field for a rounded one at
        // none, and a rounded box is never more than the square box round it.
        //
        // And with no blur at all, the hard edge itself: a shadow thrown by an
        // offset or spread alone is the box's own shape, drawn where the box
        // is not, and the ramp would have nothing to run across.
        auto blurred = mix(
            hardEdge, min(ramp(shapeDistance), axes), step(0.0001f, fragBlur.x()));

        // The box that cast this shadow, in the shadow's own space: where the
        // shadow is *not*. CSS casts a shadow as though the box were opaque, so
        // an outer one is cut out of it -- otherwise a translucent card shows
        // its own shadow through itself -- and an inset one is the same field
        // the other way up, kept inside the box and nowhere else.
        auto casterGrow = fragCaster.z();
        auto casterHalf = fragHalfSize + float2(casterGrow, casterGrow);

        // Square when the shadow's corners are square, which they are exactly
        // when the box's own are: a spread rounds nothing that was not round.
        auto casterRadius = max(0.f, radius + casterGrow) * step(0.001f, radius);
        auto casterDistance =
            roundedBox(local - fragCaster.xy(), casterHalf, casterRadius);
        auto inCaster = clamp(0.5f - casterDistance * pixelScale, 0.f, 1.f);

        auto outerShadow = blurred * (1.f - inCaster);
        auto insetShadow = (1.f - blurred) * inCaster;

        // Mixed rather than branched, for the reason the border is: a shadow
        // and a fill are one pipeline, so a card and the shadow under it go out
        // in one instanced draw.
        auto shadow = mix(outerShadow, insetShadow, fragCaster.w());
        auto fieldCoverage = mix(hardEdge, shadow, fragBlur.y());

        // Every shape pays this fetch, and that is the deliberate trade: a
        // rectangle reads the one opaque texel -- in cache for the whole frame,
        // and multiplying by one -- so that a path and the rectangles around it
        // share a pipeline and go out in the same instanced draw. Branching to
        // skip it would buy back a cached read and cost a batch break.
        auto coverage = fieldCoverage * sample(maskAtlas, maskUV).x();

        // And the clip's, out of the same texture: a clipped shape is its own
        // coverage times the region's, which is one more fetch rather than a
        // stencil or a pass of its own. Unclipped, it reads the opaque texel
        // and multiplies by one -- see ClipShader.
        coverage =
            coverage * clipCoverage(fragPosition, clipRegion, clipMask, maskAtlas);

        auto fill = gradientFill(fragColor,
                                 fragPosition,
                                 fragGradient,
                                 fragGradientRamp,
                                 fragShape.w(),
                                 gradientRamps);

        setFragment(float4(fill.x(), fill.y(), fill.z(), fill.w() * coverage));
    }

    GPU::Uniform<GPU::Float2> screenSize;
    GPU::Uniform<GPU::Float> pixelScale;
    GPU::Uniform<GPU::Float4> clipRegion;
    GPU::Uniform<GPU::Float4> clipMask;
    GPU::Uniform<GPU::Texture2D> maskAtlas;
    GPU::Uniform<GPU::Texture2D> gradientRamps;

    EACP_SHADER(
        screenSize, pixelScale, clipRegion, clipMask, maskAtlas, gradientRamps)
};

ShapeBatch::ShapeBatch(const CoverageAtlas& atlasToUse,
                       GradientRamps& rampsToUse,
                       Point logicalSizeToUse,
                       float pixelScaleToUse,
                       int sampleCountToUse,
                       GPU::PixelFormat colorFormatToUse)
    : atlas(atlasToUse)
    , ramps(rampsToUse)
    , logicalSize(logicalSizeToUse)
    , pixelScale(pixelScaleToUse)
    , sampleCount(sampleCountToUse)
    , colorFormat(colorFormatToUse)
{
    program.create();
    program->setVertices(shapeUnitQuad);

    // Always blended: every edge this draws is a coverage ramp, and without
    // blending the antialiasing would punch holes in whatever is behind it.
    //
    // The mode that accumulates the target's own alpha rather than weighting the
    // source's by itself. Identical on the window, where the destination is
    // already opaque, and the difference between a correct layer and one whose
    // every antialiased edge composites too faintly -- see Layer, which renders
    // this same batch into a transparent texture.
    program->prepare(sampleCount,
                     false,
                     GPU::PrimitiveTopology::Triangles,
                     GPU::BlendMode::AlphaBlendOntoTransparent,
                     colorFormat);
}

ShapeBatch::~ShapeBatch()
{
    detach();
}

void ShapeBatch::begin(GPU::RenderPass& passToUse)
{
    detach();

    instances.clear();
    pass = &passToUse;

    passToUse.addParticipant(*this);
}

void ShapeBatch::end()
{
    flush();
    detach();
}

void ShapeBatch::flushInto(GPU::RenderPass&)
{
    flush();

    // Not detach(): the pass is walking its participant list right now and drops
    // every one of them itself once the walk is over.
    pass = nullptr;
}

void ShapeBatch::detach()
{
    if (pass == nullptr)
        return;

    pass->removeParticipant(*this);
    pass = nullptr;
}

void ShapeBatch::setLogicalSize(Point size)
{
    logicalSize = {std::max(1.f, size.x), std::max(1.f, size.y)};
}

void ShapeBatch::setPixelScale(float scale)
{
    pixelScale = scale > 0.f ? scale : 1.f;
}

void ShapeBatch::setScissorRect(const Rect& rectInPixels)
{
    flush();

    if (pass != nullptr)
        pass->setScissorRect(rectInPixels);
}

void ShapeBatch::clearScissorRect()
{
    flush();

    if (pass != nullptr)
        pass->clearScissorRect();
}

void ShapeBatch::setClipMask(const ClipMask& mask)
{
    if (sameClipMask(clip, mask))
        return;

    // Before the state changes, so what was queued is drawn under the clip it
    // was issued in.
    flush();

    clip = mask;
}

void ShapeBatch::flush()
{
    if (instances.empty() || pass == nullptr)
        return;

    // The rows baked while this run was being queued, uploaded before the draw
    // that reads them. A row is written once and never rewritten, so this cannot
    // disturb a draw already recorded.
    ramps.commit();

    program->screenSize = Array {logicalSize.x, logicalSize.y};
    program->pixelScale = pixelScale;

    packClipMask(clip,
                 atlas.getOpaqueUV(),
                 program->clipRegion.value,
                 program->clipMask.value);

    program->maskAtlas = atlas.getTexture();
    program->gradientRamps = ramps.getTexture();
    program->setInstances(1, instances.data(), instances.size());

    pass->drawInstanced(*program, instances.size());

    instances.clear();
}

void ShapeBatch::addShape(Point origin,
                          Point edgeX,
                          Point edgeY,
                          Point halfSize,
                          const Color& color,
                          float cornerRadius,
                          float borderWidth,
                          const GradientFill& gradient)
{
    if (halfSize.x <= 0.f || halfSize.y <= 0.f || color.a <= 0.f)
        return;

    // A radius past half the shorter side is not a rounder corner, it is a
    // different shape -- and the field would fold through itself drawing it.
    auto radius = std::clamp(cornerRadius, 0.f, std::min(halfSize.x, halfSize.y));

    auto instance = ShapeInstance {};

    instance.origin[0] = origin.x;
    instance.origin[1] = origin.y;
    instance.edgeX[0] = edgeX.x;
    instance.edgeX[1] = edgeX.y;
    instance.edgeY[0] = edgeY.x;
    instance.edgeY[1] = edgeY.y;

    instance.halfSize[0] = halfSize.x;
    instance.halfSize[1] = halfSize.y;
    instance.halfExtent[0] = halfSize.x + antialiasMargin;
    instance.halfExtent[1] = halfSize.y + antialiasMargin;

    instance.color[0] = color.r;
    instance.color[1] = color.g;
    instance.color[2] = color.b;
    instance.color[3] = color.a;

    instance.shape[0] = radius;
    instance.shape[1] = std::max(0.f, borderWidth);
    instance.shape[2] = borderWidth > 0.f ? 1.f : 0.f;

    // Nothing masks a distance-field shape, so it reads the atlas's opaque
    // texel and its own coverage survives untouched.
    setMask(instance, atlas.getOpaqueUV());
    setGradient(instance, gradient);

    instances.add(instance);
}

void ShapeBatch::setMask(ShapeInstance& instance, const Rect& maskUV)
{
    instance.mask[0] = maskUV.x;
    instance.mask[1] = maskUV.y;
    instance.mask[2] = maskUV.w;
    instance.mask[3] = maskUV.h;
}

void ShapeBatch::setGradient(ShapeInstance& instance, const GradientFill& gradient)
{
    instance.shape[3] =
        packGradient(gradient, instance.gradient, instance.gradientRamp);
}

void ShapeBatch::fillMask(const Rect& rect,
                          const Color& color,
                          const Rect& maskUV,
                          const GradientFill& gradient)
{
    if (rect.w <= 0.f || rect.h <= 0.f || color.a <= 0.f)
        return;

    auto instance = ShapeInstance {};

    // The quad is the mask's own footprint exactly - no margin, since the soft
    // edge is already in the coverage rather than something the field adds.
    instance.origin[0] = rect.x;
    instance.origin[1] = rect.y;
    instance.edgeX[0] = rect.w;
    instance.edgeX[1] = 0.f;
    instance.edgeY[0] = 0.f;
    instance.edgeY[1] = rect.h;

    instance.halfExtent[0] = rect.w * 0.5f;
    instance.halfExtent[1] = rect.h * 0.5f;

    // A field box larger than the quad by the ramp's own width, so every
    // fragment lands well inside it and the distance term multiplies by one.
    // The mask is then the only thing deciding coverage, which is what a path
    // rasterized to the exact fraction of each pixel it covers needs.
    instance.halfSize[0] = instance.halfExtent[0] + antialiasMargin;
    instance.halfSize[1] = instance.halfExtent[1] + antialiasMargin;

    instance.color[0] = color.r;
    instance.color[1] = color.g;
    instance.color[2] = color.b;
    instance.color[3] = color.a;

    setMask(instance, maskUV);
    setGradient(instance, gradient);

    instances.add(instance);
}

void ShapeBatch::addAxisAlignedShape(const Rect& rect,
                                     const Color& color,
                                     float cornerRadius,
                                     float borderWidth,
                                     const GradientFill& gradient)
{
    auto halfSize = Point {rect.w * 0.5f, rect.h * 0.5f};
    auto grownWidth = rect.w + antialiasMargin * 2.f;
    auto grownHeight = rect.h + antialiasMargin * 2.f;

    addShape({rect.x - antialiasMargin, rect.y - antialiasMargin},
             {grownWidth, 0.f},
             {0.f, grownHeight},
             halfSize,
             color,
             cornerRadius,
             borderWidth,
             gradient);
}

void ShapeBatch::fillRect(const Rect& rect,
                          const Color& color,
                          float cornerRadius,
                          const GradientFill& gradient)
{
    addAxisAlignedShape(rect, color, cornerRadius, 0.f, gradient);
}

void ShapeBatch::fillShadow(const ShadowShape& shadow,
                            const Color& color,
                            const GradientFill& gradient)
{
    const auto& box = shadow.box;

    if (box.w <= 0.f || box.h <= 0.f || color.a <= 0.f)
        return;

    // Where the shadow itself falls: the box moved by the offset, and let out
    // past it by the spread -- or, inset, held in from it by the spread. A
    // spread that swallows the box leaves a sliver rather than nothing, which
    // is what an inset shadow deep enough to fill its box asks for.
    auto grow = shadow.inset ? -shadow.spread : shadow.spread;
    auto blur = std::max(0.f, shadow.blurRadius);

    auto half = Point {std::max(0.01f, box.w * 0.5f + grow),
                       std::max(0.01f, box.h * 0.5f + grow)};

    if (!shadow.inset && (box.w * 0.5f + grow <= 0.f || box.h * 0.5f + grow <= 0.f))
        return;

    // Let out with the box, so a spread rounds nothing that was square and a
    // rounded box keeps the shape of its corners however far its shadow
    // reaches. The shader reads the caster's radius back out of this one.
    auto radius =
        shadow.cornerRadius > 0.f ? std::max(0.f, shadow.cornerRadius + grow) : 0.f;

    radius = std::clamp(radius, 0.f, std::min(half.x, half.y));

    auto centre = Point {box.x + box.w * 0.5f + shadow.offset.x,
                         box.y + box.h * 0.5f + shadow.offset.y};

    // The quad, centred on the shadow as every shape's quad is centred on its
    // own field: as far as the blur reaches past an outer shadow, and as far
    // as the box it is held inside for an inset one.
    auto reach = shadow.inset ? Point {box.w * 0.5f + std::abs(shadow.offset.x),
                                       box.h * 0.5f + std::abs(shadow.offset.y)}
                              : Point {half.x + blur, half.y + blur};

    auto instance = ShapeInstance {};

    instance.origin[0] = centre.x - reach.x - antialiasMargin;
    instance.origin[1] = centre.y - reach.y - antialiasMargin;
    instance.edgeX[0] = (reach.x + antialiasMargin) * 2.f;
    instance.edgeY[1] = (reach.y + antialiasMargin) * 2.f;

    instance.halfSize[0] = half.x;
    instance.halfSize[1] = half.y;
    instance.halfExtent[0] = reach.x + antialiasMargin;
    instance.halfExtent[1] = reach.y + antialiasMargin;

    instance.color[0] = color.r;
    instance.color[1] = color.g;
    instance.color[2] = color.b;
    instance.color[3] = color.a;

    instance.shape[0] = radius;

    // One over twice the blur, so the ramp runs from opaque a blur inside the
    // shadow's edge to nothing a blur outside it; and the flag that chooses
    // it, which the CPU knows and the fragment stage would otherwise compare
    // for.
    instance.blur[0] = blur > 0.f ? 0.5f / blur : 0.f;
    instance.blur[1] = 1.f;

    // The box that cast it, against the shadow's own centre: where the shadow
    // is not drawn.
    instance.caster[0] = -shadow.offset.x;
    instance.caster[1] = -shadow.offset.y;
    instance.caster[2] = -grow;
    instance.caster[3] = shadow.inset ? 1.f : 0.f;

    setMask(instance, atlas.getOpaqueUV());
    setGradient(instance, gradient);

    instances.add(instance);
}

void ShapeBatch::drawRect(const Rect& rect,
                          const Color& color,
                          float thickness,
                          float cornerRadius,
                          const GradientFill& gradient)
{
    if (thickness <= 0.f)
        return;

    // Drawn inside the edges, so the outline of a rect never grows it. The field
    // is measured from the rect's own boundary, so the ring lands within it.
    addAxisAlignedShape(rect, color, cornerRadius, thickness, gradient);
}

void ShapeBatch::drawLine(Point a,
                          Point b,
                          const Color& color,
                          float thickness,
                          const GradientFill& gradient)
{
    if (thickness <= 0.f)
        return;

    auto delta = Point {b.x - a.x, b.y - a.y};
    auto length = std::sqrt(delta.x * delta.x + delta.y * delta.y);

    if (length <= 0.f)
        return;

    auto along = normalized(delta);
    auto across = Point {-along.y, along.x};

    auto halfLength = length * 0.5f;
    auto halfThickness = thickness * 0.5f;

    auto grownLength = length + antialiasMargin * 2.f;
    auto grownThickness = thickness + antialiasMargin * 2.f;

    // The grown quad's first corner: back off from the line's midpoint by half
    // the grown span along each of its own axes.
    auto midpoint = Point {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};

    auto origin = Point {midpoint.x - along.x * grownLength * 0.5f
                             - across.x * grownThickness * 0.5f,
                         midpoint.y - along.y * grownLength * 0.5f
                             - across.y * grownThickness * 0.5f};

    // A radius of half the thickness rounds the ends into caps, which is what a
    // line drawn by hand has and what a square-ended one visibly lacks.
    addShape(origin,
             {along.x * grownLength, along.y * grownLength},
             {across.x * grownThickness, across.y * grownThickness},
             {halfLength, halfThickness},
             color,
             halfThickness,
             0.f,
             gradient);
}
} // namespace eacp::UI
