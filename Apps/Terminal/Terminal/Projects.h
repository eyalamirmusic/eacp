#pragma once

#include "Config.h"

#include <string>
#include <vector>

namespace term
{
struct ProjectDir
{
    std::string path;
    std::string name;
};

// Depth-1 directories under each configured search dir (the sessionizer
// scan), deduped, hidden dirs skipped. The search dirs themselves are
// included too, so ~/.config-style targets stay reachable.
std::vector<ProjectDir> scanProjects(const AppConfig& config);

// Session name for a project path: basename with dots swapped for
// underscores, matching the tmux-sessionizer convention.
std::string sessionNameFor(const std::string& path);
} // namespace term
