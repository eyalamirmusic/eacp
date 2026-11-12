#include "ModuleInfo.h"
#include "ModuleInfoPlatform.h"

#include "../Utils/Files.h"

namespace eacp::Plugins
{
namespace
{
using ClaimFunction = const void* (*) ();

bool copyIsInMainProgram()
{
    return Detail::isInMainProgram(getCurrentModuleAddress());
}

// The claim counts only when the executable itself defines it, and only for
// the image it names: a plugin inside an eacp host that claims another copy is
// still a plugin.
bool copyIsClaimedByMainProgram()
{
    auto* symbol = Detail::findClaimFunction();

    if (symbol == nullptr || !Detail::isInMainProgram(symbol))
        return false;

    const auto* claimed = reinterpret_cast<ClaimFunction>(symbol)();

    return claimed != nullptr
           && Detail::isSameImage(claimed, getCurrentModuleAddress());
}
} // namespace

bool isDynamicLibrary()
{
    static const auto result =
        !copyIsInMainProgram() && !copyIsClaimedByMainProgram();

    return result;
}

FilePath getProductModulePath()
{
    if (isDynamicLibrary() || copyIsInMainProgram())
        return getCurrentModulePath();

    return Files::executablePath();
}
} // namespace eacp::Plugins
