#include "Strings.h"

#include <cctype>
#include <cstddef>

namespace eacp::Strings
{
namespace
{
constexpr auto replacementChar = char32_t {0xFFFD};

// D800..DBFF and DC00..DFFF are the two halves of a UTF-16 surrogate pair.
// Neither is a scalar value on its own, so a UTF-8 sequence decoding to one is
// ill-formed (CESU-8 and friends) and an unpaired half in wide input has nothing
// to encode; both are replaced.
bool isLeadSurrogate(char32_t unit)
{
    return unit >= 0xD800 && unit <= 0xDBFF;
}

bool isTrailSurrogate(char32_t unit)
{
    return unit >= 0xDC00 && unit <= 0xDFFF;
}

bool isSurrogate(char32_t unit)
{
    return isLeadSurrogate(unit) || isTrailSurrogate(unit);
}

void appendUtf8(std::string& out, char32_t codepoint)
{
    if (codepoint < 0x80)
    {
        out += static_cast<char>(codepoint);
    }
    else if (codepoint < 0x800)
    {
        out += static_cast<char>(0xC0 | (codepoint >> 6));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    else if (codepoint < 0x10000)
    {
        out += static_cast<char>(0xE0 | (codepoint >> 12));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (codepoint >> 18));
        out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
}

// Where wchar_t is 16 bits the astral planes need a surrogate pair; where it is
// 32 the codepoint is the code unit and this is a push_back.
void appendWide(std::wstring& out, char32_t codepoint)
{
    if constexpr (sizeof(wchar_t) == 2)
    {
        if (codepoint >= 0x10000)
        {
            codepoint -= 0x10000;
            out += static_cast<wchar_t>(0xD800 + (codepoint >> 10));
            out += static_cast<wchar_t>(0xDC00 + (codepoint & 0x3FF));
            return;
        }
    }

    out += static_cast<wchar_t>(codepoint);
}
} // namespace

std::wstring widen(std::string_view utf8)
{
    auto out = std::wstring {};
    out.reserve(utf8.size());

    const auto size = utf8.size();
    for (auto i = std::size_t {0}; i < size;)
    {
        const auto lead = static_cast<unsigned char>(utf8[i]);

        auto length = std::size_t {0};
        auto codepoint = char32_t {};

        if (lead < 0x80)
        {
            length = 1;
            codepoint = lead;
        }
        else if ((lead & 0xE0) == 0xC0)
        {
            length = 2;
            codepoint = lead & 0x1F;
        }
        else if ((lead & 0xF0) == 0xE0)
        {
            length = 3;
            codepoint = lead & 0x0F;
        }
        else if ((lead & 0xF8) == 0xF0)
        {
            length = 4;
            codepoint = lead & 0x07;
        }
        else
        {
            appendWide(out, replacementChar);
            ++i;
            continue;
        }

        auto valid = i + length <= size;
        for (auto j = std::size_t {1}; valid && j < length; ++j)
        {
            const auto byte = static_cast<unsigned char>(utf8[i + j]);
            if ((byte & 0xC0) != 0x80)
                valid = false;
            else
                codepoint = (codepoint << 6) | (byte & 0x3F);
        }

        // A truncated or mis-continued sequence costs one byte, not the whole
        // sequence: resuming at i + 1 lets the next valid lead byte resynchronise
        // instead of swallowing text that follows the damage.
        if (!valid)
        {
            appendWide(out, replacementChar);
            ++i;
            continue;
        }

        // Rejects the overlong encodings (a codepoint spelled with more bytes
        // than it needs), anything past the Unicode maximum, and surrogates.
        constexpr char32_t minima[] = {0, 0, 0x80, 0x800, 0x10000};
        if (codepoint < minima[length] || codepoint > 0x10FFFF
            || isSurrogate(codepoint))
            codepoint = replacementChar;

        appendWide(out, codepoint);
        i += length;
    }

    return out;
}

std::string narrow(std::wstring_view wide)
{
    auto out = std::string {};
    out.reserve(wide.size());

    for (auto i = std::size_t {0}; i < wide.size(); ++i)
    {
        auto codepoint = static_cast<char32_t>(
            static_cast<std::make_unsigned_t<wchar_t>>(wide[i]));

        if constexpr (sizeof(wchar_t) == 2)
        {
            // A lead half only means something with its trail: take the pair
            // together, and replace either one found on its own.
            if (isLeadSurrogate(codepoint))
            {
                const auto next =
                    i + 1 < wide.size()
                        ? static_cast<char32_t>(
                              static_cast<std::make_unsigned_t<wchar_t>>(
                                  wide[i + 1]))
                        : char32_t {0};

                if (isTrailSurrogate(next))
                {
                    codepoint =
                        0x10000 + ((codepoint - 0xD800) << 10) + (next - 0xDC00);
                    ++i;
                }
                else
                {
                    codepoint = replacementChar;
                }
            }
            else if (isTrailSurrogate(codepoint))
            {
                codepoint = replacementChar;
            }
        }
        else
        {
            if (codepoint > 0x10FFFF || isSurrogate(codepoint))
                codepoint = replacementChar;
        }

        appendUtf8(out, codepoint);
    }

    return out;
}

std::string trim(std::string_view s)
{
    auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos)
        return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return std::string {s.substr(begin, end - begin + 1)};
}

std::string toLower(std::string_view s)
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
