#include "DynamicLibraryPlatform.h"

#include <dlfcn.h>
#include <unistd.h>

namespace eacp::Plugins::Detail
{
void* loadImage(const FilePath& path)
{
    return dlopen(path.c_str(), RTLD_LOCAL | RTLD_NOW);
}

std::string lastLoadError()
{
    auto* message = dlerror();
    return message == nullptr ? std::string {} : std::string {message};
}

long long currentProcessId()
{
    return (long long) getpid();
}

void unloadImage(void* handle)
{
    dlclose(handle);
}

void* findImageSymbol(void* handle, const std::string& name)
{
    return dlsym(handle, name.c_str());
}
} // namespace eacp::Plugins::Detail
