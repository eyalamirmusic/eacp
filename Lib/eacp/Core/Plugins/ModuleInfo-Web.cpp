#include "ModuleInfo.h"

// One wasm module per page, with no file behind it and no image to load a
// second copy of eacp into.

namespace eacp::Plugins
{
FilePath getCurrentModulePath()
{
    return {};
}

std::string getModuleIdentitySuffix()
{
    return {};
}

bool isDynamicLibrary()
{
    return false;
}
} // namespace eacp::Plugins
