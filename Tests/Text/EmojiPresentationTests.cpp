#include "Common.h"

// The Unicode Emoji_Presentation property and the two variation selectors that
// override it. Pure logic over the generated table, so it runs everywhere;
// EmojiPresentation-LinuxTests.cpp covers the face the rasterizer then picks.

using namespace nano;
using namespace eacp;
using namespace eacp::Text;

namespace
{
constexpr char32_t grinningFace = 0x1F600;
constexpr char32_t regionalIndicatorA = 0x1F1E6;
constexpr char32_t heavyBlackHeart = 0x2764;
constexpr char32_t copyrightSign = 0x00A9;
constexpr char32_t watch = 0x231A;
} // namespace

auto tEmojiPresentationProperty = test("Text/emojiPresentationProperty") = []
{
    check(hasEmojiPresentation(grinningFace));
    check(hasEmojiPresentation(regionalIndicatorA));
    check(hasEmojiPresentation(watch));

    // Emoji, but text presentation until a selector says otherwise.
    check(!hasEmojiPresentation(heavyBlackHeart));
    check(!hasEmojiPresentation(copyrightSign));
    check(!hasEmojiPresentation(U'A'));

    // Either side of the watch, which is the shortest range in the table.
    check(!hasEmojiPresentation(watch - 1));
    check(!hasEmojiPresentation(watch + 2));

    // What the range test this replaced got wrong: neither end of
    // U+1F000..U+1FAFF carries the property.
    check(!hasEmojiPresentation(0x1F000));
    check(!hasEmojiPresentation(0x1FA00));
};

auto tVariationSelectorsOverride =
    test("Text/variationSelectorsOverridePresentation") = []
{
    check(!wantsEmojiPresentation(heavyBlackHeart, 0));
    check(wantsEmojiPresentation(heavyBlackHeart, emojiVariationSelector));

    check(!wantsEmojiPresentation(copyrightSign, 0));
    check(wantsEmojiPresentation(copyrightSign, emojiVariationSelector));

    check(wantsEmojiPresentation(grinningFace, 0));
    check(!wantsEmojiPresentation(grinningFace, textVariationSelector));

    check(wantsEmojiPresentation(regionalIndicatorA, 0));
};
