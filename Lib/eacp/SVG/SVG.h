#pragma once

#include "SVGAttributes.h"
#include "SVGClip.h"
#include "SVGComponent.h"
#include "SVGGeometry.h"
#include "SVGGradient.h"
#include "SVGImage.h"
#include "SVGPathParser.h"
#include "XMLParser.h"

// The native render tier - SVGView over retained shape and text layers - which
// only exists where the platform has a 2D context of its own.
#if EACP_HAS_CONTEXT
    #include "SVGParser.h"
#endif
