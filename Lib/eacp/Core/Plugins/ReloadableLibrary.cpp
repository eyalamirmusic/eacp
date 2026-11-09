#include "ReloadableLibrary.h"
#include "DynamicLibraryPlatform.h"
#include "../Threads/EventLoop.h"
#include "../Utils/Files.h"

#include <atomic>
#include <memory>
#include <string>
#include <utility>

namespace eacp::Plugins
{
namespace
{
int nextStagingSerial()
{
    static auto serial = std::atomic<int> {0};
    return ++serial;
}

void keepMappedUntilExit(DynamicLibrary image)
{
    static auto* kept = new Vector<DynamicLibrary> {};
    kept->add(std::move(image));
}

void unmapAfterThisTurn(DynamicLibrary image, const Vector<FilePath>& stagedCopies)
{
    if (!Threads::isEventLoopRunning())
    {
        if (image.isOpen())
            keepMappedUntilExit(std::move(image));

        return;
    }

    auto shared = std::make_shared<DynamicLibrary>(std::move(image));

    auto closeAndRemoveLater = [shared, stagedCopies]
    {
        shared->close();

        for (const auto& copy: stagedCopies)
            Files::removeFile(copy);
    };

    Threads::callAsync(closeAndRemoveLater);
}
} // namespace

ReloadableLibrary::ReloadableLibrary(const FilePath& builtPathToUse,
                                     const FilePath& stagingDirectoryToUse,
                                     const std::string& requiredSymbolToUse)
    : builtPath(builtPathToUse)
    , stagingDirectory(stagingDirectoryToUse)
    , requiredSymbol(requiredSymbolToUse)
{
}

ReloadableLibrary::~ReloadableLibrary()
{
    loadedPath = {};
    removeStaleCopies();
    unmapAfterThisTurn(std::move(library), stagedCopies);
}

FilePath ReloadableLibrary::nextStagedPath() const
{
    auto name = builtPath.stem() + "-" + std::to_string(generation + 1) + "-"
                + std::to_string(Detail::currentProcessId()) + "-"
                + std::to_string(nextStagingSerial()) + builtPath.extension();

    return stagingDirectory / name;
}

bool ReloadableLibrary::reload()
{
    error.clear();

    if (!Files::isFile(builtPath))
    {
        error = "no module at " + builtPath.str();
        return false;
    }

    auto staged = nextStagedPath();
    Files::removeFile(staged);

    if (!Files::copyFile(builtPath, staged))
    {
        error = "could not stage " + staged.str();
        return false;
    }

    auto fresh = DynamicLibrary {};

    if (!fresh.open(staged))
    {
        error = fresh.lastError();
        Files::removeFile(staged);
        return false;
    }

    if (!requiredSymbol.empty() && fresh.findSymbol(requiredSymbol) == nullptr)
    {
        error = "no " + requiredSymbol;
        fresh.close();
        Files::removeFile(staged);
        return false;
    }

    if (auto refusal = validate(fresh); !refusal.empty())
    {
        error = refusal;
        fresh.close();
        Files::removeFile(staged);
        return false;
    }

    stagedCopies.add(staged);

    if (library.isOpen())
        unload(std::move(library));

    library.open(staged);
    loadedPath = staged;
    ++generation;

    removeStaleCopies();
    return true;
}

void ReloadableLibrary::removeStaleCopies()
{
    auto removeUnlessLoaded = [this](const FilePath& copy)
    {
        if (copy == loadedPath)
            return false;

        Files::removeFile(copy);
        return !Files::exists(copy);
    };

    stagedCopies.eraseIf(removeUnlessLoaded);
}

bool ReloadableLibrary::isOpen() const
{
    return library.isOpen();
}

const std::string& ReloadableLibrary::lastError() const
{
    return error;
}

int ReloadableLibrary::getGeneration() const
{
    return generation;
}

const FilePath& ReloadableLibrary::getBuiltPath() const
{
    return builtPath;
}

const FilePath& ReloadableLibrary::getLoadedPath() const
{
    return loadedPath;
}

void* ReloadableLibrary::findSymbol(const std::string& name) const
{
    return library.findSymbol(name);
}
} // namespace eacp::Plugins
