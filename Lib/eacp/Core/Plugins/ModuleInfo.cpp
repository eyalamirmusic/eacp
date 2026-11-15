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

const void* findImageClaimedByMainProgram()
{
    auto* claim = Detail::findClaimFunction();

    if (claim == nullptr || !Detail::isInMainProgram(claim))
        return nullptr;

    return reinterpret_cast<ClaimFunction>(claim)();
}

bool copyIsClaimedByMainProgram()
{
    const auto* claimed = findImageClaimedByMainProgram();

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
