#pragma once

#include "SVGAttributes.h"
#include "SVGElement.h"

#include <eacp/UI/UI.h>

namespace eacp::SVG
{
// An SVG document drawn through the component tier.
//
// The sibling of SVGView, and the difference is the whole point of it. That one
// builds a native Graphics::ShapeLayer per shape -- a CAShapeLayer on macOS, a
// Direct2D geometry on Windows -- which is one window-server object per element
// of a drawing, the weight UI::Component exists to avoid. This one builds a
// UI::PathShape per shape instead: the masks are rasterized by one compute
// dispatch before the frame opens, and the document then draws as quads out of
// the shared coverage atlas, joining the same instanced draw as the interface
// around it.
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
// What it does not do at all: clip paths and masks, group opacity as
// compositing, CSS selectors (the style *attribute* is read, a <style> element
// is not), filters and images. An element asking for one of those draws without
// it rather than not at all.
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
    Graphics::Rect getViewBox() const { return viewBox; }

    // How that box is fitted to this component. The document's own
    // preserveAspectRatio, which defaults to uniform and centred rather than to
    // the stretch a naive fit would give.
    PreserveAspectRatio getAspectRatio() const { return aspectRatio; }

    // The document's units onto this component's points: what a caller placing
    // something over the artwork, or hit-testing into it, needs.
    GPUWidgets::AffineTransform documentToComponent() const;

    // The intrinsic size, for a caller sizing a window to the artwork.
    float getDocumentWidth() const { return documentWidth; }
    float getDocumentHeight() const { return documentHeight; }

    void resized() override;
    void paint(UI::Graphics& g) override;

    // How many masks the document came to. Not the element count: an element
    // that is both filled and stroked is two, because a PathShape holds one
    // filled region and a stroke is a different region.
    int getShapeCount() const { return shapes.size(); }

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

    // Distinct (family, size) pairs the document's text asked for. Each is its
    // own glyph atlas and therefore its own texture, so each is a batch break in
    // and a batch break out; the figure is here because nothing else says what a
    // text-heavy document costs.
    int getFontCount() const { return fonts.size(); }

private:
    // One filled region: the mask a kernel rasterized for it and the colour it
    // is multiplied by. A stroke is one of these too, its geometry being the
    // region the pen covers rather than the pen's path.
    struct Shape
    {
        explicit Shape(UI::Component& owner)
            : mask(owner)
        {
        }

        UI::PathShape mask;
        Graphics::Color colour;

        // The gradient the colour is replaced by, empty for the usual case.
        // Resolved when the shape was built rather than at paint time, because
        // placing one needs the geometry: a gradient in bounding-box units means
        // something different for every element it paints.
        UI::Gradient gradient;

        // The geometry's own bounds, kept because the mask's are only known once
        // a kernel has rasterized it and this has to be readable before that.
        Graphics::Rect maskBounds;
    };

    enum class TextAnchor
    {
        Start,
        Middle,
        End
    };

    // A string placed on its baseline, in this component's points. The anchor is
    // resolved at paint time rather than here, because resolving it needs the
    // width of the glyphs that will actually be drawn and only the renderer
    // knows that.
    struct TextRun
    {
        std::string text;
        Graphics::Point baseline;
        Graphics::Color colour;
        TextAnchor anchor = TextAnchor::Start;
        int fontIndex = 0;
    };

    // A renderer per (family, size) the document asks for. See the note on
    // getFontCount for what each one costs, and UI::Graphics::setTextRenderer
    // for why a document cannot simply use the host's.
    struct DocumentFont
    {
        DocumentFont(std::string familyToUse, float pointSizeToUse)
            : family(std::move(familyToUse))
            , pointSize(pointSizeToUse)
            , renderer(pointSizeToUse, family)
        {
        }

        std::string family;
        float pointSize = 0.f;
        Text::TextRenderer renderer;
    };

    // Document order, which is paint order: SVG has no z-index and later
    // elements cover earlier ones. Shapes and text runs live in their own
    // vectors because a PathShape cannot be moved -- it registers with its
    // component in its constructor -- so this is what keeps the two interleaved
    // the way the markup had them.
    struct Drawable
    {
        bool isText = false;
        int index = 0;
    };

    struct Style;

    void rebuild();
    void clearContent();

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
                  const UI::Gradient& gradient = {});

    // What a paint reference resolves to, against this document's ids and its
    // viewBox. See SVG::resolveGradient, which is where the two coordinate
    // systems are worked out.
    UI::Gradient gradientFor(const std::string& reference,
                             const Graphics::Rect& objectBounds,
                             const GPUWidgets::AffineTransform& transform) const;

    int findOrAddFont(const std::string& family, float pointSize);

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
    OwnedVector<Shape> shapes;
    Vector<TextRun> texts;
    OwnedVector<DocumentFont> fonts;

    // The previous build's renderers, between clearContent and the end of
    // rebuild. See clearContent for why they are kept and why they are not kept
    // for ever.
    OwnedVector<DocumentFont> spareFonts;
};
} // namespace eacp::SVG
