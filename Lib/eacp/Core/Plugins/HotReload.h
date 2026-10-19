#pragma once

#include "../Process/Process.h"

#include <string>
#include <string_view>

// Each app's eacp_add_hot_reload sets these on the app alone; the header is
// evaluated in the including translation unit, so eacp-core never sees them.

#ifndef EACP_HOT_RELOAD
#define EACP_HOT_RELOAD 0
#endif

#ifndef EACP_HOT_RELOAD_LIBRARY
#define EACP_HOT_RELOAD_LIBRARY ""
#endif

#ifndef EACP_HOT_RELOAD_CMAKE
#define EACP_HOT_RELOAD_CMAKE ""
#endif

#ifndef EACP_HOT_RELOAD_BUILD_DIR
#define EACP_HOT_RELOAD_BUILD_DIR ""
#endif

#ifndef EACP_HOT_RELOAD_CONFIG
#define EACP_HOT_RELOAD_CONFIG ""
#endif

#ifndef EACP_HOT_RELOAD_TARGET
#define EACP_HOT_RELOAD_TARGET ""
#endif

#ifndef EACP_HOT_RELOAD_SOURCE_DIR
#define EACP_HOT_RELOAD_SOURCE_DIR ""
#endif

namespace eacp::Plugins
{
constexpr bool isHotReload()
{
    return EACP_HOT_RELOAD != 0;
}

struct HotReloadTarget
{
    std::string_view library;
    std::string_view cmake;
    std::string_view buildDirectory;
    std::string_view config;
    std::string_view target;
    std::string_view sourceDirectory;
};

constexpr HotReloadTarget hotReloadTarget()
{
    return {EACP_HOT_RELOAD_LIBRARY,
            EACP_HOT_RELOAD_CMAKE,
            EACP_HOT_RELOAD_BUILD_DIR,
            EACP_HOT_RELOAD_CONFIG,
            EACP_HOT_RELOAD_TARGET,
            EACP_HOT_RELOAD_SOURCE_DIR};
}

inline Processes::ProcessOptions hotReloadBuildCommand(const HotReloadTarget& target)
{
    auto options = Processes::ProcessOptions {};
    options.executable = std::string {target.cmake};
    options.arguments = {"--build",
                         std::string {target.buildDirectory},
                         "--config",
                         std::string {target.config},
                         "--target",
                         std::string {target.target}};
    return options;
}
} // namespace eacp::Plugins
