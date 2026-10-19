#include "Strings.h"

#include <cctype>
#include <cerrno>
#include <clocale>
#include <cmath>
#include <cstdlib>

#if defined(__APPLE__)
#include <xlocale.h>
#elif !defined(_WIN32) && !defined(__ANDROID__)
#include <locale.h>
#endif

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

    const auto size = (int) utf8.size();
    for (auto i = 0; i < size;)
    {
        const auto lead = static_cast<unsigned char>(utf8[i]);

        auto length = 0;
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
        for (auto j = 1; valid && j < length; ++j)
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

    const auto size = (int) wide.size();

    for (auto i = 0; i < size; ++i)
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
                    i + 1 < size ? static_cast<char32_t>(
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

std::string toString(const std::string& s)
{
    return s;
}

std::string toString(std::string_view s)
{
    return std::string {s};
}

std::string toString(const char* s)
{
    return std::string {s};
}

std::string toString(char* s)
{
    return std::string {s};
}

std::string toString(char c)
{
    return std::string(1, c);
}

std::string toString(bool b)
{
    return b ? "true" : "false";
}

std::string trim(std::string_view s)
{
    auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos)
        return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return std::string {s.substr(begin, end - begin + 1)};
}

namespace
{
std::string_view::size_type lineStart(std::string_view text,
                                      std::string_view::size_type position)
{
    if (position == 0)
        return 0;

    auto previousBreak = text.find_last_of("\r\n", position - 1);
    return previousBreak == std::string_view::npos ? 0 : previousBreak + 1;
}
} // namespace

std::string lastNonEmptyLine(std::string_view text)
{
    auto end = text.find_last_not_of("\r\n \t");

    if (end == std::string_view::npos)
        return {};

    auto first = lineStart(text, end);
    return std::string {text.substr(first, end + 1 - first)};
}

std::string lineContaining(std::string_view text, std::string_view needle)
{
    auto found = text.find(needle);

    if (found == std::string_view::npos)
        return {};

    auto first = lineStart(text, found);
    auto last = text.find_first_of("\r\n", found);
    auto length = last == std::string_view::npos ? last : last - first;
    return std::string {text.substr(first, length)};
}

std::string toLower(std::string_view s)
{
    auto result = std::string {};
    result.reserve(s.size());
    for (auto c: s)
        result.push_back((char) std::tolower((unsigned char) c));
    return result;
}

std::string capitalize(std::string_view s)
{
    auto result = std::string {s};

    if (!result.empty())
        result[0] = (char) std::toupper((unsigned char) result[0]);

    return result;
}

std::string uncapitalize(std::string_view s)
{
    auto result = std::string {s};

    if (!result.empty())
        result[0] = (char) std::tolower((unsigned char) result[0]);

    return result;
}

bool equalsCaseInsensitive(std::string_view a, std::string_view b)
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

namespace
{
bool isAsciiDigit(char c)
{
    return c >= '0' && c <= '9';
}

bool startsWithHexPrefix(std::string_view s)
{
    return s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
}

// strtod reads a superset of what from_chars does: leading whitespace, a '+',
// hex floats. Anything outside from_chars' grammar is turned away up front so
// both halves of tryParse accept the same kind of text.
bool looksLikeAFromCharsNumber(std::string_view s)
{
    auto body = !s.empty() && s.front() == '-' ? s.substr(1) : s;

    if (body.empty() || startsWithHexPrefix(body))
        return false;

    auto first = body.front();
    auto lower = static_cast<char>(std::tolower(static_cast<unsigned char>(first)));

    return isAsciiDigit(first) || first == '.' || lower == 'i' || lower == 'n';
}

#if defined(_WIN32)
_locale_t cLocale()
{
    static auto locale = _create_locale(LC_ALL, "C");
    return locale;
}

float readFloating(const char* text, char** end, float*)
{
    return _strtof_l(text, end, cLocale());
}

double readFloating(const char* text, char** end, double*)
{
    return _strtod_l(text, end, cLocale());
}
#elif defined(__ANDROID__)
// Bionic's strtod ignores LC_NUMERIC, so the plain functions are already the
// C locale's.
float readFloating(const char* text, char** end, float*)
{
    return std::strtof(text, end);
}

double readFloating(const char* text, char** end, double*)
{
    return std::strtod(text, end);
}
#else
locale_t cLocale()
{
    static auto locale = newlocale(LC_ALL_MASK, "C", locale_t {});
    return locale;
}

float readFloating(const char* text, char** end, float*)
{
    return strtof_l(text, end, cLocale());
}

double readFloating(const char* text, char** end, double*)
{
    return strtod_l(text, end, cLocale());
}
#endif

template <typename T>
bool parseFloatingAs(std::string_view s, T& out)
{
    if (!looksLikeAFromCharsNumber(s))
        return false;

    // strtod needs a terminator, and a NUL inside the view stops it early, so
    // the end check below rejects one.
    auto text = std::string {s};
    auto* end = static_cast<char*>(nullptr);

    errno = 0;
    auto value = readFloating(text.c_str(), &end, static_cast<T*>(nullptr));

    if (end != text.c_str() + text.size())
        return false;

    if (errno == ERANGE && std::isinf(value))
        return false;

    out = value;
    return true;
}
} // namespace

bool Detail::parseFloating(std::string_view s, float& out)
{
    return parseFloatingAs(s, out);
}

bool Detail::parseFloating(std::string_view s, double& out)
{
    return parseFloatingAs(s, out);
}

std::optional<bool> tryParseBool(std::string_view s)
{
    for (auto word: {"true", "yes", "on", "1"})
        if (equalsCaseInsensitive(s, word))
            return true;

    for (auto word: {"false", "no", "off", "0"})
        if (equalsCaseInsensitive(s, word))
            return false;

    return std::nullopt;
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
