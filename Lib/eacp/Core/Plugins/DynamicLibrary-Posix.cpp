#include "DynamicLibrary.h"

#include <dlfcn.h>
#include <utility>

namespace eacp::Plugins
{
DynamicLibrary::DynamicLibrary(const std::string& path)
{
    open(path);
}

DynamicLibrary::~DynamicLibrary()
{
    close();
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
    : handle(std::exchange(other.handle, nullptr))
{
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept
{
    if (this != &other)
    {
        close();
        handle = std::exchange(other.handle, nullptr);
    }

    return *this;
}

bool DynamicLibrary::open(const std::string& path)
{
    close();

    // RTLD_NODELETE: close() releases the handle but the image is never
    // unmapped. A plugin's runtime-registered ObjC classes (and pending
    // CoreAnimation work referencing them) outlive dlclose, so unmapping the
    // code they point into crashes the process later — the same reason JUCE
    // hosts and VST3 never unload plugin images on macOS.
    handle = dlopen(path.c_str(), RTLD_LOCAL | RTLD_NOW | RTLD_NODELETE);
    return handle != nullptr;
}

void DynamicLibrary::close()
{
    if (handle != nullptr)
    {
        dlclose(handle);
        handle = nullptr;
    }
}

bool DynamicLibrary::isOpen() const
{
    return handle != nullptr;
}

void* DynamicLibrary::findSymbol(const std::string& name) const
{
    if (handle == nullptr)
        return nullptr;

    return dlsym(handle, name.c_str());
}
} // namespace eacp::Plugins
