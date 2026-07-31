#include "NumberReader.h"

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

bool NumberReader::readFlag()
{
    skipWhitespaceAndCommas();

    if (!atEnd() && (peek() == '0' || peek() == '1'))
        return src[pos++] == '1';

    // Not what the grammar allows, but a document that writes "0.0" where a flag
    // belongs means false by it, and refusing to read the number would put every
    // later coordinate of the command one place out.
    return readFloat() != 0.f;
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
