#pragma once

#include "DynamicLibrary.h"

namespace eacp::Plugins
{
namespace Detail
{
// The loader's message, with the advice to restart where it names a symbol
// the running process does not have.
std::string explainedLoadError(const std::string& loaderError);
} // namespace Detail

// A DynamicLibrary over a module file that is rebuilt while the process runs.
//
// reload() opens a fresh copy of builtPath, never builtPath itself, staged
// under the built file's own name in a folder of its own inside
// stagingDirectory (<stem>-<generation>-<pid>-<serial>): a new path per load
// sidesteps DynamicLibrary's path-keyed registry (which would hand back the
// old image) and Windows' lock on a loaded DLL, which the linker must be free
// to overwrite. A PDB beside the built file is staged beside the copy, so a
// debugger never holds the one the next link writes. A staged copy is removed
// with its folder. On Windows, where a run that ends with an image loaded
// leaves its folder behind, construction removes the folders earlier runs left
// for the same module; one whose image another running copy has loaded will
// not delete, and stays.
//
// Every pointer obtained through findFunction/findSymbol, and anything a
// function in the image returned, is invalid after the next successful
// reload(): copy out what you need first. Main thread only.
//
// With a requiredSymbol, an image that does not export it is refused before
// it replaces the current one, so the previous generation stays loaded. So is
// an image `validate` answers with a non-empty reason, which becomes
// lastError().
//
// Destruction never unmaps the current image there and then: it goes on a
// later loop turn, or, with no loop running, stays mapped until the process
// exits. Objects the image made may therefore outlive this library for the
// rest of the current turn, as members destroyed after its owner do. A staged
// copy Windows will not delete while it is loaded goes on that later turn too,
// and with no loop running stays in the staging directory.
//
// A module that fails to load because it calls a function the running process
// lacks (the loader's "undefined symbol" or "symbol not found", Windows' error
// 127) says so in lastError(), and that only restarting the app helps.
class ReloadableLibrary
{
public:
    using ValidateFunc = std::function<std::string(const DynamicLibrary& image)>;

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

    ValidateFunc validate = [](const DynamicLibrary&) { return std::string {}; };

    // The built module's symbol file, staged beside each copy under its own
    // file name. Empty means builtPath with the extension .pdb.
    FilePath symbolsPath;

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
