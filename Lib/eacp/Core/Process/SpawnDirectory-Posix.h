#pragma once

#include <spawn.h>
#include <string>

namespace eacp::Processes
{
bool addSpawnWorkingDirectory(posix_spawn_file_actions_t& actions,
                              const std::string& directory);
} // namespace eacp::Processes
