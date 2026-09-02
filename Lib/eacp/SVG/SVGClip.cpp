#include "SVGClip.h"

#include "SVGGeometry.h"

#include <algorithm>
#include <cmath>

namespace eacp::SVG
{
namespace
{
// A <use> inside a clipPath may name something that contains it, the same way
// one in the document may. Shallower than the document's limit because a clip
// that nests at all is already unusual.
constexpr int maxClipUseDepth = 4;

GPUWidgets::FillRule parseClipRule(const std::string& value)
{
    return value == "evenodd" ? GPUWidgets::FillRule::EvenOdd
                              : GPUWidgets::FillRule::NonZero;
}

bool usesBoundingBox(const SVGElement& clipPath)
{
    return clipPath.attr("clipPathUnits") == "objectBoundingBox";
}

// The space the clipPath's children are authored in: the referencing element's
// own units, or the unit square mapped onto its bounding box.
GPUWidgets::AffineTransform contentTransform(const SVGElement& clipPath,
                                             const Graphics::Rect& objectBounds)
{
    if (!usesBoundingBox(clipPath))
        return {};

    return {
        objectBounds.w, 0.f, 0.f, objectBounds.h, objectBounds.x, objectBounds.y};
}

// What a percentage inside the clipPath is a fraction of. In bounding-box units
// the coordinates are already fractions of the box, so a hundred percent is one
// of them; in user space it is the viewport the reference was written in, the
// same rule a gradient's own units follow.
Viewport contentViewport(const SVGElement& clipPath, const Viewport& viewport)
{
    return usesBoundingBox(clipPath) ? Viewport {1.f, 1.f} : viewport;
}

void addClipChild(const SVGElement& element,
                  const ElementsById& byId,
                  const GPUWidgets::AffineTransform& inherited,
                  const Viewport& viewport,
                  float flatness,
                  GPUWidgets::Path& region,
                  int depth)
{
    auto transform = inherited;

    auto own = element.attr("transform");

    if (!own.empty())
        transform = parseTransformMatrix(own).then(transform);

    // A <use> in a clipPath is what a document writes when the same outline
    // clips several things; the referenced element is built here rather than
    // shared, since it is being flattened into one region either way.
    if (element.tag == "use")
    {
        if (depth >= maxClipUseDepth)
            return;

        auto found = byId.find(hrefId(element));

        if (found == byId.end())
            return;

        auto placed = GPUWidgets::AffineTransform::translation(
                          lengthAttr(element, "x", viewport, LengthAxis::Horizontal),
                          lengthAttr(element, "y", viewport, LengthAxis::Vertical))
                          .then(transform);

        addClipChild(
            *found->second, byId, placed, viewport, flatness, region, depth + 1);
        return;
    }

    if (!isShapeTag(element.tag))
        return;

    // A degenerate transform has no geometry to flatten against, and dividing
    // the tolerance by it would ask for an unbounded number of segments.
    auto scale = transform.getScaleFactor();

    if (scale <= 0.f)
        return;

    auto path = buildGeometry(element, viewport, flatness / scale);

    if (path.isEmpty())
        return;

    region.append(path.transformed(transform));
}
} // namespace

ClipRegion resolveClipPath(const std::string& reference,
                           const ElementsById& byId,
                           const Graphics::Rect& objectBounds,
                           const Viewport& viewport,
                           float flatness)
{
    auto result = ClipRegion {};

    if (reference.empty())
        return result;

    auto found = byId.find(reference);

    if (found == byId.end() || found->second->tag != "clipPath")
        return result;

    const auto& clipPath = *found->second;

    result.resolved = true;

    auto base = contentTransform(clipPath, objectBounds);
    auto content = contentViewport(clipPath, viewport);

    // The clipPath's own clip-rule, which its children inherit in the CSS sense
    // -- a document setting it once on the container is the usual spelling.
    auto container = PropertyReader {clipPath}("clip-rule");

    auto contributed = 0;
    auto lastRule = GPUWidgets::FillRule::NonZero;

    for (const auto& child: clipPath.children)
    {
        auto before = result.path.getSubPaths().size();

        addClipChild(child, byId, base, content, flatness, result.path, 0);

        if (result.path.getSubPaths().size() == before)
            continue;

        ++contributed;

        auto own = PropertyReader {child}("clip-rule");
        lastRule = parseClipRule(own.empty() ? container : own);
    }

    // One child's rule is its own; several children are a union, and a union is
    // what non-zero computes -- under even-odd every overlap between two of them
    // would read as a hole in the clip.
    result.rule = contributed == 1 ? lastRule : GPUWidgets::FillRule::NonZero;

    return result;
}

bool clipUsesBoundingBox(const std::string& reference, const ElementsById& byId)
{
    auto found = byId.find(reference);

    return found != byId.end() && found->second->tag == "clipPath"
           && usesBoundingBox(*found->second);
}

std::optional<Graphics::Rect> asAxisAlignedRect(const GPUWidgets::Path& path)
{
    const auto& subPaths = path.getSubPaths();

    if (subPaths.size() != 1)
        return {};

    const auto& points = subPaths[0].points;
    auto count = points.size();

    auto bounds = path.getBounds();

    if (bounds.w <= 0.f || bounds.h <= 0.f)
        return {};

    // Relative, because these points have been through a transform: a rectangle
    // mapped by a matrix built from a translate and a scale lands a few ulps off
    // the corners it was authored at, and at a document's scale that is a
    // fraction of a millionth of the shape.
    auto tolerance = std::max(bounds.w, bounds.h) * 1e-4f;

    auto isNear = [tolerance](float a, float b)
    { return std::abs(a - b) <= tolerance; };

    // Four corners, or the five a document writes when it closes the outline by
    // repeating the first point rather than by saying so.
    if (count == 5 && isNear(points[0].x, points[4].x)
        && isNear(points[0].y, points[4].y))
        count = 4;

    if (count != 4)
        return {};

    for (auto index = 0; index < 4; ++index)
    {
        const auto& point = points[index];

        auto onCorner =
            (isNear(point.x, bounds.x) || isNear(point.x, bounds.right()))
            && (isNear(point.y, bounds.y) || isNear(point.y, bounds.bottom()));

        if (!onCorner)
            return {};

        // Each edge along one axis and not the other. Four points that are all
        // corners and whose every edge moves in exactly one direction can only
        // be the rectangle they bound -- and the "not the other" is what rejects
        // a degenerate outline that visits one corner twice.
        const auto& next = points[(index + 1) % 4];

        if (isNear(point.x, next.x) == isNear(point.y, next.y))
            return {};
    }

    return bounds;
}
} // namespace eacp::SVG
