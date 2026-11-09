#pragma once

#include "PluginExport.h"

#include <array>
#include <type_traits>

namespace eacp::Plugins
{
namespace Detail
{
template <typename Signature>
constexpr const char* signatureOf()
{
#if defined(_MSC_VER) && !defined(__clang__)
    return __FUNCSIG__;
#else
    return __PRETTY_FUNCTION__;
#endif
}

template <typename First, typename... Rest>
constexpr bool allDistinct()
{
    if constexpr (sizeof...(Rest) == 0)
        return true;
    else
        return (!std::is_same_v<First, Rest> && ...) && allDistinct<Rest...>();
}
} // namespace Detail

// One reloadable function: type-erased, with its signature as this compiler
// spells it, which is how the app finds it and checks a rebuilt module.
struct ReloadableEntry
{
    using Erased = void (*)();

    template <typename R, typename... Args>
    static ReloadableEntry of(R (*function)(Args...))
    {
        return {reinterpret_cast<Erased>(function),
                Detail::signatureOf<R(Args...)>()};
    }

    Erased function = nullptr;
    const char* signature = "";
};

// What a hot-reload module's one entry point returns: every function it lists.
struct ReloadableTable
{
    const ReloadableEntry* entries = nullptr;
    int count = 0;
};

namespace Detail
{
template <typename... Functions>
std::array<ReloadableEntry, sizeof...(Functions)>
    reloadableEntries(Functions... functions)
{
    static_assert(sizeof...(Functions) > 0, "EACP_RELOADABLE lists no function");
    static_assert(allDistinct<Functions...>(),
                  "EACP_RELOADABLE lists two functions of the same type: each is "
                  "looked up by its type, so every type may appear once");

    return {ReloadableEntry::of(functions)...};
}
} // namespace Detail
} // namespace eacp::Plugins

// The entry returns a C++ type from a C-linkage function, which module and app
// share since both are one eacp built by one compiler.
#if defined(__clang__)
#define EACP_RELOADABLE_ENTRY_BEGIN                                                 \
    _Pragma("clang diagnostic push")                                                \
        _Pragma("clang diagnostic ignored \"-Wreturn-type-c-linkage\"")
#define EACP_RELOADABLE_ENTRY_END _Pragma("clang diagnostic pop")
#else
#define EACP_RELOADABLE_ENTRY_BEGIN
#define EACP_RELOADABLE_ENTRY_END
#endif

EACP_RELOADABLE_ENTRY_BEGIN
EACP_HOT_RELOAD_EXPORT eacp::Plugins::ReloadableTable eacpReloadableTable();
EACP_RELOADABLE_ENTRY_END

#define EACP_RELOADABLE_STRINGIZE(name) #name

namespace eacp::Plugins
{
inline constexpr auto reloadableTableSymbol =
    EACP_RELOADABLE_STRINGIZE(eacpReloadableTable);
} // namespace eacp::Plugins

// Lists the functions a Plugins::Reloadable reloads, each of a distinct type.
// Used once per app, at global scope, after the functions (kept in an
// anonymous namespace). The table is a local static of this non-inline
// function, so the app's copy and the module's never merge.
#define EACP_RELOADABLE(...)                                                        \
    EACP_RELOADABLE_ENTRY_BEGIN                                                     \
    EACP_HOT_RELOAD_EXPORT eacp::Plugins::ReloadableTable eacpReloadableTable()     \
    {                                                                               \
        static const auto entries =                                                 \
            eacp::Plugins::Detail::reloadableEntries(__VA_ARGS__);                  \
        return {entries.data(), static_cast<int>(entries.size())};                  \
    }                                                                               \
    EACP_RELOADABLE_ENTRY_END
