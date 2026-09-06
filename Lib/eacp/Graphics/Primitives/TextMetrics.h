#pragma once

#include "Font.h"

namespace eacp::Graphics
{

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
