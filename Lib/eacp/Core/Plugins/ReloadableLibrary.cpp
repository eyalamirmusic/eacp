#include "ReloadableLibrary.h"
#include "DynamicLibraryPlatform.h"
#include "../Utils/Files.h"

#include <atomic>
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
    library.close();
    loadedPath = {};
    removeStaleCopies();
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
