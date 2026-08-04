#pragma once

#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace eacp::Strings
{
// Strips leading and trailing spaces, tabs, CRs and LFs.
std::string trim(std::string_view s);
// ASCII-only lowercasing, for matching extensions, header names and the like.
std::string toLower(std::string_view s);

// Changes the case of the first character only, leaving the rest as written -
// for identifiers and labels, where lowercasing the whole string would destroy
// casing that is already meaningful ("Escape", not "ESCAPE"). Empty in, empty
// out.
std::string capitalize(std::string_view s);
std::string uncapitalize(std::string_view s);

bool equalsCaseInsensitive(std::string_view a, std::string_view b);

int hexCharToInt(char c);

// UTF-8 -> the platform's wide encoding: UTF-16 where wchar_t is 16 bits
// (Windows), UTF-32 where it is 32 (Linux, macOS). This is the single place the
// framework crosses that boundary - every Win32 -W call, Media Foundation URL
// and DirectWrite string goes through here.
//
// Hand-rolled rather than MultiByteToWideChar so one implementation serves every
// platform, and so the answer never depends on a code page or a locale.
//
// Malformed input is not an error: each ill-formed byte becomes U+FFFD and the
// scan resumes at the next one, so this neither throws nor fails. Callers that
// must reject bad UTF-8 have to check before converting.
std::wstring widen(std::string_view utf8);

// The reverse leg, on the same contract: unpaired surrogates and units past
// U+10FFFF become U+FFFD, so it neither throws nor fails. Together these two are
// the framework's only encoding conversions - nothing else should call
// MultiByteToWideChar or WideCharToMultiByte.
std::string narrow(std::wstring_view wide);

// Number/bool → string. String-like and char inputs pass through unchanged so
// callers can concatenate heterogeneous values without minding their types.
// This is the single place the framework turns a value into text (see LOG).
inline std::string toString(const std::string& s)
{
    return s;
}
inline std::string toString(std::string_view s)
{
    return std::string {s};
}
inline std::string toString(const char* s)
{
    return std::string {s};
}
inline std::string toString(char* s)
{
    return std::string {s};
}
inline std::string toString(char c)
{
    return std::string(1, c);
}
inline std::string toString(bool b)
{
    return b ? "true" : "false";
}

template <typename T>
std::string toString(const T& value)
{
    if constexpr (std::is_enum_v<T>)
        return std::to_string(static_cast<std::underlying_type_t<T>>(value));
    else
    {
        static_assert(std::is_arithmetic_v<T>,
                      "toString handles strings, bool, char, enums and numbers");
        return std::to_string(value);
    }
}

// Concatenates any mix of the above into one string, left to right.
template <typename... Args>
std::string concat(const Args&... args)
{
    return (std::string {} + ... + toString(args));
}

// string → number, the counterpart to toString. Uses from_chars, so it neither
// throws nor depends on the locale, and it rejects trailing junk. Returns
// nullopt unless the whole string is a valid T.
template <typename T>
std::optional<T> tryParse(std::string_view s)
{
    static_assert(std::is_integral_v<T>,
                  "tryParse<T> handles integers; use tryParseFloat for reals");
    auto value = T {};
    auto* last = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(s.data(), last, value);
    if (ec != std::errc {} || ptr != last)
        return std::nullopt;
    return value;
}

template <typename T>
T parseOr(std::string_view s, T fallback)
{
    return tryParse<T>(s).value_or(fallback);
}

std::optional<float> tryParseFloat(const std::string& s);
std::optional<int> tryParseInt(const std::string& s);

float parseFloatOr(const std::string& s, float fallback = 0.f);
int parseIntOr(const std::string& s, int fallback = 0);
} // namespace eacp::Strings
