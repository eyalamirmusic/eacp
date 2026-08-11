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
// has somewhere to land. A point is never smaller than a device pixel.
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
        // Nearest: a mask is drawn at the size it was rasterized, one texel to
        // one device pixel.
        maskAtlas.sampling = {GPU::TextureFilter::Nearest,
                              GPU::TextureAddressMode::Clamp};

        // Linear: a ramp is 256 texels stretched across the shape.
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

        auto position = origin + corner.x() * edgeX + corner.y() * edgeY;
        auto clipX = position.x() / screenSize.x() * 2.f - 1.f;
        auto clipY = 1.f - position.y() / screenSize.y() * 2.f;
        setPosition(float4(clipX, clipY, 0.f, 1.f));

        // Where this fragment sits inside the box, in points from its centre.
        // Runs past halfSize by the margin, which is the band the ramp needs.
        auto local = varying((corner - 0.5f) * (halfExtent * 2.f));

        auto fragHalfSize = varying(halfSize);
        auto fragColor = varying(color);
        auto fragShape = varying(shape);

        // The space the gradient was placed in, which the box is also in.
        auto fragPosition = varying(position);
        auto fragGradient = varying(gradient);
        auto fragGradientRamp = varying(gradientRamp);

        // A zero-sized rect collapses to one texel, which is what an unmasked
        // shape is given.
        auto maskUV = varying(mask.xy() + corner * mask.zw());

        auto radius = fragShape.x();
        auto borderWidth = fragShape.y();

        // The rounded-box field: distance to the box shrunk by the radius, then
        // let back out by it. Negative inside, positive outside.
        auto q = abs(local) - (fragHalfSize - float2(radius, radius));
        auto outside = length(max(0.f, q));
        auto inside = min(0.f, max(q.x(), q.y()));
        auto distance = outside + inside - radius;

        // The same field read as a ring, for a border.
        auto halfBorder = borderWidth * 0.5f;
        auto ring = abs(distance + halfBorder) - halfBorder;

        // Selected rather than branched: two pipelines cost a batch break.
        auto shapeDistance = mix(distance, ring, fragShape.z());

        // Half a device pixel each side of the edge.
        auto fieldCoverage = clamp(0.5f - shapeDistance * pixelScale, 0.f, 1.f);

        // Every shape pays this fetch, an unmasked one reading the opaque texel,
        // so paths and rectangles share a pipeline.
        auto coverage = fieldCoverage * sample(maskAtlas, maskUV).x();

        // And the clip's, out of the same texture.
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

    // Always blended, every edge being a coverage ramp. The mode accumulates the
    // target's alpha, which is identical on an opaque window and is what keeps
    // edges rendered into a Layer's transparent texture correct.
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

    // Rows are written once and never rewritten, so uploading mid-pass cannot
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

    // Past half the shorter side the field would fold through itself.
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

    // Nothing masks a distance-field shape, so it multiplies by one.
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

    // The mask's own footprint exactly: the soft edge is already in the coverage.
    instance.origin[0] = rect.x;
    instance.origin[1] = rect.y;
    instance.edgeX[0] = rect.w;
    instance.edgeX[1] = 0.f;
    instance.edgeY[0] = 0.f;
    instance.edgeY[1] = rect.h;

    instance.halfExtent[0] = rect.w * 0.5f;
    instance.halfExtent[1] = rect.h * 0.5f;

    // A field box larger than the quad, so every fragment lands well inside it
    // and the distance term multiplies by one, leaving the mask to decide.
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

void ShapeBatch::drawRect(const Rect& rect,
                          const Color& color,
                          float thickness,
                          float cornerRadius,
                          const GradientFill& gradient)
{
    if (thickness <= 0.f)
        return;

    // Drawn inside the edges, so an outline never grows the rect.
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

    auto midpoint = Point {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};

    auto origin = Point {midpoint.x - along.x * grownLength * 0.5f
                             - across.x * grownThickness * 0.5f,
                         midpoint.y - along.y * grownLength * 0.5f
                             - across.y * grownThickness * 0.5f};

    // A radius of half the thickness rounds the ends into caps.
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
