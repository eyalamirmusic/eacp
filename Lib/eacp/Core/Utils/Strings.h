#pragma once

#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace eacp::Strings
{
std::string trim(const std::string& s);
std::string toLower(const std::string& s);

bool equalsCaseInsensitive(const std::string& a, const std::string& b);

int hexCharToInt(char c);

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
