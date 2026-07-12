#include "ModuleInfo.h"

#include <filesystem>

namespace eacp::Plugins
{
bool isDynamicLibrary()
{
    static const auto result = []
    {
        // A PIE executable and a shared object are both ET_DYN, so the ELF
        // header can't tell them apart; compare images instead.
        auto ec = std::error_code();
        auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);

        if (ec)
            return false;

        return FilePath {exe} != getCurrentModulePath();
    }();

    return result;
}
} // namespace eacp::Plugins
