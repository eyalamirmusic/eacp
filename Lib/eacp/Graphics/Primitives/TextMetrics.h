#pragma once

/** @file Measuring text in a Font. EACP_HAS_CONTEXT only (Apple and Windows). */

#include "Font.h"

namespace eacp::Graphics
{

/// Static measurements in points, through Core Text on Apple and DirectWrite on
/// Windows. Indices are byte offsets into the UTF-8 string.
struct TextMetrics
{
    static float measureWidth(const std::string& text, const Font& font);
    static float
        getOffsetForIndex(const std::string& text, int index, const Font& font);
    static int
        getIndexForOffset(const std::string& text, float xOffset, const Font& font);
    static float getLineHeight(const Font& font);
    static float getAscent(const Font& font);
    static float getDescent(const Font& font);
};

} // namespace eacp::Graphics
