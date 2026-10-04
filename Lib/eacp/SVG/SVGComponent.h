#pragma once

#include "SVGAttributes.h"
#include "SVGElement.h"

#include <eacp/GPUWidgets/Path/PathRasterizer.h>
#include <eacp/UI/Component/Component.h>
#include <eacp/UI/Render/Gradient.h>

namespace eacp::SVG
{
// An SVG document drawn through the component tier.
//
// The module's one renderer, and the same on every platform. It builds a
// UI::PathShape per shape rather than a window-server object per element: the
// masks are rasterized by one compute dispatch before the frame opens, and the
// document then draws as quads out of the shared coverage atlas, joining the
// same instanced draw as the interface around it.
//
// Which makes a static document nearly free to display. Rasterization is
// triggered by setting the geometry, so a document that is not being resized
// pays its CPU once and is quads for ever after; a document being resized or
// animated re-rasterizes, and that is the workload the GPU binner made
// affordable.
//
// It is one component and not a tree of them, because there is nothing for a
// tree to express: transforms are baked into the points rather than applied by
// moving a child, so nesting carries no state down to the draw. One component
// means one clip region, and therefore no batch break anywhere in a document
// however deeply the markup nested.
//
//     SVG::SVGComponent document;
//     document.setDocument(*SVG::parseXML(markup));
//     host.setRootComponent(document);
//
// Gradients it does: linear and radial, any number of stops, all three spread
// methods, both unit systems, gradientTransform, and one gradient inheriting
// another through href. What it leaves out of them is the focal point of a
// radial, which draws as a concentric one.
//
// Clip paths it does: a clipPath of any number of shapes, its own transforms,
// <use> inside it, both unit systems, and clip-rule. A rectangular one is a
// scissor rect and costs the atlas nothing; every other shape is a mask, and one
// mask reaches a shape at a time -- so where a document clips a clipped group,
// the inner region is exact and the outer one contributes its bounding box. Text
// is cut by the rectangle rather than by the shape, glyphs being drawn by a
// renderer that samples no mask.
//
// Opacity it does properly, which is two features spelled the same way: on a
// shape it is the colour's alpha, and on a container it is the group's -- drawn
// into a texture of its own and faded once, so that the overlaps inside it stay
// as solid as they were drawn. A document fades what it wrote rather than what
// happened to be cheap.
//
// What it does not do at all: <mask>, CSS selectors (the style *attribute* is
// read, a <style> element is not), filters and images. An element asking for one
// of those draws without it rather than not at all.
class SVGComponent : public UI::Component
{
public:
    SVGComponent();
    ~SVGComponent() override;

    // The parsed markup. Kept, because the geometry is rebuilt against the
    // component's size and so has to be buildable again on every resize.
    void setDocument(const SVGElement& root);

    // The document's own coordinate system: the viewBox if it has one, and
    // otherwise its width and height. What the geometry is authored in, before
    // the transform onto this component's bounds.
    Graphics::Rect getViewBox() const;

    // How that box is fitted to this component. The document's own
    // preserveAspectRatio, which defaults to uniform and centred rather than to
    // the stretch a naive fit would give.
    PreserveAspectRatio getAspectRatio() const;

    // The document's units onto this component's points: what a caller placing
    // something over the artwork, or hit-testing into it, needs.
    GPUWidgets::AffineTransform documentToComponent() const;

    // The intrinsic size, for a caller sizing a window to the artwork.
    float getDocumentWidth() const;
    float getDocumentHeight() const;

    void resized() override;
    void paint(UI::Graphics& g) override;

    // How many masks the document came to. Not the element count: an element
    // that is both filled and stroked is two, because a PathShape holds one
    // filled region and a stroke is a different region.
    int getShapeCount() const;

    // Containers the document asked to fade as a whole, each of which is a
    // texture of its own and a render pass to fill it. Zero for a document whose
    // opacity is all per-element, which is most of them.
    int getOpacityGroupCount() const;

    // Distinct clip regions the document came to. Not the number of elements
    // carrying a clip-path: a group's clip is one region however many children
    // it cuts, which is what stops a clipped group of twenty shapes costing
    // twenty identical masks.
    int getClipCount() const;

    // Of those, how many took a mask. The rest were rectangles, which are a
    // scissor rect and cost the atlas nothing at all -- and a viewport clip, the
    // commonest of all clips, is always one of those.
    int getClipMaskCount() const;

    // Of those, how many the coverage atlas had no room for -- each one missing
    // from the picture. See ComponentHost::getLastDroppedPathCount.
    int getDroppedShapeCount() const;

    // And how many never asked it for room, being large enough that drawing them
    // as triangles costs less than storing their coverage would. See
    // UI::PathShape::Backing: it is what keeps a document of large stacked
    // shapes inside an atlas that cannot hold their masks.
    int getMeshedShapeCount() const;

    // The area of every mask added up, in this component's points squared.
    // Times the square of the device scale, that is roughly the number of atlas
    // texels the document asks for - roughly, because a mask is the geometry's
    // bounds snapped out to whole device pixels, so the real figure is up to a
    // pixel per side larger.
    //
    // Worth reading, because it is the figure the atlas-ceiling argument in
    // plan.md bounds by the window's own area -- a mask being the size of its
    // shape on screen, nothing visible at once can exceed what is visible. That
    // holds while shapes tile and fails while they stack: a drawing is a
    // background, then shapes over it, then shapes over those, and each carries
    // its own full bounding box. This sum is how far past the window a
    // particular document goes.
    float getTotalMaskArea() const;

    // The same sum over the shapes that actually took a mask, which is what the
    // atlas is really asked for. The two figures differ by exactly the shapes
    // the mesh route took, and the gap between them is the whole of what that
    // route buys.
    float getAtlasMaskArea() const;

    // Distinct faces the document's text asked for -- a (family, size, style)
    // apiece. They share the tree's one glyph atlas, so what a face costs is its
    // own glyphs and nothing else: no texture, no batch break, and a document
    // mixing six of them is still one draw. The figure is kept because it used
    // to be the expensive number in a text-heavy document and is now the cheap
    // one, which is worth being able to see.
    int getFontCount() const;

private:
    struct ClipState;
    struct Shape;
    enum class TextAnchor;
    struct TextRun;
    struct Clip;
    struct Drawable;
    struct OpacityGroup;
    struct Style;

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

    // `depth` counts <use> indirections rather than tree depth, and exists
    // because a document may reference an element that contains the reference:
    // the specification forbids it and nothing stops a file doing it, so the
    // walk stops rather than recursing until the stack runs out.
    void buildElement(const SVGElement& element, const Style& inherited, int depth);
    void buildShapes(const SVGElement& element, const Style& style);
    void buildTextRun(const SVGElement& element, const Style& style);

    // A <use>, which is the referenced element built again here: with this
    // element's inherited style, its transform, and the extra translation its x
    // and y ask for. Not a shared mask -- each use site has its own transform
    // and therefore its own coverage, so there is nothing to share.
    void buildUse(const SVGElement& element, const Style& style, int depth);
    void
        buildNestedSvg(const SVGElement& element, const Style& inherited, int depth);

    // A <symbol> (or a nested <svg>) instantiated by a use: a container that
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

    // Every element in the document that named itself, so a <use> can find it.
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
};
} // namespace eacp::SVG
