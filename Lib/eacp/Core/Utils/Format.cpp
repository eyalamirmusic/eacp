#include "Format.h"

// The one place in eacp that includes fmt; see Format.h.
#include <fmt/args.h>
#include <fmt/format.h>

#include <cmath>
#include <cstdlib>

namespace eacp::Strings
{
namespace
{
using ArgStore = fmt::dynamic_format_arg_store<fmt::format_context>;

void pushArg(ArgStore& store, const FormatArg& arg)
{
    std::visit(
        [&store](const auto& value)
        {
            using T = std::decay_t<decltype(value)>;

            if constexpr (std::is_same_v<T, std::string>)
                store.push_back(std::string_view {value});
            else
                store.push_back(value);
        },
        arg);
}

std::string formatErrorText(std::string_view pattern, const char* what)
{
    return concat("[format error: ", what, "] ", pattern);
}
} // namespace

std::string vformat(std::string_view pattern, std::span<const FormatArg> args)
{
    auto store = ArgStore {};
    store.reserve(args.size(), 0);

    for (const auto& arg: args)
        pushArg(store, arg);

    try
    {
        return fmt::vformat(pattern, store);
    }
    catch (const fmt::format_error& error)
    {
        return formatErrorText(pattern, error.what());
    }
}

std::string shortestRoundTrip(float value)
{
    for (auto precision = 6; precision < 9; ++precision)
    {
        auto text = format("{:.{}g}", value, precision);

        if (!std::isfinite(value) || std::strtof(text.c_str(), nullptr) == value)
            return text;
    }

    return format("{:.9g}", value);
}
} // namespace eacp::Strings
