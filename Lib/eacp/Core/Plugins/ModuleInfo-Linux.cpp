#include "ModuleInfo.h"

#include "../Utils/StdPath.h"

#include <filesystem>

namespace eacp::Plugins
{
bool isDynamicLibrary()
{
    static const auto result = []
    {
        // A PIE executable and a shared object are both ET_DYN, so the ELF
        // header can't tell them apart; compare images instead -- resolved on
        // both sides, because /proc/self/exe always is and the path dladdr
        // reports is the one the program was invoked with. Launched as `./app`
        // they differ, and a standalone app taken for a plugin returns from
        // run<T>() without ever starting its event loop.
        auto ec = std::error_code();
        const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);

        if (ec)
            return false;

        const auto module = toStdPath(getCurrentModulePath());

        if (module.empty())
            return false;

        const auto resolved = std::filesystem::weakly_canonical(module, ec);

        // Unresolvable is answered the same way an unreadable /proc/self/exe
        // is, above: standalone.
        if (ec)
            return false;

        return FilePath {exe} != FilePath {resolved};
    }();

    return result;
}
} // namespace eacp::Plugins
