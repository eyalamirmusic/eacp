#pragma once

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
// What rung 1 does not do: gradients, clip paths and masks, group opacity as
// compositing, defs/use, CSS selectors, filters, images, and elliptical arcs.
// An element asking for one of those draws without it rather than not at all.
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

    void buildElement(const SVGElement& element, const Style& inherited);
    void buildShapes(const SVGElement& element, const Style& style);
    void buildTextRun(const SVGElement& element, const Style& style);

    void addShape(const GPUWidgets::Path& path,
                  const Graphics::Color& colour,
                  GPUWidgets::FillRule rule);

    int findOrAddFont(const std::string& family, float pointSize);

    // The document's own units onto this component's bounds: the viewBox origin
    // moved to zero, then stretched to fill. preserveAspectRatio is not read,
    // so a document whose aspect differs from its component distorts rather than
    // letterboxes.
    GPUWidgets::AffineTransform documentToComponent() const;

    SVGElement documentRoot;
    Graphics::Rect viewBox;
    float documentWidth = 0.f;
    float documentHeight = 0.f;

    Vector<Drawable> order;
    OwnedVector<Shape> shapes;
    Vector<TextRun> texts;
    OwnedVector<DocumentFont> fonts;
};
} // namespace eacp::SVG
