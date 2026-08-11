#pragma once

#include "SVGAttributes.h"
#include "SVGClip.h"
#include "SVGElement.h"

#include <eacp/UI/UI.h>

namespace eacp::SVG
{
// An SVG document drawn as one UI::Component, one UI::PathShape per shape.
// Unsupported: <mask>, CSS selectors (the style attribute is read, <style> is
// not), filters, images, and the focal point of a radial gradient.
class SVGComponent : public UI::Component
{
public:
    SVGComponent();
    ~SVGComponent() override;

    // Copied, since the geometry is rebuilt from it on every resize.
    void setDocument(const SVGElement& root);

    // The space the geometry is authored in: the viewBox, else width/height.
    Graphics::Rect getViewBox() const { return viewBox; }

    PreserveAspectRatio getAspectRatio() const { return aspectRatio; }

    // The document's units onto this component's points.
    GPUWidgets::AffineTransform documentToComponent() const;

    float getDocumentWidth() const { return documentWidth; }
    float getDocumentHeight() const { return documentHeight; }

    void resized() override;
    void paint(UI::Graphics& g) override;

    // Masks, not elements: one that is both filled and stroked counts twice.
    int getShapeCount() const { return shapes.size(); }

    int getOpacityGroupCount() const { return groups.size(); }

    // Distinct regions, not elements: a group's clip is one region however many
    // children it cuts.
    int getClipCount() const { return clips.size(); }

    // Of those, the ones that took a mask. The rest are scissor rects.
    int getClipMaskCount() const;

    // Of those, the ones the coverage atlas had no room for -- each one missing
    // from the picture. See ComponentHost::getLastDroppedPathCount.
    int getDroppedShapeCount() const;

    // And the ones drawn as triangles instead. See UI::PathShape::Backing.
    int getMeshedShapeCount() const;

    // Every mask's bounds added up, in this component's points squared.
    float getTotalMaskArea() const;

    // The same sum over the shapes that actually took an atlas mask.
    float getAtlasMaskArea() const;

    // Distinct (family, size, style) faces, all sharing one glyph atlas.
    int getFontCount() const;

private:
    // What a clip-path came to for one drawable: the region multiplying its
    // coverage, and the rectangle everything rectangular about its clips
    // intersected to. Both, since only the rectangle reaches the text renderer.
    struct ClipState
    {
        int maskIndex = -1;
        Graphics::Rect rect;
        bool hasRect = false;

        bool isEmpty() const { return maskIndex < 0 && !hasRect; }
    };

    // One filled region. A stroke is one of these too, its geometry being the
    // region the pen covers rather than the pen's path.
    struct Shape
    {
        explicit Shape(UI::Component& owner)
            : mask(owner)
        {
        }

        UI::PathShape mask;
        Graphics::Color colour;
        ClipState clip;

        // Resolved at build time, since placing a gradient in bounding-box
        // units needs the geometry.
        UI::Gradient gradient;

        // The geometry's own bounds: the mask's are only known once a kernel
        // has rasterized it, and this has to be readable before that.
        Graphics::Rect maskBounds;
    };

    enum class TextAnchor
    {
        Start,
        Middle,
        End
    };

    // A string placed on its baseline, in this component's points. The anchor
    // is resolved at paint time, needing the width of the glyphs to be drawn.
    struct TextRun
    {
        std::string text;
        Graphics::Point baseline;
        Graphics::Color colour;
        TextAnchor anchor = TextAnchor::Start;

        UI::Font font;

        // Only the rectangle of it ever applies. See ClipState.
        ClipState clip;
    };

    // A clip region, built once however many drawables it cuts. Shared across a
    // group's children, but not across the elements of a clip in bounding-box
    // units, which is placed against each of them.
    struct Clip
    {
        explicit Clip(UI::Component& owner)
            : mask(owner)
        {
        }

        std::string reference;
        GPUWidgets::AffineTransform transform;
        Graphics::Rect objectBounds;

        // Unused for a rectangular clip: bounds are the whole of it.
        UI::PathShape mask;
        bool isRectangle = false;

        Graphics::Rect bounds;
    };

    // Document order, which is paint order. The three kinds live in their own
    // vectors because neither a PathShape nor a Layer can be moved, so this is
    // what keeps them interleaved the way the markup had them.
    struct Drawable
    {
        enum class Kind
        {
            Shape,
            Text,
            Group
        };

        Kind kind = Kind::Shape;
        int index = 0;
    };

    // A container composited into a texture of its own, so its opacity fades
    // the group rather than each child. Built innermost-first, since UI::Layer
    // renders layers in the order they registered.
    struct OpacityGroup
    {
        explicit OpacityGroup(UI::Component& owner)
            : layer(owner)
        {
        }

        UI::Layer layer;
        Vector<Drawable> content;
    };

    struct Style;

    void rebuild();
    void clearContent();

    void paintDrawables(UI::Graphics& g, const Vector<Drawable>& drawables);

    void buildElementContent(const SVGElement& element,
                             const Style& style,
                             int depth);

    void buildOpacityGroup(const SVGElement& element,
                           const Style& style,
                           int depth,
                           float opacity);

    // Where a composited group's content reaches, in this component's points. A
    // run holding text takes the whole component, its extent being unmeasured.
    Graphics::Rect boundsOf(const Vector<Drawable>& drawables) const;

    static void applyPresentationAttributes(Style& style, const SVGElement& element);

    // `depth` counts <use> indirections, not tree depth: the specification
    // forbids a reference cycle and nothing stops a file writing one.
    void buildElement(const SVGElement& element, const Style& inherited, int depth);
    void buildShapes(const SVGElement& element, const Style& style);
    void buildTextRun(const SVGElement& element, const Style& style);

    void buildUse(const SVGElement& element, const Style& style, int depth);

    // A <symbol> or nested <svg>: its viewBox mapped onto the use site's box.
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

    ClipState resolveClips(const Style& style, const Graphics::Rect& objectBounds);

    // Negative where the reference resolves to nothing -- an id naming no
    // clipPath -- which the format says draws the element unclipped.
    int findOrAddClip(const std::string& reference,
                      const GPUWidgets::AffineTransform& transform,
                      const Graphics::Rect& objectBounds);

    UI::Gradient gradientFor(const std::string& reference,
                             const Graphics::Rect& objectBounds,
                             const GPUWidgets::AffineTransform& transform) const;

    SVGElement documentRoot;
    Graphics::Rect viewBox;
    PreserveAspectRatio aspectRatio;
    float documentWidth = 0.f;
    float documentHeight = 0.f;

    // Pointers into documentRoot, which is never edited after it is set.
    std::unordered_map<std::string, const SVGElement*> elementsById;

    Vector<Drawable> order;

    // Where the walk appends: the document's order, or a composited group's.
    Vector<Drawable>* output = &order;

    OwnedVector<Shape> shapes;
    OwnedVector<OpacityGroup> groups;
    OwnedVector<Clip> clips;
    Vector<TextRun> texts;
};
} // namespace eacp::SVG
