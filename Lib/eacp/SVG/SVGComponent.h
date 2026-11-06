#pragma once

/** @file SVGComponent, the UI component that draws an SVG document. */

#include "SVGAttributes.h"
#include "SVGElement.h"

#include <eacp/GPUWidgets/Path/PathRasterizer.h>
#include <eacp/UI/Component/Component.h>
#include <eacp/UI/Render/Gradient.h>

namespace eacp::SVG
{
// An SVG document drawn as one UI component, the same on every platform.
// Shapes become UI::PathShape masks in the shared coverage atlas, rasterized
// when the geometry changes (setDocument, resize), so a static document costs
// only quads per frame.
//
//     SVG::SVGComponent document;
//     document.setDocument(*SVG::parseXML(markup));
//     host.setRootComponent(document);
//
// Supported: linear and radial gradients (a radial's focal point is ignored),
// clip paths (a clipped group inside a clipped group clips to the outer one's
// bounding box; text is clipped to the clip's rectangle), shape and group
// opacity (a group is faded as one layer).
// Not supported: `<mask>`, `<style>` elements and CSS selectors, filters;
// such elements draw without the effect.
class SVGComponent : public UI::Component
{
public:
    SVGComponent();
    ~SVGComponent() override;

    // The parsed markup; kept, and rebuilt against the size on every resize.
    void setDocument(const SVGElement& root);

    // The viewBox, or the document's width and height when it has none.
    Graphics::Rect getViewBox() const;

    // The document's preserveAspectRatio (default: uniform, centred).
    PreserveAspectRatio getAspectRatio() const;

    // The document's units onto this component's points: what a caller placing
    // something over the artwork, or hit-testing into it, needs.
    GPUWidgets::AffineTransform documentToComponent() const;

    // The intrinsic size, for a caller sizing a window to the artwork.
    float getDocumentWidth() const;
    float getDocumentHeight() const;

    void resized() override;
    void paint(UI::Graphics& g) override;

    // Masks the document produced; a filled and stroked element counts two.
    int getShapeCount() const;

    // Groups faded as a whole, each its own texture and render pass.
    int getOpacityGroupCount() const;

    // Distinct clip regions; a group's clip counts once however many children
    // it cuts.
    int getClipCount() const;

    // Clip regions that needed a mask; rectangles are scissor rects instead.
    int getClipMaskCount() const;

    // Shapes the coverage atlas had no room for, missing from the picture.
    int getDroppedShapeCount() const;

    // Shapes drawn as triangles instead of atlas masks because they are large
    // (see UI::PathShape::Backing).
    int getMeshedShapeCount() const;

    // The area of every shape's mask bounds summed, in points squared; times
    // the device scale squared, roughly the atlas texels requested. Exceeds
    // the window area when shapes overlap.
    float getTotalMaskArea() const;

    // The same sum over only the shapes that took a mask (not meshed).
    float getAtlasMaskArea() const;

    // Distinct (family, size, style) faces; they share the one glyph atlas.
    int getFontCount() const;

    int getImageCount() const;

private:
    struct ClipState;
    struct Shape;
    enum class TextAnchor;
    struct TextRun;
    struct Clip;
    struct Drawable;
    struct OpacityGroup;
    struct Style;
    struct EmbeddedImage;
    struct ImageRun;

    void rebuild();
    void clearContent();

    // One list of drawables, in document order. Called for the document itself
    // and again for each composited group, through its layer.
    void paintDrawables(UI::Graphics& g, const Vector<Drawable>& drawables);

    // The element built as it stands, with whatever the walk has already decided
    // about it. The half of buildElement that opacity does not change.
    void buildElementContent(const SVGElement& element,
                             const Style& style,
                             int depth);

    // The element built into a texture of its own, to be faded as one thing.
    void buildOpacityGroup(const SVGElement& element,
                           const Style& style,
                           int depth,
                           float opacity);

    // Where a composited group's content reaches, in this component's points --
    // which is how large a texture it needs. A run holding text takes the whole
    // component, a string's extent being the renderer's business rather than the
    // builder's, and everything is cut to the component in any case.
    Graphics::Rect boundsOf(const Vector<Drawable>& drawables) const;

    // Everything the element says about itself, over what it inherited.
    static void applyPresentationAttributes(Style& style, const SVGElement& element);

    // `depth` counts `<use>` indirections rather than tree depth, and exists
    // because a document may reference an element that contains the reference:
    // the specification forbids it and nothing stops a file doing it, so the
    // walk stops rather than recursing until the stack runs out.
    void buildElement(const SVGElement& element, const Style& inherited, int depth);
    void buildShapes(const SVGElement& element, const Style& style, int depth);
    void buildTextRun(const SVGElement& element, const Style& style);
    void buildImage(const SVGElement& element, const Style& style);

    void buildPatternFill(const SVGElement& element,
                          const GPUWidgets::Path& fillPath,
                          const Style& style,
                          int depth);

    EmbeddedImage* embeddedImageFor(const SVGElement& element);
    int addClipRegion(const GPUWidgets::Path& path, GPUWidgets::FillRule rule);

    // A `<use>`, which is the referenced element built again here: with this
    // element's inherited style, its transform, and the extra translation its x
    // and y ask for. Not a shared mask -- each use site has its own transform
    // and therefore its own coverage, so there is nothing to share.
    void buildUse(const SVGElement& element, const Style& style, int depth);
    void
        buildNestedSvg(const SVGElement& element, const Style& inherited, int depth);

    // A `<symbol>` (or a nested `<svg>`) instantiated by a use: a container that
    // brings its own viewBox, mapped onto the size the use site asked for.
    void buildSymbol(const SVGElement& symbol,
                     const SVGElement& useSite,
                     const Style& inherited,
                     int depth);

    const SVGElement* findElementById(const std::string& id) const;

    void addShape(const GPUWidgets::Path& path,
                  const Graphics::Color& colour,
                  GPUWidgets::FillRule rule,
                  const ClipState& clip,
                  const UI::Gradient& gradient = {});

    // The clips an element inherited, resolved against the geometry they are
    // about to cut. Empty when nothing in the tree above it asked for one, which
    // is the case that has to cost nothing.
    ClipState resolveClips(const Style& style, const Graphics::Rect& objectBounds);

    // The entry for one clip reference at one place, made if this is the first
    // drawable to ask for it. Negative when the reference resolves to nothing --
    // an id that names no clipPath, or one holding no geometry -- which the
    // format says draws the element unclipped.
    int findOrAddClip(const std::string& reference,
                      const GPUWidgets::AffineTransform& transform,
                      const Viewport& viewport,
                      const Graphics::Rect& objectBounds);

    // What a paint reference resolves to, against this document's ids and its
    // viewBox. See SVG::resolveGradient, which is where the two coordinate
    // systems are worked out.
    UI::Gradient gradientFor(const std::string& reference,
                             const Graphics::Rect& objectBounds,
                             const GPUWidgets::AffineTransform& transform) const;

    SVGElement documentRoot;
    Graphics::Rect viewBox;
    PreserveAspectRatio aspectRatio;
    float documentWidth = 0.f;
    float documentHeight = 0.f;

    // Every element in the document that named itself, so a `<use>` can find it.
    // Pointers into documentRoot, which is why that is stored whole and never
    // edited after it is set.
    std::unordered_map<std::string, const SVGElement*> elementsById;

    Vector<Drawable> order;

    // Where the walk is currently appending. The document's own order, except
    // inside a container that is being composited, where it is that group's.
    Vector<Drawable>* output = &order;

    OwnedVector<Shape> shapes;
    OwnedVector<OpacityGroup> groups;
    OwnedVector<Clip> clips;
    Vector<TextRun> texts;
    Vector<ImageRun> images;

    OwnedVector<EmbeddedImage> embeddedImages;
    std::unordered_map<const SVGElement*, int> embeddedImageIndex;
};
} // namespace eacp::SVG
