#pragma once

#include "SVGAttributes.h"
#include "SVGClip.h"
#include "SVGComponent.h"
#include "SVGGeometry.h"
#include "SVGGradient.h"
#include "SVGImage.h"
#include "SVGPathParser.h"
#include "XMLParser.h"

// Reached through eacp-svg's PUBLIC link to eacp-graphics, which defines it.
#ifndef EACP_HAS_CONTEXT
    #error "EACP_HAS_CONTEXT is undefined - link eacp-svg"
#endif

// The native render tier - SVGView over retained shape and text layers - which
// only exists where the platform has a 2D context of its own.
#if EACP_HAS_CONTEXT
    #include "SVGParser.h"
#endif
