#include "SVGComponent.h"

#include "SVGAttributes.h"
#include "SVGPathParser.h"

#include <algorithm>

namespace eacp::SVG
{
// Everything an element inherits from the tree above it.
//
// The whole reason it exists: SVGBuilder reads fill straight off the element
// with no walk to the parent, so a `<g fill="red">` colours nothing and every
// child of it comes out black. Most real documents set fill on a group, which
// makes that one bug enough to render an illustration in the wrong colours from
// end to end.
struct SVGComponent::Style
{
    Graphics::Color fill = Graphics::Color::black();
    Graphics::Color stroke = Graphics::Color::black();
    bool hasFill = true;
    bool hasStroke = false;

    // Multiplied into the colours rather than composited. Correct for a single
    // element, and an approximation for a group: fading a subtree as a unit
    // means drawing it to a texture and fading that, which is a different
    // feature wearing the same attribute name.
    float opacity = 1.f;
    float fillOpacity = 1.f;
    float strokeOpacity = 1.f;

    GPUWidgets::FillRule fillRule = GPUWidgets::FillRule::NonZero;

    // Width in the document's units, converted when the region is built.
    GPUWidgets::StrokeStyle strokeStyle;

    std::string fontFamily = UI::defaultUIFontFamily();
    float fontSize = 16.f;

    // The document's units onto this component's points, composed down the
    // tree. Baked into the geometry rather than applied to anything at draw
    // time, which is what lets the whole document be one component.
    GPUWidgets::AffineTransform transform;
};

namespace
{
// Path's own default: the tolerance that holds a flattened curve inside a tenth
// of a device pixel at the usual 2x. Read off a default-constructed path rather
// than restated, so the two cannot drift.
float fillFlatness()
{
    return GPUWidgets::Path {}.getFlatness();
}

// Ten times tighter, for geometry about to be stroked. Offsetting a polyline
// amplifies whatever error the flattening left in it, and both edges of the
// stroke carry it -- see the note on strokeToFill, which measured the
// difference.
float strokeFlatness()
{
    return fillFlatness() * 0.1f;
}

// The first name of a font-family list, unquoted. "Georgia, 'Times New Roman',
// serif" is one face and two fallbacks, and the tier below has no way to be
// handed a fallback chain.
std::string firstFontFamily(const std::string& value)
{
    auto end = value.find(',');
    auto first = value.substr(0, end);

    auto isTrimmable = [](char c)
    { return std::isspace(static_cast<unsigned char>(c)) || c == '\'' || c == '"'; };

    while (!first.empty() && isTrimmable(first.front()))
        first.erase(first.begin());

    while (!first.empty() && isTrimmable(first.back()))
        first.pop_back();

    return first;
}

GPUWidgets::FillRule parseFillRule(const std::string& value)
{
    return value == "evenodd" ? GPUWidgets::FillRule::EvenOdd
                              : GPUWidgets::FillRule::NonZero;
}

GPUWidgets::LineCap parseLineCap(const std::string& value)
{
    if (value == "round")
        return GPUWidgets::LineCap::Round;
    if (value == "square")
        return GPUWidgets::LineCap::Square;

    return GPUWidgets::LineCap::Butt;
}

GPUWidgets::LineJoin parseLineJoin(const std::string& value)
{
    if (value == "round")
        return GPUWidgets::LineJoin::Round;
    if (value == "bevel")
        return GPUWidgets::LineJoin::Bevel;

    return GPUWidgets::LineJoin::Miter;
}

void applyColour(const std::string& value, Graphics::Color& colour, bool& present)
{
    if (value.empty())
        return;

    auto parsed = parseColor(value);
    present = !parsed.isNone;

    if (present)
        colour = parsed.color;
}

void applyNumber(const std::string& value, float& target)
{
    if (!value.empty())
        target = Strings::parseFloatOr(value, target);
}

void addRectGeometry(GPUWidgets::Path& path, const SVGElement& element)
{
    auto rect = Graphics::Rect {element.numAttr("x"),
                                element.numAttr("y"),
                                element.numAttr("width"),
                                element.numAttr("height")};

    if (rect.w <= 0.f || rect.h <= 0.f)
        return;

    auto rx = element.numAttr("rx");
    auto ry = element.numAttr("ry");

    // Either radius alone stands for both, which is what the format says.
    if (rx <= 0.f)
        rx = ry;
    if (ry <= 0.f)
        ry = rx;

    // Path rounds corners with one radius rather than two, so an elliptical
    // corner becomes the circular one that fits inside it.
    if (rx > 0.f && ry > 0.f)
        path.addRoundedRect(rect, std::min(rx, ry));
    else
        path.addRect(rect);
}

void addEllipseGeometry(
    GPUWidgets::Path& path, float cx, float cy, float rx, float ry)
{
    if (rx <= 0.f || ry <= 0.f)
        return;

    path.addEllipse({cx - rx, cy - ry, rx * 2.f, ry * 2.f});
}

void addPolylineGeometry(GPUWidgets::Path& path,
                         const SVGElement& element,
                         bool closed)
{
    auto points = parsePointList(element.attr("points"));

    if (points.empty())
        return;

    path.moveTo(points[0]);

    for (auto i = 1; i < points.size(); ++i)
        path.lineTo(points[i]);

    if (closed)
        path.close();
}

// The element's geometry in the document's own units, flattened to `flatness`.
//
// Untransformed on purpose. The caller maps it afterwards, which for a stroke is
// the difference between a pen that scales with the drawing and one that does
// not: stroking here and transforming the region turns a round pen into the
// ellipse a non-uniform scale should make of it, where transforming first and
// stroking after would keep it stubbornly round.
GPUWidgets::Path buildGeometry(const SVGElement& element, float flatness)
{
    auto path = GPUWidgets::Path {};
    path.setFlatness(flatness);

    auto& tag = element.tag;

    if (tag == "rect")
        addRectGeometry(path, element);
    else if (tag == "circle")
        addEllipseGeometry(path,
                           element.numAttr("cx"),
                           element.numAttr("cy"),
                           element.numAttr("r"),
                           element.numAttr("r"));
    else if (tag == "ellipse")
        addEllipseGeometry(path,
                           element.numAttr("cx"),
                           element.numAttr("cy"),
                           element.numAttr("rx"),
                           element.numAttr("ry"));
    else if (tag == "line")
    {
        path.moveTo({element.numAttr("x1"), element.numAttr("y1")});
        path.lineTo({element.numAttr("x2"), element.numAttr("y2")});
    }
    else if (tag == "polyline")
        addPolylineGeometry(path, element, false);
    else if (tag == "polygon")
        addPolylineGeometry(path, element, true);
    else if (tag == "path")
        parseSVGPathInto(element.attr("d"), path);

    return path;
}

bool isShapeTag(const std::string& tag)
{
    return tag == "rect" || tag == "circle" || tag == "ellipse" || tag == "line"
           || tag == "polyline" || tag == "polygon" || tag == "path";
}

bool isContainerTag(const std::string& tag)
{
    return tag == "g" || tag == "svg";
}
} // namespace

void SVGComponent::applyPresentationAttributes(Style& style,
                                               const SVGElement& element)
{
    applyColour(element.attr("fill"), style.fill, style.hasFill);
    applyColour(element.attr("stroke"), style.stroke, style.hasStroke);

    applyNumber(element.attr("stroke-width"), style.strokeStyle.width);
    applyNumber(element.attr("stroke-miterlimit"), style.strokeStyle.miterLimit);
    applyNumber(element.attr("fill-opacity"), style.fillOpacity);
    applyNumber(element.attr("stroke-opacity"), style.strokeOpacity);
    applyNumber(element.attr("font-size"), style.fontSize);

    auto opacity = element.attr("opacity");
    if (!opacity.empty())
        style.opacity *= Strings::parseFloatOr(opacity, 1.f);

    auto fillRule = element.attr("fill-rule");
    if (!fillRule.empty())
        style.fillRule = parseFillRule(fillRule);

    auto cap = element.attr("stroke-linecap");
    if (!cap.empty())
        style.strokeStyle.cap = parseLineCap(cap);

    auto join = element.attr("stroke-linejoin");
    if (!join.empty())
        style.strokeStyle.join = parseLineJoin(join);

    auto family = element.attr("font-family");
    if (!family.empty())
        style.fontFamily = firstFontFamily(family);

    auto transform = element.attr("transform");
    if (!transform.empty())
    {
        // The element's own transform maps its coordinates into its parent's,
        // so it applies before everything inherited.
        style.transform = parseTransformMatrix(transform).then(style.transform);
    }
}

SVGComponent::SVGComponent() = default;
SVGComponent::~SVGComponent() = default;

void SVGComponent::setDocument(const SVGElement& root)
{
    documentRoot = root;

    documentWidth = root.numAttr("width", 300.f);
    documentHeight = root.numAttr("height", 150.f);

    auto numbers = parseNumberList(root.attr("viewBox"));

    if (numbers.size() >= 4)
    {
        viewBox = {numbers[0], numbers[1], numbers[2], numbers[3]};

        // The origin is subtracted, not ignored. SVGBuilder reads only the
        // third and fourth numbers, so viewBox="10 20 100 100" renders shifted
        // by (10, 20) and nothing says so.
        if (root.attr("width").empty())
            documentWidth = viewBox.w;

        if (root.attr("height").empty())
            documentHeight = viewBox.h;
    }
    else
    {
        viewBox = {0.f, 0.f, documentWidth, documentHeight};
    }

    rebuild();
    repaint();
}

GPUWidgets::AffineTransform SVGComponent::documentToComponent() const
{
    if (viewBox.w <= 0.f || viewBox.h <= 0.f)
        return {};

    auto area = getLocalBounds();

    return GPUWidgets::AffineTransform::translation(-viewBox.x, -viewBox.y)
        .then(GPUWidgets::AffineTransform::scaling(area.w / viewBox.w,
                                                   area.h / viewBox.h));
}

void SVGComponent::clearContent()
{
    order.clear();
    shapes.clear();
    texts.clear();

    // Dropped with the rest, so a resize rebuilds every glyph atlas the document
    // needs. Which is the honest cost: a resize re-rasterizes every mask in the
    // document anyway, and one atlas per distinct text size is small beside
    // that. It would be worth keeping if a document were ever text-heavy enough
    // for the rebuild to show.
    fonts.clear();
}

// Every mask in the document, built against the size the component is now.
//
// A resize therefore costs the whole document again -- flattening on the CPU and
// one compute dispatch on the GPU. That is the shape of the tier rather than an
// oversight: coverage is rasterized in device pixels, so a mask built for one
// size is the wrong mask for another, and the alternative is a document that
// goes soft as it grows.
void SVGComponent::rebuild()
{
    clearContent();

    auto area = getLocalBounds();

    if (area.w <= 0.f || area.h <= 0.f || documentRoot.tag.empty())
        return;

    auto style = Style {};
    style.transform = documentToComponent();

    buildElement(documentRoot, style);
}

void SVGComponent::resized()
{
    rebuild();
}

void SVGComponent::buildElement(const SVGElement& element, const Style& inherited)
{
    auto style = inherited;
    applyPresentationAttributes(style, element);

    if (isShapeTag(element.tag))
    {
        buildShapes(element, style);
        return;
    }

    if (element.tag == "text")
    {
        buildTextRun(element, style);
        return;
    }

    if (!isContainerTag(element.tag))
        return;

    for (auto& child: element.children)
        buildElement(child, style);
}

void SVGComponent::buildShapes(const SVGElement& element, const Style& style)
{
    // A degenerate transform collapses the document to nothing, and dividing the
    // flatness by it would ask for an unbounded number of segments on the way.
    auto scale = style.transform.getScaleFactor();

    if (scale <= 0.f)
        return;

    if (style.hasFill)
    {
        auto path = buildGeometry(element, fillFlatness() / scale);

        if (!path.isEmpty())
            addShape(path.transformed(style.transform),
                     style.fill.withAlpha(style.fill.a * style.opacity
                                          * style.fillOpacity),
                     style.fillRule);
    }

    if (style.hasStroke && style.strokeStyle.width > 0.f)
    {
        // Built a second time rather than shared with the fill, at a tolerance
        // ten times tighter. The two masks are different regions anyway, so
        // there is nothing to share but the polyline, and the polyline a fill
        // can afford is not one a stroke can.
        auto path = buildGeometry(element, strokeFlatness() / scale);

        if (!path.isEmpty())
        {
            // PathShape::setStroke would do this, but it strokes what it is
            // given and this has to stroke in the document's units and transform
            // the region afterwards -- see buildGeometry. The result fills
            // non-zero whatever the element's fill-rule said, because it is a
            // union of overlapping contours and even-odd would read every
            // overlap as a hole.
            auto region = GPUWidgets::strokeToFill(path, style.strokeStyle);

            addShape(region.transformed(style.transform),
                     style.stroke.withAlpha(style.stroke.a * style.opacity
                                            * style.strokeOpacity),
                     GPUWidgets::FillRule::NonZero);
        }
    }
}

void SVGComponent::buildTextRun(const SVGElement& element, const Style& style)
{
    if (element.textContent.empty())
        return;

    auto scale = style.transform.getScaleFactor();
    auto pointSize = style.fontSize * scale;

    if (pointSize <= 0.f)
        return;

    auto run = TextRun {};
    run.text = element.textContent;

    // SVG's y on a text element is the baseline, which is exactly what
    // Graphics::drawText's pen wants. SVGBuilder guesses at y - fontSize
    // instead, and then centres against a width of fontSize x length x 0.6.
    //
    // Only the origin is transformed. A rotated transform rotates where the text
    // sits and not the text, because a glyph is an axis-aligned quad out of an
    // atlas; text on a path is the same missing feature seen from the other end.
    run.baseline =
        style.transform.apply({element.numAttr("x"), element.numAttr("y")});

    run.colour =
        style.hasFill
            ? style.fill.withAlpha(style.fill.a * style.opacity * style.fillOpacity)
            : Graphics::Color::black(0.f);

    auto anchor = element.attr("text-anchor");

    if (anchor == "middle")
        run.anchor = TextAnchor::Middle;
    else if (anchor == "end")
        run.anchor = TextAnchor::End;

    run.fontIndex = findOrAddFont(style.fontFamily, pointSize);

    texts.add(run);
    order.add({true, texts.size() - 1});
}

void SVGComponent::addShape(const GPUWidgets::Path& path,
                            const Graphics::Color& colour,
                            GPUWidgets::FillRule rule)
{
    auto& shape = shapes.createNew(*this);

    shape.colour = colour;
    shape.maskBounds = path.getBounds();
    shape.mask.setPath(path, rule);

    order.add({false, shapes.size() - 1});
}

int SVGComponent::findOrAddFont(const std::string& family, float pointSize)
{
    // Matched with a tolerance rather than exactly: two sizes a hundredth of a
    // point apart rasterize to the same glyphs and would otherwise cost two
    // atlases and two batch breaks to say so.
    for (auto i = 0; i < fonts.size(); ++i)
        if (fonts[i]->family == family
            && std::abs(fonts[i]->pointSize - pointSize) < 0.01f)
            return i;

    fonts.createNew(family, pointSize);

    return fonts.size() - 1;
}

float SVGComponent::getTotalMaskArea() const
{
    auto total = 0.f;

    for (auto& shape: shapes)
        total += shape->maskBounds.w * shape->maskBounds.h;

    return total;
}

int SVGComponent::getDroppedShapeCount() const
{
    auto count = 0;

    for (auto& shape: shapes)
        if (shape->mask.wasDropped())
            ++count;

    return count;
}

void SVGComponent::paint(UI::Graphics& g)
{
    // The document's own font, while a run of text is being drawn. Swapped only
    // when it changes, so a document whose text is all one size costs one break
    // in and one out however many strings it has.
    auto* activeFont = static_cast<DocumentFont*>(nullptr);

    for (auto& item: order)
    {
        if (!item.isText)
        {
            auto& shape = *shapes[item.index];

            g.setColour(shape.colour);
            g.fillPath(shape.mask);
            continue;
        }

        auto& run = texts[item.index];
        auto& font = *fonts[run.fontIndex];

        if (&font != activeFont)
        {
            g.setTextRenderer(font.renderer);
            activeFont = &font;
        }

        // Resolved here rather than when the run was built, because the offset
        // an anchor needs is the width of the glyphs that are about to be drawn
        // and only the renderer holding them can say what that is.
        auto x = run.baseline.x;

        if (run.anchor == TextAnchor::Middle)
            x -= g.measureText(run.text) * 0.5f;
        else if (run.anchor == TextAnchor::End)
            x -= g.measureText(run.text);

        g.setColour(run.colour);
        g.drawText(run.text, {x, run.baseline.y});
    }

    if (activeFont != nullptr)
        g.resetTextRenderer();
}
} // namespace eacp::SVG
