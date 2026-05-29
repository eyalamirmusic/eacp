#include "Strings.h"

#include <cctype>

namespace eacp::Strings
{
std::string trim(const std::string& s)
{
    auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::string toLower(const std::string& s)
{
    auto result = std::string {};
    result.reserve(s.size());
    for (auto c: s)
        result.push_back((char) std::tolower((unsigned char) c));
    return result;
}

bool equalsCaseInsensitive(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;

    return toLower(a) == toLower(b);
}

int hexCharToInt(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

std::optional<float> tryParseFloat(const std::string& s)
{
    try
    {
        return std::stof(s);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::optional<int> tryParseInt(const std::string& s)
{
    try
    {
        return std::stoi(s);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

float parseFloatOr(const std::string& s, float fallback)
{
    return tryParseFloat(s).value_or(fallback);
}

int parseIntOr(const std::string& s, int fallback)
{
    return tryParseInt(s).value_or(fallback);
}
} // namespace eacp::Strings
