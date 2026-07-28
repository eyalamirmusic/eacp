#include "ShapeBatch.h"

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

constexpr ShapeVertex unitQuad[] = {
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
    Program() { compile(); }

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

        auto radius = fragShape.x();
        auto borderWidth = fragShape.y();

        // The rounded-box field: distance to the box shrunk by the radius, then
        // let back out by it. Negative inside, positive outside, and correct in
        // both -- which is what lets the same number serve the fill and the ring.
        auto q = abs(local) - (fragHalfSize - float2(radius, radius));
        auto outside = length(max(0.f, q));
        auto inside = min(0.f, max(q.x(), q.y()));
        auto distance = outside + inside - radius;

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
        auto coverage = clamp(0.5f - shapeDistance * pixelScale, 0.f, 1.f);

        setFragment(float4(
            fragColor.x(), fragColor.y(), fragColor.z(), fragColor.w() * coverage));
    }

    GPU::Uniform<GPU::Float2> screenSize;
    GPU::Uniform<GPU::Float> pixelScale;

    EACP_SHADER(screenSize, pixelScale)
};

ShapeBatch::ShapeBatch(Point logicalSizeToUse,
                       float pixelScaleToUse,
                       int sampleCountToUse,
                       GPU::PixelFormat colorFormatToUse)
    : logicalSize(logicalSizeToUse)
    , pixelScale(pixelScaleToUse)
    , sampleCount(sampleCountToUse)
    , colorFormat(colorFormatToUse)
{
    program.create();
    program->setVertices(unitQuad);

    // Always blended: every edge this draws is a coverage ramp, and without
    // blending the antialiasing would punch holes in whatever is behind it.
    program->prepare(sampleCount,
                     false,
                     GPU::PrimitiveTopology::Triangles,
                     GPU::BlendMode::AlphaBlend,
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

void ShapeBatch::flush()
{
    if (instances.empty() || pass == nullptr)
        return;

    program->screenSize = Array {logicalSize.x, logicalSize.y};
    program->pixelScale = pixelScale;
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
                          float borderWidth)
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
    instance.shape[3] = 0.f;

    instances.add(instance);
}

void ShapeBatch::addAxisAlignedShape(const Rect& rect,
                                     const Color& color,
                                     float cornerRadius,
                                     float borderWidth)
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
             borderWidth);
}

void ShapeBatch::fillRect(const Rect& rect, const Color& color, float cornerRadius)
{
    addAxisAlignedShape(rect, color, cornerRadius, 0.f);
}

void ShapeBatch::drawRect(const Rect& rect,
                          const Color& color,
                          float thickness,
                          float cornerRadius)
{
    if (thickness <= 0.f)
        return;

    // Drawn inside the edges, so the outline of a rect never grows it. The field
    // is measured from the rect's own boundary, so the ring lands within it.
    addAxisAlignedShape(rect, color, cornerRadius, thickness);
}

void ShapeBatch::drawLine(Point a, Point b, const Color& color, float thickness)
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
             0.f);
}
} // namespace eacp::UI
