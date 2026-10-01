#pragma once

#include <cctype>
#include <optional>
#include <string>

namespace term
{
// fzf-style case-insensitive subsequence match (same scorer as Wim's
// palette). Consecutive matches and word-boundary hits score higher; skipped
// text costs a little. Spaces in the query are ignored so "eacp term" can
// match "eacp — Terminal".
inline std::optional<int> fuzzyScore(const std::string& query,
                                     const std::string& text)
{
    auto lower = [](char c) { return (char) std::tolower((unsigned char) c); };

    auto isBoundary = [&](std::size_t index)
    {
        if (index == 0)
            return true;

        auto prev = (unsigned char) text[index - 1];
        return !std::isalnum(prev);
    };

    auto score = 0;
    auto consecutive = false;
    std::size_t position = 0;

    for (auto queryChar: query)
    {
        if (queryChar == ' ')
            continue;

        auto found = false;

        for (auto i = position; i < text.size(); ++i)
        {
            if (lower(text[i]) != lower(queryChar))
                continue;

            score += consecutive && i == position ? 8 : 1;

            if (isBoundary(i))
                score += 4;

            score -= (int) std::min<std::size_t>(i - position, 3);
            position = i + 1;
            consecutive = true;
            found = true;
            break;
        }

        if (!found)
            return std::nullopt;
    }

    return score;
}
} // namespace term
