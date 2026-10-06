#include "UnicodeEmoji.h"

#include <algorithm>
#include <iterator>

namespace eacp::Text
{
bool hasEmojiPresentation(char32_t codepoint)
{
    const auto* begin = std::begin(emojiPresentationRanges);
    const auto* end = std::end(emojiPresentationRanges);

    const auto* found = std::upper_bound(begin,
                                         end,
                                         codepoint,
                                         [](char32_t value, const EmojiRange& range)
                                         { return value < range.first; });

    return found != begin && codepoint <= (found - 1)->last;
}

bool wantsEmojiPresentation(char32_t codepoint, char32_t next)
{
    if (next == emojiVariationSelector)
        return true;

    if (next == textVariationSelector)
        return false;

    return hasEmojiPresentation(codepoint);
}
} // namespace eacp::Text
