#include "SpawnDirectory-Posix.h"

#include <dlfcn.h>

namespace eacp::Processes
{
// Bionic has the call from API 34 only, and hides its declaration below the
// API level a build targets, so it is looked up at run time.
bool addSpawnWorkingDirectory(posix_spawn_file_actions_t& actions,
                              const std::string& directory)
{
    using AddChdir = int (*)(posix_spawn_file_actions_t*, const char*);

    static auto* const addChdir = reinterpret_cast<AddChdir>(
        dlsym(RTLD_DEFAULT, "posix_spawn_file_actions_addchdir_np"));

    return addChdir != nullptr && addChdir(&actions, directory.c_str()) == 0;
}
} // namespace eacp::Processes
