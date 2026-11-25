#include "DynamicLibrary.h"
#include "DynamicLibraryPlatform.h"

#include <unistd.h>

namespace eacp::Plugins
{
Vector<std::string> DynamicLibrary::getFunctionNames() const
{
    return {};
}

namespace Detail
{
void* loadImage(const FilePath&)
{
    return nullptr;
}

std::string lastLoadError()
{
    return "dynamic libraries cannot be loaded on the web";
}

long long currentProcessId()
{
    return (long long) getpid();
}

void unloadImage(void*) {}

void* findImageSymbol(void*, const std::string&)
{
    return nullptr;
}
} // namespace Detail
} // namespace eacp::Plugins
