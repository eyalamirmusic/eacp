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

    // One of an arc command's two flags, which is a single '0' or '1' and not a
    // number.
    //
    // The distinction is not pedantry: the grammar lets a flag run straight into
    // whatever follows it, so "a5 5 0 0110 0" is largeArc 0, sweep 1, then the
    // point (10, 0). Read as numbers those four characters are one value of 110,
    // and every minified path in the world is written that way.
    bool readFlag();

    std::string_view src;
    std::size_t pos = 0;
};

} // namespace eacp::SVG
