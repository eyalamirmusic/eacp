#pragma once

#include "../Process/Process.h"

#include <string>
#include <string_view>

namespace eacp::Plugins
{
struct HotReloadTarget
{
    std::string_view library;
    std::string_view cmake;
    std::string_view buildDirectory;
    std::string_view config;
    std::string_view target;
    std::string_view sourceDirectory;

    // The module's own source files, absolute and separated by '|'. Empty
    // watches the whole source directory instead.
    std::string_view sources;

    // A file of NAME=value lines the build runs with on top of the app's own
    // environment: on Windows, the compiler environment the build directory
    // was configured in, so an app started outside a developer prompt can
    // still build. Empty, or a file that does not exist, adds nothing.
    std::string_view buildEnvironment;

    // The module's symbol file (a PDB on Windows), staged beside each loaded
    // copy. Empty means the module's path with the extension .pdb.
    std::string_view symbols;
};

// cmake --build for the target's library alone, in the target's build
// environment, and on Windows with no console window of its own.
Processes::ProcessOptions hotReloadBuildCommand(const HotReloadTarget& target);
} // namespace eacp::Plugins
