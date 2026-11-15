#include "ModuleInfo.h"
#include "ModuleInfoPlatform.h"

#include "../Utils/Files.h"

// A web program is one wasm module with no dynamic linking, so this copy is
// always the application and always the one image there is.
namespace eacp::Plugins
{
namespace
{
const char moduleAnchor = 0;
} // namespace

const void* getCurrentModuleAddress()
{
    return &moduleAnchor;
}

bool Detail::isInMainProgram(const void*)
{
    return true;
}

bool Detail::isSameImage(const void*, const void*)
{
    return true;
}

void* Detail::findClaimFunction()
{
    return nullptr;
}

FilePath Detail::loadedImagePath()
{
    return Files::executablePath();
}

FilePath getCurrentModulePath()
{
    return Files::executablePath();
}

std::string getModuleIdentitySuffix()
{
    return "0";
}
} // namespace eacp::Plugins
