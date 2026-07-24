#include "FilePath.h"

#include <cstddef>
#include <type_traits>

namespace eacp
{
namespace
{
constexpr auto replacementChar = char32_t {0xFFFD};

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

bool isLeadSurrogate(char32_t unit)
{
    return unit >= 0xD800 && unit <= 0xDBFF;
}

bool isTrailSurrogate(char32_t unit)
{
    return unit >= 0xDC00 && unit <= 0xDFFF;
}
} // namespace

FilePath::FilePath(std::string textToUse)
    : text(std::move(textToUse))
{
}

FilePath::FilePath(std::string_view textToUse)
    : text(textToUse)
{
}

FilePath::FilePath(const char* textToUse)
    : text(textToUse)
{
}

const std::string& FilePath::str() const
{
    return text;
}

const char* FilePath::c_str() const
{
    return text.c_str();
}

bool FilePath::empty() const
{
    return text.empty();
}

std::string FilePath::extension() const
{
    auto separator = text.find_last_of("/\\");
    auto start = separator == std::string::npos ? 0 : separator + 1;
    auto dot = text.find_last_of('.');

    if (dot == std::string::npos || dot <= start)
        return {};

    return text.substr(dot);
}

FilePath FilePath::parentDirectory() const
{
    auto separator = text.find_last_of("/\\");

    if (separator == std::string::npos)
        return {};

    if (separator == 0)
        return FilePath {"/"};

    return FilePath {text.substr(0, separator)};
}

std::wstring FilePath::wide() const
{
    auto out = std::wstring {};
    out.reserve(text.size());

    const auto size = text.size();
    for (auto i = std::size_t {0}; i < size;)
    {
        const auto lead = static_cast<unsigned char>(text[i]);

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
            const auto byte = static_cast<unsigned char>(text[i + j]);
            if ((byte & 0xC0) != 0x80)
                valid = false;
            else
                codepoint = (codepoint << 6) | (byte & 0x3F);
        }

        if (!valid)
        {
            appendWide(out, replacementChar);
            ++i;
            continue;
        }

        constexpr char32_t minima[] = {0, 0, 0x80, 0x800, 0x10000};
        if (codepoint < minima[length] || codepoint > 0x10FFFF
            || isLeadSurrogate(codepoint) || isTrailSurrogate(codepoint))
            codepoint = replacementChar;

        appendWide(out, codepoint);
        i += length;
    }

    return out;
}

void FilePath::assignFromWide(std::wstring_view wide)
{
    text.clear();
    text.reserve(wide.size());

    for (auto i = std::size_t {0}; i < wide.size(); ++i)
    {
        auto codepoint = static_cast<char32_t>(
            static_cast<std::make_unsigned_t<wchar_t>>(wide[i]));

        if constexpr (sizeof(wchar_t) == 2)
        {
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
            if (codepoint > 0x10FFFF || isLeadSurrogate(codepoint)
                || isTrailSurrogate(codepoint))
                codepoint = replacementChar;
        }

        if (codepoint == U'\\')
            codepoint = U'/';

        appendUtf8(text, codepoint);
    }
}

FilePath FilePath::operator/(std::string_view part) const
{
    auto joined = text;
    if (!joined.empty() && joined.back() != '/')
        joined += '/';

    joined += part;
    return FilePath {std::move(joined)};
}
} // namespace eacp
