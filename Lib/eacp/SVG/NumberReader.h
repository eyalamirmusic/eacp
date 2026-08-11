#pragma once

#include "Common.h"

namespace eacp::SVG
{

struct NumberReader
{
    bool atEnd() const;
    char peek() const;
    void skipWhitespaceAndCommas();
    bool hasNumber();
    float readFloat();

    // An arc flag is a single '0' or '1', not a number: the grammar lets it run
    // into what follows, so "a5 5 0 0110 0" is 0, 1, then the point (10, 0).
    bool readFlag();

    std::string_view src;
    std::size_t pos = 0;
};

} // namespace eacp::SVG
