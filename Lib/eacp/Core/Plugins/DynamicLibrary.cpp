#include "DynamicLibrary.h"
#include "DynamicLibraryPlatform.h"
#include "../Platform/ScopedAutoReleasePool.h"
#include "../Threads/EventLoop.h"
#include "../Utils/Singleton.h"

#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace eacp::Plugins
{
namespace
{
// One entry per opened path; the last close() unloads the image.
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

// Immortal because ~DynamicLibrary closes into this, and a library pinned for
// the whole process is destroyed after the registry that first created it.
ImageRegistry& registry()
{
    return Singleton::getImmortal<ImageRegistry>();
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

void unload(DynamicLibrary library, const Callback& quiesce)
{
    {
        // Drains what the teardown autoreleases before the image can go.
        auto pool = ScopedAutoReleasePool {};
        quiesce();
    }

    // App teardown: nothing left to defer to, so `library` unmaps as it goes
    // out of scope here.
    if (!Threads::isEventLoopRunning())
        return;

    // Give the loop a turn first, so work the OS queued during the teardown
    // runs while the image is still mapped.
    auto shared = std::make_shared<DynamicLibrary>(std::move(library));
    Threads::callAsync([shared] { shared->close(); });
}
} // namespace eacp::Plugins
