#include "Platform.h"
#include "../Plugins/ModuleInfo.h"

namespace eacp::Platform
{

bool isStandalone()
{
    return !isDLL();
}

bool isDLL()
{
    return Plugins::isDynamicLibrary();
}

} // namespace eacp::Platform
