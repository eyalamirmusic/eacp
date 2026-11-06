#pragma once
/** @file
    The module and build command an app's hot reload rebuilds, as plain strings.
*/

#include "../Process/Process.h"

#include <string>
#include <string_view>

namespace eacp::Plugins
{
/// Filled in by eacp_add_hot_reload through compile definitions; see
/// hotReloadTarget().
struct HotReloadTarget
{
    std::string_view library;
    std::string_view cmake;
    std::string_view buildDirectory;
    std::string_view config;
    std::string_view target;
    std::string_view sourceDirectory;

    /// The module's own source files, absolute and separated by '|'. Empty
    /// watches the whole source directory instead.
    std::string_view sources;
};

/// `cmake --build <dir> --config <config> --target <target>` for the target.
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
