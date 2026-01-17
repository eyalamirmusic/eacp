#pragma once

#include "Font.h"
#include <string>

namespace eacp::Graphics
{

struct TextMetrics
{
    static float measureWidth(const std::string& text, const Font& font);
    static float getOffsetForIndex(const std::string& text, size_t index, const Font& font);
    static size_t getIndexForOffset(const std::string& text, float xOffset, const Font& font);
    static float getLineHeight(const Font& font);
    static float getAscent(const Font& font);
    static float getDescent(const Font& font);
};

} // namespace eacp::Graphics
