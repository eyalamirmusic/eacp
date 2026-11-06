#pragma once

/** @file
    SVG documents: an XML parser, the attribute, path, gradient and clip
    readers, and `SVGComponent`, the one renderer, which draws a document
    through the `UI` component tier on the GPU. There is no Context-based SVG
    renderer. Built where `EACP_HAS_TEXT` is on: Apple, Windows, Linux and
    Android.

    Entry points: `SVGComponent` to show a document in a component tree,
    `renderToImage` for pixels off screen (main thread; an invalid Image when
    there is no GPU device or the markup does not parse), `parseXML` for the
    element tree alone.
*/

#include "SVGAttributes.h"
#include "SVGClip.h"
#include "SVGComponent.h"
#include "SVGGeometry.h"
#include "SVGGradient.h"
#include "SVGImage.h"
#include "SVGPathParser.h"
#include "XMLParser.h"
