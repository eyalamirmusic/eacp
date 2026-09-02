#pragma once

#include "Common.h"
#include "SVGElement.h"

#include <eacp/GPUWidgets/Path/AffineTransform.h>
#include <eacp/UI/Render/Gradient.h>

namespace eacp::SVG
{

struct ColorResult
{
    Graphics::Color color;
    bool isNone = false;
};

ColorResult parseColor(const std::string& value);

// A transform list flattened into fields, which loses what the list said.
// Ordering is gone - translate(..) rotate(..) and its reverse land here
// identically - a repeated function overwrites the one before it, and matrix,
// skewX and skewY have nowhere to go at all. Kept because SVGBuilder's native
// child views can only be moved, not transformed, so the translation is all it
// can use. Anything building geometry should read parseTransformMatrix.
struct Transform
{
    float translateX = 0.f;
    float translateY = 0.f;
    float scaleX = 1.f;
    float scaleY = 1.f;
    float rotateDeg = 0.f;
};

Transform parseTransform(const std::string& value);

// The same list as the matrix it actually denotes: every function of the
// specification, composed in the order written. Angles are degrees, as the
// format has them.
GPUWidgets::AffineTransform parseTransformMatrix(const std::string& value);

// How a viewBox is fitted to the viewport it is drawn into.
//
// The default is the interesting part: a document that says nothing gets
// xMidYMid meet, which is uniform and centred - *not* the stretch-to-fit that
// falls out of scaling each axis by itself. Which is why this exists at all: a
// logo in a component of the wrong aspect was quietly being made oval.
struct PreserveAspectRatio
{
    enum class Align
    {
        Min,
        Mid,
        Max
    };

    // False for the one value that distorts, preserveAspectRatio="none", where
    // each axis is scaled to fill its own and the alignment has nothing to do.
    bool uniform = true;

    Align x = Align::Mid;
    Align y = Align::Mid;

    // meet scales the box to fit inside the viewport and leaves the spare axis
    // empty; slice scales it to cover and lets the overflow run past the edges,
    // which the component's own clip cuts.
    bool slice = false;
};

PreserveAspectRatio parsePreserveAspectRatio(const std::string& value);

// The map from a viewBox onto the viewport it fills: the box's origin to zero,
// scaled by the fit, and shifted by whatever the alignment does with the spare
// space. What an <svg> does with its own viewBox and what a <symbol> does with
// the size its <use> gave it, which is why it is a free function rather than
// something the component keeps to itself.
GPUWidgets::AffineTransform viewBoxTransform(const Graphics::Rect& viewBox,
                                             const Graphics::Rect& viewport,
                                             const PreserveAspectRatio& fit);

// Which of the viewport's extents a percentage is a fraction of. SVG 1.1 §7.10
// gives an x-axis length the width, a y-axis one the height, and a length with
// no axis of its own -- a radius, a stroke width -- the diagonal over root two,
// which is the same length a gradient's `r` is resolved against.
enum class LengthAxis
{
    Horizontal,
    Vertical,
    Diagonal
};

// What a percentage is a percentage *of*: the nearest viewport, which is the
// viewBox of the closest enclosing <svg> where it brought one and that <svg>'s
// own width and height where it did not.
//
// Carried down the tree by whoever is walking it rather than looked up, because
// an element cannot see the ancestor that established it -- and the walk has to
// resolve the establishing element's own width and height against the viewport
// outside it before the new one exists.
struct Viewport
{
    float width = 0.f;
    float height = 0.f;

    float across(LengthAxis axis) const;

    bool operator==(const Viewport& other) const = default;
};

// A geometry attribute's value as a length: a plain number, or a percentage of
// the viewport along the axis it belongs to.
//
// A suffix that is not '%' is passed over, which is what reading one of these
// as a bare number has always done with it -- right for the px an SVG user unit
// already is, and a shrug at the absolute units almost nothing writes.
float parseLength(const std::string& value,
                  const Viewport& viewport,
                  LengthAxis axis,
                  float fallback = 0.f);

float lengthAttr(const SVGElement& element,
                 const std::string& name,
                 const Viewport& viewport,
                 LengthAxis axis,
                 float fallback = 0.f);

// Whether a root <svg>'s width or height says how big the *document* is.
//
// A percentage says how big whatever is showing it is, which is not something
// the document knows anything about, so it is exactly as intrinsic a size as no
// width at all -- and a caller asking for one has to fall back the same way,
// onto the viewBox's ratio or onto 300x150.
bool namesAnIntrinsicLength(const std::string& value);

// A style="..." attribute's declarations, by property name.
//
// The same properties a presentation attribute spells, in CSS's punctuation -
// and where a document writes both, this one wins. That is the whole of the
// cascade an SVG can rely on without a stylesheet, and it is enough, because a
// style attribute is what every drawing program emits.
//
// Selectors are a different project: this reads a declaration block and nothing
// else, so a <style> element in the document is still ignored.
std::unordered_map<std::string, std::string>
    parseStyleDeclarations(const std::string& value);

// An element's presentation properties, its style="..." declarations first and
// the attribute of the same name after.
//
// Which way round matters: a style attribute is a CSS declaration block, so
// where a document writes a property both ways the declaration is the one that
// wins. Drawing programs emit both constantly - the attribute for compatibility
// and the declaration for what they actually mean.
//
// Built once per element and then asked, because the declarations are parsed in
// the constructor: reading eight properties off one element is one parse.
struct PropertyReader
{
    explicit PropertyReader(const SVGElement& elementToUse)
        : element(elementToUse)
        , declarations(parseStyleDeclarations(elementToUse.attr("style")))
    {
    }

    std::string operator()(const std::string& name) const
    {
        auto found = declarations.find(name);

        return found != declarations.end() ? found->second : element.attr(name);
    }

    const SVGElement& element;
    std::unordered_map<std::string, std::string> declarations;
};

Vector<float> parseNumberList(const std::string& value);

Vector<Graphics::Point> parsePointList(const std::string& value);

// The id a paint value refers to, for `fill="url(#logoFade)"` and its stroke
// twin, or empty when the value is a plain colour. A url() the document wrote
// with a fallback after it -- `url(#missing) red` -- still names the reference;
// resolving it is the caller's business, and falling back is what it does when
// the id is not there.
std::string parsePaintReference(const std::string& value);

// The id an element's href names, or empty when it has none or names another
// document -- which there is nothing here to look in.
//
// Both spellings, because SVG 2 dropped the namespace and every file written
// before it still carries xlink:href. A document that writes both means the
// same thing twice.
std::string hrefId(const SVGElement& element);

} // namespace eacp::SVG
