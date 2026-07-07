#pragma once

#include <cstddef>
#include <string_view>

namespace eacp::SVG
{

struct NumberReader
{
    bool atEnd() const;
    char peek() const;
    void skipWhitespaceAndCommas();
    bool hasNumber();
    float readFloat();

    std::string_view src;
    std::size_t pos = 0;
};

} // namespace eacp::SVG
