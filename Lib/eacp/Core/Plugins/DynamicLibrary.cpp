#include "DynamicLibrary.h"
#include "DynamicLibraryPlatform.h"

#include <map>
#include <mutex>
#include <utility>

namespace eacp::Plugins
{
namespace
{
// One process-wide entry per opened path: every DynamicLibrary on that path
// shares one OS handle, and the count tracks how many are alive. The last
// close() unloads the image and drops the entry.
struct LoadedImage
{
    void* handle = nullptr;
    int referenceCount = 0;
};

struct ImageRegistry
{
    std::mutex mutex;
    std::map<std::string, LoadedImage> images;
};

ImageRegistry& registry()
{
    static auto instance = ImageRegistry {};
    return instance;
}
} // namespace

DynamicLibrary::DynamicLibrary(const FilePath& pathToOpen)
{
    open(pathToOpen);
}

DynamicLibrary::~DynamicLibrary()
{
    close();
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
    : handle(std::exchange(other.handle, nullptr))
    , path(std::exchange(other.path, {}))
{
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept
{
    if (this != &other)
    {
        close();
        handle = std::exchange(other.handle, nullptr);
        path = std::exchange(other.path, {});
    }

    return *this;
}

bool DynamicLibrary::open(const FilePath& pathToOpen)
{
    close();

    auto guard = std::lock_guard {registry().mutex};
    auto& entry = registry().images[pathToOpen.str()];

    if (entry.handle == nullptr)
    {
        entry.handle = Detail::loadImage(pathToOpen);

        if (entry.handle == nullptr)
        {
            registry().images.erase(pathToOpen.str());
            return false;
        }
    }

    ++entry.referenceCount;
    handle = entry.handle;
    path = pathToOpen.str();
    return true;
}

void DynamicLibrary::close()
{
    if (handle == nullptr)
        return;

    {
        auto guard = std::lock_guard {registry().mutex};
        auto found = registry().images.find(path);

        if (found != registry().images.end() && --found->second.referenceCount == 0)
        {
            Detail::unloadImage(found->second.handle);
            registry().images.erase(found);
        }
    }

    handle = nullptr;
    path.clear();
}

bool DynamicLibrary::isOpen() const
{
    return handle != nullptr;
}

void* DynamicLibrary::findSymbol(const std::string& name) const
{
    if (handle == nullptr)
        return nullptr;

    return Detail::findImageSymbol(handle, name);
}
} // namespace eacp::Plugins
