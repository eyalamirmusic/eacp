#include "SVGComponent.h"

#include "SVGAttributes.h"
#include "SVGGeometry.h"
#include "SVGGradient.h"
#include "SVGPathParser.h"

#include <algorithm>

namespace eacp::SVG
{
// Everything an element inherits from the tree above it.
struct SVGComponent::Style
{
    struct ClipReference
    {
        std::string reference;
        GPUWidgets::AffineTransform transform;
    };

    Graphics::Color fill = Graphics::Color::black();
    Graphics::Color stroke = Graphics::Color::black();
    bool hasFill = true;
    bool hasStroke = false;

    // The id a `fill="url(#id)"` names, empty for the usual case of a colour.
    std::string fillReference;
    std::string strokeReference;

    // Multiplied into the colours. A *container's* opacity is not this: it
    // composites through a layer and never reaches here.
    float opacity = 1.f;
    float fillOpacity = 1.f;
    float strokeOpacity = 1.f;

    GPUWidgets::FillRule fillRule = GPUWidgets::FillRule::NonZero;

    // Width in the document's units, converted when the region is built.
    GPUWidgets::StrokeStyle strokeStyle;

    // Cut into the centre line before it is stroked, so in document units too.
    GPUWidgets::DashPattern dash;

    std::string fontFamily = UI::defaultUIFontFamily();
    float fontSize = 16.f;
    bool bold = false;
    bool italic = false;

    // The document's units onto this component's points, composed down the
    // tree and baked into the geometry rather than applied at draw time.
    GPUWidgets::AffineTransform transform;

    // In force outermost first, each with the transform of the element that
    // wrote it: a child adding a transform does not move an inherited clip.
    // Resolved late, a bounding-box clip needing the geometry it is to cut.
    Vector<ClipReference> clips;
};

namespace
{
// Path's own default, read off rather than restated so the two cannot drift.
float fillFlatness()
{
    return GPUWidgets::Path {}.getFlatness();
}

// Ten times tighter, since offsetting a polyline amplifies whatever error the
// flattening left in it, and both edges of the stroke carry it.
float strokeFlatness()
{
    return fillFlatness() * 0.1f;
}

// The first name of a font-family list, unquoted: the tier below has no way to
// be handed a fallback chain.
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

// A colour, or the id of a gradient defined elsewhere -- remembered rather than
// resolved, a bounding-box gradient needing geometry that does not exist yet.
void applyPaint(const std::string& value,
                Graphics::Color& colour,
                std::string& reference,
                bool& present)
{
    if (value.empty())
        return;

    auto referenced = parsePaintReference(value);

    if (!referenced.empty())
    {
        reference = referenced;
        present = true;
        return;
    }

    reference.clear();

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

bool isContainerTag(const std::string& tag)
{
    return tag == "g" || tag == "svg";
}

// A <symbol> and a nested <svg> bring a viewBox and are instantiated into
// whatever box the use site asks for; everything else is drawn as authored.
bool isViewportTag(const std::string& tag)
{
    return tag == "symbol" || tag == "svg";
}

// A <use> may name an element that contains it, which the specification forbids
// and nothing in a file prevents.
constexpr int maxUseDepth = 8;

// Not a property that inherits: it applies to the element, and for a container
// that means to the group. Read here rather than folded into the style.
float elementOpacity(const SVGElement& element)
{
    auto value = PropertyReader {element}("opacity");

    if (value.empty())
        return 1.f;

    return std::clamp(Strings::parseFloatOr(value, 1.f), 0.f, 1.f);
}

void applyDashPattern(const std::string& value, GPUWidgets::DashPattern& dash)
{
    if (value.empty())
        return;

    dash.lengths = Strings::toLower(value) == "none" ? Vector<float> {}
                                                     : parseNumberList(value);
}

void collectIds(const SVGElement& element,
                std::unordered_map<std::string, const SVGElement*>& byId)
{
    auto id = element.attr("id");

    // First wins where a document repeats an id, as a browser does.
    if (!id.empty())
        byId.emplace(id, &element);

    for (auto& child: element.children)
        collectIds(child, byId);
}
} // namespace

void SVGComponent::applyPresentationAttributes(Style& style,
                                               const SVGElement& element)
{
    auto read = PropertyReader {element};

    applyPaint(read("fill"), style.fill, style.fillReference, style.hasFill);
    applyPaint(read("stroke"), style.stroke, style.strokeReference, style.hasStroke);

    applyNumber(read("stroke-width"), style.strokeStyle.width);
    applyNumber(read("stroke-miterlimit"), style.strokeStyle.miterLimit);
    applyNumber(read("stroke-dashoffset"), style.dash.offset);
    applyNumber(read("fill-opacity"), style.fillOpacity);
    applyNumber(read("stroke-opacity"), style.strokeOpacity);
    applyNumber(read("font-size"), style.fontSize);

    applyDashPattern(read("stroke-dasharray"), style.dash);

    auto fillRule = read("fill-rule");
    if (!fillRule.empty())
        style.fillRule = parseFillRule(fillRule);

    auto cap = read("stroke-linecap");
    if (!cap.empty())
        style.strokeStyle.cap = parseLineCap(cap);

    auto join = read("stroke-linejoin");
    if (!join.empty())
        style.strokeStyle.join = parseLineJoin(join);

    auto family = read("font-family");
    if (!family.empty())
        style.fontFamily = firstFontFamily(family);

    // Numeric weights count as bold from 600 up, where the format puts it.
    auto weight = read("font-weight");
    if (!weight.empty())
        style.bold = weight == "bold" || weight == "bolder"
                     || (std::isdigit(static_cast<unsigned char>(weight.front()))
                         && std::stof(weight) >= 600.f);

    auto slant = read("font-style");
    if (!slant.empty())
        style.italic = slant == "italic" || slant == "oblique";

    // Off the attribute and not through the declarations: SVG 1.1 has no
    // transform property, and the later CSS one writes its arguments in a
    // grammar parseTransformMatrix does not implement.
    auto transform = element.attr("transform");
    if (!transform.empty())
    {
        style.transform = parseTransformMatrix(transform).then(style.transform);
    }

    // After the transform, a clip-path naming a region in the space the element
    // establishes rather than the one it sits in.
    auto clip = parsePaintReference(read("clip-path"));

    if (!clip.empty())
        style.clips.add({clip, style.transform});
}

SVGComponent::SVGComponent() = default;
SVGComponent::~SVGComponent() = default;

void SVGComponent::setDocument(const SVGElement& root)
{
    documentRoot = root;

    // Into the copy, not the argument, which does not have to outlive the call.
    elementsById.clear();
    collectIds(documentRoot, elementsById);

    documentWidth = root.numAttr("width", 300.f);
    documentHeight = root.numAttr("height", 150.f);

    aspectRatio = parsePreserveAspectRatio(root.attr("preserveAspectRatio"));

    auto numbers = parseNumberList(root.attr("viewBox"));

    if (numbers.size() >= 4)
    {
        viewBox = {numbers[0], numbers[1], numbers[2], numbers[3]};

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
    return viewBoxTransform(viewBox, getLocalBounds(), aspectRatio);
}

void SVGComponent::clearContent()
{
    order.clear();
    output = &order;
    shapes.clear();
    groups.clear();
    clips.clear();
    texts.clear();
}

// Every mask, built against the size the component is now: coverage is
// rasterized in device pixels, so a mask built for one size is wrong for
// another. A resize therefore costs the whole document again.
void SVGComponent::rebuild()
{
    clearContent();

    auto area = getLocalBounds();

    if (area.w > 0.f && area.h > 0.f && !documentRoot.tag.empty())
    {
        auto style = Style {};
        style.transform = documentToComponent();

        buildElement(documentRoot, style, 0);
    }
}

void SVGComponent::resized()
{
    rebuild();
}

void SVGComponent::buildElement(const SVGElement& element,
                                const Style& inherited,
                                int depth)
{
    auto style = inherited;
    applyPresentationAttributes(style, element);

    auto own = elementOpacity(element);

    // A shape or a string has nothing to composite with: its opacity is its
    // colour's, needing no texture. Only a container is a *group*.
    auto leaf = isShapeTag(element.tag) || element.tag == "text";

    if (leaf || own >= 1.f)
    {
        style.opacity *= own;
        buildElementContent(element, style, depth);
        return;
    }

    buildOpacityGroup(element, style, depth, own);
}

// The children are built with the *inherited* opacity and not the group's,
// which is applied once where the layer is composited.
void SVGComponent::buildOpacityGroup(const SVGElement& element,
                                     const Style& style,
                                     int depth,
                                     float opacity)
{
    auto content = Vector<Drawable> {};
    auto* enclosing = output;

    output = &content;
    buildElementContent(element, style, depth);
    output = enclosing;

    if (content.empty())
        return;

    // After the content, since a layer may hold another and UI::Layer renders
    // them in the order they registered.
    auto& group = groups.createNew(*this);

    group.content = std::move(content);
    group.layer.setOpacity(opacity);
    group.layer.setBounds(boundsOf(group.content));

    auto index = groups.size() - 1;

    group.layer.onPaint = [this, index](UI::Graphics& g)
    { paintDrawables(g, groups[index]->content); };

    output->add({Drawable::Kind::Group, index});
}

void SVGComponent::buildElementContent(const SVGElement& element,
                                       const Style& style,
                                       int depth)
{
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

    if (element.tag == "use")
    {
        buildUse(element, style, depth);
        return;
    }

    // Which is what makes <defs> and <symbol> draw nothing where they stand,
    // being descended into only through the <use> that names them.
    if (!isContainerTag(element.tag))
        return;

    for (auto& child: element.children)
        buildElement(child, style, depth);
}

Graphics::Rect SVGComponent::boundsOf(const Vector<Drawable>& drawables) const
{
    auto area = getLocalBounds();
    auto result = Graphics::Rect {};
    auto found = false;

    auto include = [&](const Graphics::Rect& rect)
    {
        if (!found)
        {
            result = rect;
            found = true;
            return;
        }

        auto left = std::min(result.x, rect.x);
        auto top = std::min(result.y, rect.y);

        result = {left,
                  top,
                  std::max(result.right(), rect.right()) - left,
                  std::max(result.bottom(), rect.bottom()) - top};
    };

    for (const auto& item: drawables)
    {
        if (item.kind == Drawable::Kind::Shape)
        {
            // Grown by a point: a mask is the geometry snapped out to whole
            // device pixels, and a mesh carries a feather half a pixel wide.
            const auto& shape = *shapes[item.index];

            include(shape.maskBounds.inset(-1.f));
            continue;
        }

        if (item.kind == Drawable::Kind::Group)
        {
            include(groups[item.index]->layer.getBounds());
            continue;
        }

        // A run of text, whose extent nobody has measured yet, so the group
        // takes the whole component rather than a box that might cut it.
        return area;
    }

    return found ? result.intersection(area) : Graphics::Rect {};
}

const SVGElement* SVGComponent::findElementById(const std::string& id) const
{
    if (id.empty())
        return nullptr;

    auto found = elementsById.find(id);

    return found != elementsById.end() ? found->second : nullptr;
}

void SVGComponent::buildUse(const SVGElement& element, const Style& style, int depth)
{
    if (depth >= maxUseDepth)
        return;

    auto* target = findElementById(hrefId(element));

    if (target == nullptr)
        return;

    auto useStyle = style;

    // x and y translate *within* the group the use establishes, so they are the
    // first thing the referenced geometry meets.
    useStyle.transform = GPUWidgets::AffineTransform::translation(
                             element.numAttr("x"), element.numAttr("y"))
                             .then(useStyle.transform);

    if (isViewportTag(target->tag))
    {
        buildSymbol(*target, element, useStyle, depth + 1);
        return;
    }

    // Built again rather than shared: a PathShape holds the mask a kernel
    // rasterized at one size and place, so two use sites are two masks.
    buildElement(*target, useStyle, depth + 1);
}

void SVGComponent::buildSymbol(const SVGElement& symbol,
                               const SVGElement& useSite,
                               const Style& inherited,
                               int depth)
{
    auto style = inherited;
    applyPresentationAttributes(style, symbol);

    auto numbers = parseNumberList(symbol.attr("viewBox"));

    if (numbers.size() >= 4)
    {
        auto box = Graphics::Rect {numbers[0], numbers[1], numbers[2], numbers[3]};

        // Said nothing, the use site draws the symbol at its authored size.
        auto viewport = Graphics::Rect {0.f,
                                        0.f,
                                        useSite.numAttr("width", box.w),
                                        useSite.numAttr("height", box.h)};

        auto fit = parsePreserveAspectRatio(symbol.attr("preserveAspectRatio"));

        style.transform = viewBoxTransform(box, viewport, fit).then(style.transform);
    }

    for (auto& child: symbol.children)
        buildElement(child, style, depth);
}

void SVGComponent::buildShapes(const SVGElement& element, const Style& style)
{
    // A degenerate transform collapses the document to nothing, and dividing
    // the flatness by it would ask for unboundedly many segments on the way.
    auto scale = style.transform.getScaleFactor();

    if (scale <= 0.f)
        return;

    auto fillPath = style.hasFill ? buildGeometry(element, fillFlatness() / scale)
                                  : GPUWidgets::Path {};

    // Built a second time rather than shared with the fill: the two masks are
    // different regions, and the polyline a fill can afford a stroke cannot.
    auto strokePath = style.hasStroke && style.strokeStyle.width > 0.f
                          ? buildGeometry(element, strokeFlatness() / scale)
                          : GPUWidgets::Path {};

    // The geometry's box and not the stroked region's, the format meaning the
    // box the shape was authored as. Resolved once for both, so a filled and
    // stroked element is two masks under one clip.
    auto objectBounds =
        !fillPath.isEmpty() ? fillPath.getBounds() : strokePath.getBounds();

    auto clip = resolveClips(style, objectBounds);

    // An empty clip region means the element is not drawn at all, which differs
    // from having no clip.
    if (clip.hasRect && clip.rect.isEmpty())
        return;

    if (!fillPath.isEmpty())
        addShape(
            fillPath.transformed(style.transform),
            style.fill.withAlpha(style.fill.a * style.opacity * style.fillOpacity),
            style.fillRule,
            clip,
            gradientFor(style.fillReference, objectBounds, style.transform));

    if (!strokePath.isEmpty())
    {
        // Stroked in the document's units and transformed afterwards, unlike
        // PathShape::setStroke. The result fills non-zero whatever the fill-rule
        // said, being a union of contours even-odd would read as holes.
        auto region = GPUWidgets::strokeToFill(
            GPUWidgets::dashPath(strokePath, style.dash), style.strokeStyle);

        // Against the *centre line's* bounds: that is the box the format means,
        // and a stroke growing it would shift the shading with the pen width.
        addShape(region.transformed(style.transform),
                 style.stroke.withAlpha(style.stroke.a * style.opacity
                                        * style.strokeOpacity),
                 GPUWidgets::FillRule::NonZero,
                 clip,
                 gradientFor(style.strokeReference,
                             strokePath.getBounds(),
                             style.transform));
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

    // SVG's y on a text element is the baseline. Only the origin is
    // transformed: a rotated transform moves where the text sits and not the
    // text, a glyph being an axis-aligned quad out of an atlas.
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

    run.font = {
        style.fontFamily, pointSize, Text::toFontStyle(style.bold, style.italic)};

    // Only the rectangular part of a clip applies to glyphs, but its bounds
    // still trim the string. The box is empty because a run's own extent is
    // unmeasured here, so a bounding-box clip is not applied to text at all.
    run.clip = resolveClips(style, {});

    if (run.clip.hasRect && run.clip.rect.isEmpty())
        return;

    texts.add(run);
    output->add({Drawable::Kind::Text, texts.size() - 1});
}

void SVGComponent::addShape(const GPUWidgets::Path& path,
                            const Graphics::Color& colour,
                            GPUWidgets::FillRule rule,
                            const ClipState& clip,
                            const UI::Gradient& gradient)
{
    auto& shape = shapes.createNew(*this);

    shape.colour = colour;
    shape.gradient = gradient;
    shape.clip = clip;
    shape.maskBounds = path.getBounds();
    shape.mask.setPath(path, rule);

    output->add({Drawable::Kind::Shape, shapes.size() - 1});
}

int SVGComponent::findOrAddClip(const std::string& reference,
                                const GPUWidgets::AffineTransform& transform,
                                const Graphics::Rect& objectBounds)
{
    // In user space the box is not read, and keying on it there would give a
    // group's clip one region per child.
    auto placedAgainstBox = clipUsesBoundingBox(reference, elementsById);

    // A box of nothing to place it against: left unclipped rather than clipped
    // to a point.
    if (placedAgainstBox && (objectBounds.w <= 0.f || objectBounds.h <= 0.f))
        return -1;

    auto bounds = placedAgainstBox ? objectBounds : Graphics::Rect {};

    auto matches = [&](const Clip& clip)
    {
        return clip.reference == reference && clip.transform == transform
               && UI::sameRect(clip.objectBounds, bounds);
    };

    for (auto i = 0; i < clips.size(); ++i)
        if (matches(*clips[i]))
            return i;

    auto scale = transform.getScaleFactor();

    if (scale <= 0.f)
        return -1;

    auto region =
        resolveClipPath(reference, elementsById, bounds, fillFlatness() / scale);

    // Not a clipPath, or a reference to nothing: the element draws unclipped. A
    // clipPath holding no geometry is the other case, and falls through to an
    // entry with empty bounds, so everything under it is clipped away.
    if (!region.resolved)
        return -1;

    auto path = region.path.transformed(transform);

    auto& clip = clips.createNew(*this);

    clip.reference = reference;
    clip.transform = transform;
    clip.objectBounds = bounds;

    // A rectangle is a scissor rect: exact, free of the atlas, and the only
    // kind of clip that reaches the glyphs. Every viewport clip is one.
    if (auto rectangle = asAxisAlignedRect(path))
    {
        clip.isRectangle = true;
        clip.bounds = *rectangle;

        return clips.size() - 1;
    }

    clip.bounds = path.getBounds();

    // Never the mesh route: a mesh carries its coverage in its own vertices and
    // leaves nothing for another shape to sample.
    clip.mask.setBacking(UI::PathShape::Backing::Mask);
    clip.mask.setPath(path, region.rule);

    return clips.size() - 1;
}

SVGComponent::ClipState
    SVGComponent::resolveClips(const Style& style,
                               const Graphics::Rect& objectBounds)
{
    auto result = ClipState {};

    for (const auto& reference: style.clips)
    {
        auto index =
            findOrAddClip(reference.reference, reference.transform, objectBounds);

        if (index < 0)
            continue;

        const auto& clip = *clips[index];

        // Every clip narrows the rectangle, whether or not its own shape is the
        // one that survives as a mask.
        result.rect =
            result.hasRect ? result.rect.intersection(clip.bounds) : clip.bounds;
        result.hasRect = true;

        // And the innermost shaped one takes the mask: a fragment reads a
        // single region.
        if (!clip.isRectangle)
            result.maskIndex = index;
    }

    return result;
}

UI::Gradient
    SVGComponent::gradientFor(const std::string& reference,
                              const Graphics::Rect& objectBounds,
                              const GPUWidgets::AffineTransform& transform) const
{
    if (reference.empty())
        return {};

    return resolveGradient(
        reference, elementsById, objectBounds, viewBox, transform);
}

int SVGComponent::getFontCount() const
{
    auto distinct = Vector<UI::Font> {};

    for (auto& run: texts)
    {
        auto seen = false;

        for (auto& font: distinct)
            seen =
                seen
                || (font.style == run.font.style && Text::sameFace(font, run.font));

        if (!seen)
            distinct.add(run.font);
    }

    return distinct.size();
}

float SVGComponent::getTotalMaskArea() const
{
    auto total = 0.f;

    for (auto& shape: shapes)
        total += shape->maskBounds.w * shape->maskBounds.h;

    return total;
}

float SVGComponent::getAtlasMaskArea() const
{
    auto total = 0.f;

    for (auto& shape: shapes)
        if (!shape->mask.isMeshed())
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

int SVGComponent::getClipMaskCount() const
{
    auto count = 0;

    for (auto& clip: clips)
        if (!clip->isRectangle)
            ++count;

    return count;
}

int SVGComponent::getMeshedShapeCount() const
{
    auto count = 0;

    for (auto& shape: shapes)
        if (shape->mask.isMeshed())
            ++count;

    return count;
}

void SVGComponent::paint(UI::Graphics& g)
{
    paintDrawables(g, order);
}

void SVGComponent::paintDrawables(UI::Graphics& g, const Vector<Drawable>& drawables)
{
    auto drawShape = [&](const Shape& shape)
    {
        g.setColour(shape.colour);

        // Set per shape, each one's gradient being placed against its own
        // geometry.
        if (shape.gradient.isEmpty())
            g.clearGradient();
        else
            g.setGradient(shape.gradient);

        g.fillPath(shape.mask);
    };

    auto drawText = [&](const TextRun& run)
    {
        // Set per run: every face the document uses is in the same atlas, so
        // this is an assignment and not a batch break.
        g.setFont(run.font);

        // Resolved here rather than at build time, the offset an anchor needs
        // being the width of the glyphs about to be drawn.
        auto x = run.baseline.x;

        if (run.anchor == TextAnchor::Middle)
            x -= g.measureText(run.text) * 0.5f;
        else if (run.anchor == TextAnchor::End)
            x -= g.measureText(run.text);

        g.setColour(run.colour);
        g.drawText(run.text, {x, run.baseline.y});
    };

    auto applyClip = [&](const ClipState& clip)
    {
        if (clip.hasRect)
            g.reduceClipRegion(clip.rect);

        if (clip.maskIndex >= 0)
            g.reduceClipToShape(clips[clip.maskIndex]->mask);
    };

    auto draw = [&](const Drawable& item)
    {
        if (item.kind == Drawable::Kind::Text)
        {
            drawText(texts[item.index]);
            return;
        }

        if (item.kind == Drawable::Kind::Group)
        {
            g.drawLayer(groups[item.index]->layer);
            return;
        }

        drawShape(*shapes[item.index]);
    };

    for (auto& item: drawables)
    {
        // A group carries no clip of its own: an inherited clip is resolved
        // where the geometry is, so its drawables have it already.
        const auto* clip =
            item.kind == Drawable::Kind::Text    ? &texts[item.index].clip
            : item.kind == Drawable::Kind::Shape ? &shapes[item.index]->clip
                                                 : nullptr;

        // A clip costs a batch break each way, so a run of shapes under one
        // stays a single draw and a document without them pays nothing.
        if (clip == nullptr || clip->isEmpty())
        {
            draw(item);
            continue;
        }

        auto scope = UI::Graphics::ScopedState {g};
        applyClip(*clip);

        draw(item);
    }
}
} // namespace eacp::SVG
