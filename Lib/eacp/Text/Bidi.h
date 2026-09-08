#pragma once

#include "UnicodeBidi.h"

#include <eacp/Core/Utils/Containers.h>

#include <cstdint>
#include <string_view>

namespace eacp::Text
{
// The Unicode Bidirectional Algorithm, UAX #9, as portable code: it turns a
// paragraph of logically ordered text into embedding levels and from those
// into the visual order a shaper is to place runs in.
//
// It lives here rather than in a rasterizer because only one of the three
// backends needs it. CoreText and DirectWrite run their own bidi inside
// CTLine and IDWriteTextLayout, so GlyphRasterizer::shape() already hands
// back visually ordered glyphs on Apple and Windows; the Linux backend hands
// HarfBuzz one run at a time and had nothing above it doing the reordering.
// Running this there makes the seam mean the same thing on all three, and
// running it on the other two would reorder what is already reordered.
enum class BidiBaseDirection
{
    // P2/P3: the first strong character outside an isolate decides.
    Auto,
    LeftToRight,
    RightToLeft,
};

// What X9 does to the embeddings, the overrides and the codepoints already
// BN: they take no part in the rest of the algorithm and are not in the
// visual order. The byte runs bidiRuns() reports do cover them, since a
// shaper has to be handed the whole string.
inline constexpr std::uint8_t bidiRemovedLevel = 0xFF;

// One embedding level per input codepoint, and the level the paragraph
// itself resolved to.
struct BidiLevels
{
    int paragraphLevel = 0;
    Vector<std::uint8_t> levels;
};

// P2-P3, X1-X10, W1-W7, N0-N2, I1-I2 and the L1 reset, over one paragraph.
BidiLevels bidiLevels(Span<const char32_t> text,
                      BidiBaseDirection base = BidiBaseDirection::Auto);

// L2: the indices of the codepoints the levels kept, left to right.
Vector<int> bidiVisualOrder(const BidiLevels& levels);

// A maximal stretch of one embedding level, as bytes of the UTF-8 that was
// analysed. Runs are contiguous and cover the whole text - a codepoint X9
// removed joins the run before it - so a shaper can walk them and shape
// every byte exactly once.
struct BidiRun
{
    int begin = 0;
    int end = 0;
    int level = 0;

    bool isRightToLeft() const { return (level & 1) != 0; }
};

// The paragraph's runs in visual order, left to right. Empty for empty text;
// one run for text with nothing to reorder, which is the common case and the
// one this is cheapest on.
Vector<BidiRun> bidiRuns(std::string_view text,
                         BidiBaseDirection base = BidiBaseDirection::Auto);

// True when the text has nothing for the algorithm to do: no right-to-left
// or Arabic-letter codepoint and no explicit formatting character, so the
// visual order is the logical one whatever the base direction is.
bool isLeftToRightOnly(std::string_view text);

// L4: a mirrored character is drawn as its mirror in a right-to-left run,
// so an opening parenthesis in Hebrew is the shape a reader expects there.
//
// A caller that places codepoints itself applies this; the Linux rasterizer
// does not, because HarfBuzz mirrors a right-to-left buffer on its own and
// mirroring twice is mirroring not at all.
inline char32_t bidiMirroredAt(char32_t codepoint, int level)
{
    return (level & 1) != 0 ? bidiMirroredGlyph(codepoint) : codepoint;
}
} // namespace eacp::Text
