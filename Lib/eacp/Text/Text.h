#pragma once

/** @file
    GPU text: shaping, glyph rasterizing, a glyph atlas and an instanced glyph
    renderer, plus the Unicode bidirectional algorithm the Linux shaper runs.
    Built where `EACP_HAS_TEXT` is on: Apple, Windows, Linux and Android.
    The rasterizer is the platform's own (Core Text, DirectWrite, FreeType with
    HarfBuzz and fontconfig, android.graphics), so glyphs differ per platform.

    Entry points: `GlyphRasterizer` to shape and rasterize, `GlyphAtlas` to
    cache glyphs in a texture, `TextRenderer` to lay out and draw a string.
    Most apps draw text through `UI` components instead of using this directly.
    On Linux a font test self-skips when fontconfig finds no font.
*/

#include "Bidi.h"
#include "Font.h"
#include "GlyphAtlas.h"
#include "GlyphBitmap.h"
#include "GlyphRenderer.h"
#include "GlyphRasterizer.h"
#include "ShelfPacker.h"
#include "TextRenderer.h"
#include "Utf8.h"
