#pragma once

#include "DynamicLibrary.h"

namespace eacp::Plugins
{
// A DynamicLibrary over a module file that is rebuilt while the process runs.
//
// reload() opens a fresh copy of builtPath staged under its own name in
// stagingDirectory, never builtPath itself: a new path per load sidesteps
// DynamicLibrary's path-keyed registry (which would hand back the old image)
// and Windows' lock on a loaded DLL, which the linker must be free to
// overwrite.
//
// Every pointer obtained through findFunction/findSymbol, and anything a
// function in the image returned, is invalid after the next successful
// reload(): copy out what you need first. Main thread only.
//
// With a requiredSymbol, an image that does not export it is refused before
// it replaces the current one, so the previous generation stays loaded.
class ReloadableLibrary
{
public:
    ReloadableLibrary(const FilePath& builtPathToUse,
                      const FilePath& stagingDirectoryToUse,
                      const std::string& requiredSymbolToUse = {});
    ~ReloadableLibrary();

    ReloadableLibrary(const ReloadableLibrary&) = delete;
    ReloadableLibrary& operator=(const ReloadableLibrary&) = delete;
    ReloadableLibrary(ReloadableLibrary&&) = delete;
    ReloadableLibrary& operator=(ReloadableLibrary&&) = delete;

    // Stages a fresh copy of the built path and opens it, handing the previous
    // image to Plugins::unload. False leaves the current image untouched.
    bool reload();

    [[nodiscard]] bool isOpen() const;

    // Why the last reload() failed: the loader's message, or what went wrong
    // staging the copy. Empty after a successful reload().
    [[nodiscard]] const std::string& lastError() const;

    // 0 before the first successful reload, one more per success.
    [[nodiscard]] int getGeneration() const;

    [[nodiscard]] const FilePath& getBuiltPath() const;

    // The staged copy currently open; empty when none is.
    [[nodiscard]] const FilePath& getLoadedPath() const;

    template <typename FunctionType>
    FunctionType findFunction(const std::string& name) const
    {
        return library.findFunction<FunctionType>(name);
    }

    [[nodiscard]] void* findSymbol(const std::string& name) const;

private:
    FilePath nextStagedPath() const;
    void removeStaleCopies();

    FilePath builtPath;
    FilePath stagingDirectory;
    std::string requiredSymbol;
    FilePath loadedPath;
    DynamicLibrary library;
    Vector<FilePath> stagedCopies;
    std::string error;
    int generation = 0;
};
} // namespace eacp::Plugins
