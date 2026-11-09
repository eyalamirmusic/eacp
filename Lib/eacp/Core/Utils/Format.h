#pragma once

#include "Strings.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace eacp::Strings
{
// Brace-pattern formatting: format("{} of {:08X}", n, code). The pattern is
// std::format/fmt syntax, but it is checked at runtime, not compile time - a
// malformed pattern or a spec that does not fit its argument comes back as a
// readable error string rather than a throw, so a bad log line never takes the
// caller down.
//
// The implementation is fmt, deliberately hidden behind vformat in Format.cpp:
// the arguments are type-erased into FormatArg here, so fmt never appears in a
// header and nothing outside eacp-core links or sees it.

// float stays apart from double so "{}" prints the float's own shortest
// round-trip ("0.1"), not the widened double's. The string alternatives are
// views into the caller's storage, which outlives the call; std::string holds
// what toString produced for anything else.
using FormatArg = std::variant<bool,
                               char,
                               long long,
                               unsigned long long,
                               float,
                               double,
                               std::string_view,
                               std::string,
                               const void*>;

template <typename T>
FormatArg toFormatArg(const T& value)
{
    using U = std::remove_cv_t<T>;

    if constexpr (std::is_same_v<U, bool> || std::is_same_v<U, char>)
        return value;
    else if constexpr (std::is_integral_v<U>)
    {
        if constexpr (std::is_signed_v<U>)
            return static_cast<long long>(value);
        else
            return static_cast<unsigned long long>(value);
    }
    else if constexpr (std::is_same_v<U, float>)
        return value;
    else if constexpr (std::is_floating_point_v<U>)
        return static_cast<double>(value);
    else if constexpr (std::is_null_pointer_v<U>)
        return static_cast<const void*>(nullptr);
    else if constexpr (std::is_convertible_v<const U&, std::string_view>)
        return std::string_view {value};
    else if constexpr (std::is_pointer_v<U>)
        return static_cast<const void*>(value);
    else
        return toString(value);
}

std::string vformat(std::string_view pattern, std::span<const FormatArg> args);

template <typename... Args>
std::string format(std::string_view pattern, const Args&... args)
{
    // Qualified: ADL on std::string_view finds std::vformat once <format> is in.
    if constexpr (sizeof...(Args) == 0)
        return Strings::vformat(pattern, {});
    else
    {
        const auto packed =
            std::array<FormatArg, sizeof...(Args)> {toFormatArg(args)...};
        return Strings::vformat(pattern, packed);
    }
}

// The shortest %g-style text ("{:.Ng}", N from 6 up to 9) that strtof reads
// back as exactly this float; non-finite values print as fmt spells them.
// Deliberately not fmt's "{}" shortest form, which switches to exponent
// notation from 1e7 up ("1.6777215e+07") where %g keeps the digits: callers
// emit this into shader source and host-visible parameter text, and their
// tests pin that notation.
std::string shortestRoundTrip(float value);
} // namespace eacp::Strings
