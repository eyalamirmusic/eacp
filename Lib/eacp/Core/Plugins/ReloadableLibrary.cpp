#include "ReloadableLibrary.h"
#include "DynamicLibraryPlatform.h"
#include "../Platform/Platform.h"
#include "../Threads/EventLoop.h"
#include "../Utils/Files.h"
#include "../Utils/Strings.h"

#include <array>
#include <atomic>
#include <optional>
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

// Each generation is staged in a directory of its own, under the built file's
// own name, so whatever sits beside the image (a debugger's symbol file, say)
// can be staged beside it under its own name too.
FilePath stagingFolderOf(const FilePath& staged)
{
    return staged.parentDirectory();
}

// The image first: Windows refuses to delete one that is still mapped, and
// then its folder stays too, for a later attempt.
bool removeStaged(const FilePath& staged)
{
    Files::removeFile(staged);

    if (Files::exists(staged))
        return false;

    Files::removeAll(stagingFolderOf(staged));
    return true;
}

// The process id in <stem>-<generation>-<pid>-<serial>, the folder
// nextStagedPath names, or nothing for any other name.
std::optional<long long> stagingFolderProcess(std::string_view folder,
                                              const std::string& stem)
{
    if (!folder.starts_with(stem + "-"))
        return std::nullopt;

    folder.remove_prefix(stem.size() + 1);
    auto numbers = std::array<std::optional<long long>, 3> {};

    for (auto part = std::size_t {0}; part < numbers.size(); ++part)
    {
        auto dash = folder.find('-');
        auto isLast = part + 1 == numbers.size();

        if (isLast != (dash == std::string_view::npos))
            return std::nullopt;

        numbers[part] = Strings::tryParse<long long>(folder.substr(0, dash));

        if (!numbers[part])
            return std::nullopt;

        folder.remove_prefix(isLast ? folder.size() : dash + 1);
    }

    return numbers[1];
}

bool belongsToAnotherRunningCopy(long long processId)
{
    return processId != Detail::currentProcessId()
           && Detail::isProcessRunning(processId);
}

// Windows cannot delete a loaded image, so whatever a run still had loaded when
// it ended stays behind. A folder named for another running process is left
// alone: that copy may have staged it and not loaded it yet. Elsewhere a loaded
// file can be deleted, and every copy is removed on the turn its image is
// unloaded.
void removeCopiesLeftByEarlierRuns(const FilePath& builtPath,
                                   const FilePath& stagingDirectory)
{
    if constexpr (!Platform::isWindows())
        return;

    auto stem = builtPath.stem();

    for (const auto& entry: Files::listDirectory(stagingDirectory))
    {
        if (entry.kind != Files::EntryKind::directory)
            continue;

        auto process = stagingFolderProcess(entry.path.fileName(), stem);

        if (process && !belongsToAnotherRunningCopy(*process))
            removeStaged(entry.path / builtPath.fileName());
    }
}

// A debugger reads the symbols beside the image it loaded: a module linked with
// /PDBALTPATH:%_PDB% names only its PDB's file, so the staged copy, under that
// file name, is what it opens, and the linker stays free to rewrite the built
// one (LNK1201).
void stageSymbolsBeside(const FilePath& symbols, const FilePath& staged)
{
    if (Files::isFile(symbols))
        Files::copyFile(symbols, staged.parentDirectory() / symbols.fileName());
}

bool isMissingSymbolError(const std::string& error)
{
    for (auto marker: {"undefined symbol", "symbol not found", "Symbol not found"})
        if (error.find(marker) != std::string::npos)
            return true;

    return error.starts_with("error 127:");
}

// Queued behind unload's own close, so the folder goes as soon as the image is
// unmapped, which on Windows is the first moment it can.
void unloadAndRemove(DynamicLibrary image, const FilePath& staged)
{
    unload(std::move(image));

    if (Threads::isEventLoopRunning())
        Threads::callAsync([staged] { removeStaged(staged); });
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
            removeStaged(copy);
    };

    Threads::callAsync(closeAndRemoveLater);
}
} // namespace

std::string Detail::explainedLoadError(const std::string& loaderError)
{
    if (!isMissingSymbolError(loaderError))
        return loaderError;

    return loaderError
           + " (the module calls code this running app does not contain: eacp or "
             "the app changed since it started, so restart the app)";
}

ReloadableLibrary::ReloadableLibrary(const FilePath& builtPathToUse,
                                     const FilePath& stagingDirectoryToUse,
                                     const std::string& requiredSymbolToUse)
    : builtPath(builtPathToUse)
    , stagingDirectory(stagingDirectoryToUse)
    , requiredSymbol(requiredSymbolToUse)
{
    removeCopiesLeftByEarlierRuns(builtPath, stagingDirectory);
}

ReloadableLibrary::~ReloadableLibrary()
{
    loadedPath = {};
    removeStaleCopies();
    unmapAfterThisTurn(std::move(library), stagedCopies);
}

FilePath ReloadableLibrary::nextStagedPath() const
{
    auto folder = builtPath.stem() + "-" + std::to_string(generation + 1) + "-"
                  + std::to_string(Detail::currentProcessId()) + "-"
                  + std::to_string(nextStagingSerial());

    return stagingDirectory / folder / builtPath.fileName();
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
    removeStaged(staged);

    if (!Files::copyFile(builtPath, staged))
    {
        removeStaged(staged);
        error = "could not stage " + staged.str();
        return false;
    }

    stageSymbolsBeside(
        symbolsPath.empty() ? builtPath.withExtension(".pdb") : symbolsPath, staged);

    auto fresh = DynamicLibrary {};

    if (!fresh.open(staged))
    {
        error = Detail::explainedLoadError(fresh.lastError());
        removeStaged(staged);
        return false;
    }

    if (!requiredSymbol.empty() && fresh.findSymbol(requiredSymbol) == nullptr)
    {
        error = "no " + requiredSymbol;
        fresh.close();
        removeStaged(staged);
        return false;
    }

    if (auto refusal = validate(fresh); !refusal.empty())
    {
        error = refusal;
        fresh.close();
        removeStaged(staged);
        return false;
    }

    stagedCopies.add(staged);

    if (library.isOpen())
        unloadAndRemove(std::move(library), loadedPath);

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

        return removeStaged(copy);
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
