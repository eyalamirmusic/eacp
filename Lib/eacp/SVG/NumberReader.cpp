#include "NumberReader.h"

#include <cctype>
#include <string>

namespace eacp::SVG
{

namespace
{
bool isAsciiSpace(char c)
{
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

bool isAsciiDigit(char c)
{
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

bool isNumberStart(char c)
{
    return isAsciiDigit(c) || c == '-' || c == '+' || c == '.';
}
} // namespace

bool NumberReader::atEnd() const
{
    return pos >= src.size();
}

char NumberReader::peek() const
{
    return src[pos];
}

void NumberReader::skipWhitespaceAndCommas()
{
    while (!atEnd() && (isAsciiSpace(peek()) || peek() == ','))
        ++pos;
}

bool NumberReader::hasNumber()
{
    skipWhitespaceAndCommas();
    return !atEnd() && isNumberStart(peek());
}

float NumberReader::readFloat()
{
    skipWhitespaceAndCommas();

    auto start = pos;

    if (!atEnd() && (peek() == '-' || peek() == '+'))
        ++pos;

    while (!atEnd() && isAsciiDigit(peek()))
        ++pos;

    if (!atEnd() && peek() == '.')
    {
        ++pos;
        while (!atEnd() && isAsciiDigit(peek()))
            ++pos;
    }

    if (!atEnd() && (peek() == 'e' || peek() == 'E'))
    {
        ++pos;
        if (!atEnd() && (peek() == '-' || peek() == '+'))
            ++pos;
        while (!atEnd() && isAsciiDigit(peek()))
            ++pos;
    }

    if (pos == start)
        return 0.f;

    return std::stof(std::string(src.substr(start, pos - start)));
}

} // namespace eacp::SVG
