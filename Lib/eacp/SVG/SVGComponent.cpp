#include "SVGComponent.h"

#include "SVGAttributes.h"
#include "SVGGeometry.h"
#include "SVGGradient.h"
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
    // A clip-path an element asked for, and the space it was asked for in.
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
    // Inherited like the colour it stands in for, so a group can paint its
    // children with one gradient.
    std::string fillReference;
    std::string strokeReference;

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

    // Cut into the centre line before it is stroked, and therefore in the
    // document's units too.
    GPUWidgets::DashPattern dash;

    std::string fontFamily = UI::defaultUIFontFamily();
    float fontSize = 16.f;

    // The document's units onto this component's points, composed down the
    // tree. Baked into the geometry rather than applied to anything at draw
    // time, which is what lets the whole document be one component.
    GPUWidgets::AffineTransform transform;

    // The clip-paths in force, outermost first, each with the transform that was
    // in force where it was written -- a clip belongs to the space of the
    // element carrying it, and a child adding a transform of its own does not
    // move the clip it inherited.
    //
    // Carried rather than resolved on the spot because a clip in bounding-box
    // units cannot be resolved without the geometry it is about to cut, and that
    // does not exist while the style is being read. Resolving late is also what
    // shares one region between every child of a clipped group.
    //
    // clip-path is not an inherited property, and this is not it being inherited
    // -- an element's clip applies to the element, which for a container is
    // everything drawn inside it. Reading the attribute per element and adding
    // to the copy is exactly that scope.
    Vector<ClipReference> clips;
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

// A paint: a colour, or the id of a gradient the document defined elsewhere.
//
// The reference is remembered rather than resolved, because resolving it needs
// the shape -- a gradient in bounding-box units is placed against the geometry
// it fills, which does not exist yet while the style is being read.
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

// The referenced element's own coordinates, before a <use> places it. A
// <symbol> and a nested <svg> bring a viewBox and are therefore instantiated
// into whatever box the use site asks for; everything else is drawn where it
// was authored.
bool isViewportTag(const std::string& tag)
{
    return tag == "symbol" || tag == "svg";
}

// A <use> may name an element that contains the <use>, which the specification
// forbids and nothing in a file prevents. Eight levels is past anything an
// honest document nests and short enough that a cycle costs nothing.
constexpr int maxUseDepth = 8;

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

    // First wins where a document repeats an id, which is what a browser does
    // with the same mistake.
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

    auto opacity = read("opacity");
    if (!opacity.empty())
        style.opacity *= Strings::parseFloatOr(opacity, 1.f);

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

    // Read off the attribute and not through the declarations, unlike everything
    // above it. SVG 1.1 has no transform *property*, and the CSS one that came
    // later writes its arguments differently enough - lengths with units,
    // angles with them too - that reading it here would be reading a grammar
    // parseTransformMatrix does not implement.
    auto transform = element.attr("transform");
    if (!transform.empty())
    {
        // The element's own transform maps its coordinates into its parent's,
        // so it applies before everything inherited.
        style.transform = parseTransformMatrix(transform).then(style.transform);
    }

    // After the transform, because the region a clip-path names is in the space
    // the element establishes rather than the one it sits in: a group that
    // translates carries its clip with it.
    auto clip = parsePaintReference(read("clip-path"));

    if (!clip.empty())
        style.clips.add({clip, style.transform});
}

SVGComponent::SVGComponent() = default;
SVGComponent::~SVGComponent() = default;

void SVGComponent::setDocument(const SVGElement& root)
{
    documentRoot = root;

    // Into the copy, not the argument: these outlive the call and the caller's
    // element does not have to.
    elementsById.clear();
    collectIds(documentRoot, elementsById);

    documentWidth = root.numAttr("width", 300.f);
    documentHeight = root.numAttr("height", 150.f);

    aspectRatio = parsePreserveAspectRatio(root.attr("preserveAspectRatio"));

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
    return viewBoxTransform(viewBox, getLocalBounds(), aspectRatio);
}

void SVGComponent::clearContent()
{
    order.clear();
    shapes.clear();
    clips.clear();
    texts.clear();

    // Set aside for the next build rather than destroyed. A renderer bakes its
    // family and size into a glyph atlas, so dropping one and asking for the
    // same pair again is a raster of every glyph the document uses -- and a
    // rebuild is not rare, since anything that changes the document's size runs
    // one.
    //
    // Kept on the side rather than simply kept, because a *resize* genuinely
    // asks for new sizes: the point size is the document's font size times the
    // transform's scale, so a drag that crosses two hundred widths would leave
    // two hundred renderers behind if the cache only ever grew. findOrAddFont
    // claims what it recognizes out of here and rebuild drops the rest, which
    // makes the cache exactly what the last build used.
    for (auto& font: fonts)
        spareFonts.add(std::move(font));

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

    if (area.w > 0.f && area.h > 0.f && !documentRoot.tag.empty())
    {
        auto style = Style {};
        style.transform = documentToComponent();

        buildElement(documentRoot, style, 0);
    }

    // Every renderer the build did not claim back out of the cache: the sizes
    // the document used to want and does not any more.
    spareFonts.clear();
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

    // Which is also what makes <defs> and <symbol> draw nothing where they
    // stand: neither is a container here, so neither is descended into except
    // through the <use> that names it.
    if (!isContainerTag(element.tag))
        return;

    for (auto& child: element.children)
        buildElement(child, style, depth);
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

    // A use is the referenced element inside a group carrying the use's own
    // attributes, with x and y translating *within* that group -- so the
    // translation is the first thing the referenced geometry meets and the use's
    // transform, already folded into style, is applied to the result.
    useStyle.transform = GPUWidgets::AffineTransform::translation(
                             element.numAttr("x"), element.numAttr("y"))
                             .then(useStyle.transform);

    if (isViewportTag(target->tag))
    {
        buildSymbol(*target, element, useStyle, depth + 1);
        return;
    }

    // Built again rather than shared. A PathShape holds the mask a kernel
    // rasterized at one size and one place, so two use sites of one symbol are
    // two masks however identical the markup was -- which is the tier's cost for
    // an instanced document, and the reason a use of a big shape is as expensive
    // as writing it out.
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

        // The use site says how big the symbol is drawn. Said nothing, it is
        // drawn at the size it was authored, which is the box itself.
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
    // A degenerate transform collapses the document to nothing, and dividing the
    // flatness by it would ask for an unbounded number of segments on the way.
    auto scale = style.transform.getScaleFactor();

    if (scale <= 0.f)
        return;

    auto fillPath = style.hasFill ? buildGeometry(element, fillFlatness() / scale)
                                  : GPUWidgets::Path {};

    // Built a second time rather than shared with the fill, at a tolerance ten
    // times tighter. The two masks are different regions anyway, so there is
    // nothing to share but the polyline, and the polyline a fill can afford is
    // not one a stroke can.
    auto strokePath = style.hasStroke && style.strokeStyle.width > 0.f
                          ? buildGeometry(element, strokeFlatness() / scale)
                          : GPUWidgets::Path {};

    // The element's own bounding box, which a clip in bounding-box units is
    // placed against -- the geometry's and not the stroked region's, the format
    // meaning the box the shape was authored as. Resolved once for both, so an
    // element that is filled and stroked is two masks under one clip rather than
    // two clips.
    auto objectBounds =
        !fillPath.isEmpty() ? fillPath.getBounds() : strokePath.getBounds();

    auto clip = resolveClips(style, objectBounds);

    // A clipPath that resolved to a region of nothing: the element is not drawn
    // at all, which is what an empty clip means and is different from having no
    // clip.
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
        // PathShape::setStroke would do this, but it strokes what it is given
        // and this has to stroke in the document's units and transform the
        // region afterwards -- see buildGeometry. The result fills non-zero
        // whatever the element's fill-rule said, because it is a union of
        // overlapping contours and even-odd would read every overlap as a hole.
        //
        // Dashed before stroking, since a dash cuts the centre line and the
        // stroke has replaced it. Free when nothing asked for one: dashPath
        // hands an empty pattern its path straight back.
        auto region = GPUWidgets::strokeToFill(
            GPUWidgets::dashPath(strokePath, style.dash), style.strokeStyle);

        // Against the *centre line's* bounds and not the stroked region's,
        // because that is the bounding box the format means: a gradient in
        // bounding-box units is placed by the geometry, and a stroke growing it
        // by half a pen width would shift the shading with the width.
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

    // Only the rectangular part of it will ever be applied -- a glyph is drawn
    // by a renderer that samples no mask -- but the bounds of a shaped clip
    // still cut the string, which is the difference between a clipped caption
    // being trimmed and it running out of the region entirely.
    //
    // The box is empty because a run's own is not known here: it is the extent
    // of glyphs the renderer has not measured yet. A clip in bounding-box units
    // is therefore not applied to text at all, rather than applied against a box
    // of nothing, which would erase it.
    run.clip = resolveClips(style, {});

    if (run.clip.hasRect && run.clip.rect.isEmpty())
        return;

    texts.add(run);
    order.add({true, texts.size() - 1});
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

    order.add({false, shapes.size() - 1});
}

int SVGComponent::findOrAddClip(const std::string& reference,
                                const GPUWidgets::AffineTransform& transform,
                                const Graphics::Rect& objectBounds)
{
    // Only where the region is actually placed against it. In user space the
    // box is not read at all, and keying on it there would give a group's clip
    // one region per child -- which is the whole thing this sharing exists to
    // avoid.
    auto placedAgainstBox = clipUsesBoundingBox(reference, elementsById);

    // A box of nothing to place it against: text, whose extent is the
    // renderer's business, or geometry that came to nothing. Left unclipped
    // rather than clipped to a point.
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

    // Not a clipPath, or a reference to nothing at all: ignored, and the element
    // draws unclipped. A clipPath that *is* one and holds no geometry is the
    // other case entirely, and falls through to an entry whose bounds are empty
    // -- so everything under it is clipped away.
    if (!region.resolved)
        return -1;

    auto path = region.path.transformed(transform);

    auto& clip = clips.createNew(*this);

    clip.reference = reference;
    clip.transform = transform;
    clip.objectBounds = bounds;

    // A rectangle is a scissor rect: exact, free of the atlas, and the only kind
    // of clip that reaches the glyphs. Worth testing for rather than assuming
    // away, because the commonest clip in any document is a viewport and every
    // viewport is one.
    if (auto rectangle = asAxisAlignedRect(path))
    {
        clip.isRectangle = true;
        clip.bounds = *rectangle;

        return clips.size() - 1;
    }

    clip.bounds = path.getBounds();

    // Never the mesh route. A mesh carries its coverage in its own vertices and
    // leaves nothing for another shape to sample, and a clip is only a clip
    // because something else can read it.
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
        // one that survives as a mask. That is what makes nesting exact wherever
        // the outer clips are rectangles, and bounded wherever they are not.
        result.rect =
            result.hasRect ? result.rect.intersection(clip.bounds) : clip.bounds;
        result.hasRect = true;

        // And the innermost shaped one takes the mask, there being one to take:
        // a fragment reads a single region, so an outer shape has already given
        // what it can give above.
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

int SVGComponent::findOrAddFont(const std::string& family, float pointSize)
{
    // Matched with a tolerance rather than exactly: two sizes a hundredth of a
    // point apart rasterize to the same glyphs and would otherwise cost two
    // atlases and two batch breaks to say so.
    auto matches = [&](const DocumentFont& font)
    {
        return font.family == family && std::abs(font.pointSize - pointSize) < 0.01f;
    };

    for (auto i = 0; i < fonts.size(); ++i)
        if (matches(*fonts[i]))
            return i;

    // The previous build's, if it wanted this one too -- which a rebuild at an
    // unchanged size always does. Moved across rather than copied, a renderer
    // owning a glyph atlas being the thing worth not making twice.
    for (auto i = 0; i < spareFonts.size(); ++i)
    {
        if (!matches(*spareFonts[i]))
            continue;

        fonts.add(std::move(spareFonts[i]));
        spareFonts.removeAt(i);

        return fonts.size() - 1;
    }

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
    // The document's own font, while a run of text is being drawn. Swapped only
    // when it changes, so a document whose text is all one size costs one break
    // in and one out however many strings it has.
    auto* activeFont = static_cast<DocumentFont*>(nullptr);

    auto drawShape = [&](const Shape& shape)
    {
        g.setColour(shape.colour);

        // Set per shape rather than kept across them, because each one's
        // gradient is placed against its own geometry. A document with no
        // gradients never touches either call after the first.
        if (shape.gradient.isEmpty())
            g.clearGradient();
        else
            g.setGradient(shape.gradient);

        g.fillPath(shape.mask);
    };

    auto drawText = [&](const TextRun& run)
    {
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
    };

    auto applyClip = [&](const ClipState& clip)
    {
        if (clip.hasRect)
            g.reduceClipRegion(clip.rect);

        if (clip.maskIndex >= 0)
            g.reduceClipToShape(clips[clip.maskIndex]->mask);
    };

    for (auto& item: order)
    {
        const auto& clip =
            item.isText ? texts[item.index].clip : shapes[item.index]->clip;

        // Only where there is one. A clip costs a batch break each way, and a
        // document without them should pay nothing at all -- which also means
        // the run of shapes under one clip stays one draw, since the state does
        // not go up and down between them.
        if (clip.isEmpty())
        {
            if (item.isText)
                drawText(texts[item.index]);
            else
                drawShape(*shapes[item.index]);

            continue;
        }

        auto scope = UI::Graphics::ScopedState {g};
        applyClip(clip);

        if (item.isText)
            drawText(texts[item.index]);
        else
            drawShape(*shapes[item.index]);
    }

    if (activeFont != nullptr)
        g.resetTextRenderer();
}
} // namespace eacp::SVG
