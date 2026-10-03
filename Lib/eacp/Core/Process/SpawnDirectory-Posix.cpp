#include "SpawnDirectory-Posix.h"

namespace eacp::Processes
{
bool addSpawnWorkingDirectory(posix_spawn_file_actions_t& actions,
                              const std::string& directory)
{
    // _np is the only variant available at our deployment target; the
    // non-suffixed addchdir is macOS 26+ only.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    return posix_spawn_file_actions_addchdir_np(&actions, directory.c_str()) == 0;
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
}
} // namespace eacp::Processes
