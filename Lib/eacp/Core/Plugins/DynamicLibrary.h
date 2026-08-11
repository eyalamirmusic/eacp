#pragma once

#include "../Utils/Common.h"
#include "../Utils/FilePath.h"

namespace eacp::Plugins
{
// RAII shared handle to a runtime-loaded library, bound locally and eagerly
// (RTLD_LOCAL | RTLD_NOW). Instances on one path share a refcounted image: it
// stays mapped until the last close(), which must prove the module quiescent.
class DynamicLibrary
{
public:
    DynamicLibrary() = default;
    explicit DynamicLibrary(const FilePath& path);
    ~DynamicLibrary();

    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;

    bool open(const FilePath& path);
    void close();

    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] void* findSymbol(const std::string& name) const;

    template <typename FunctionType>
    FunctionType findFunction(const std::string& name) const
    {
        return reinterpret_cast<FunctionType>(findSymbol(name));
    }

    // Ready to pass back into findSymbol (the Mach-O leading underscore is
    // stripped). Linux returns an empty list for now.
    [[nodiscard]] Vector<std::string> getFunctionNames() const;

private:
    void* handle = nullptr;
    std::string path;
};

// Tears a module down in the order the platform requires: `quiesce` destroys
// what the module owns, deferred releases drain, then the image unmaps on a
// later loop turn. UI thread only; unmaps only at the last handle on the path.
void unload(DynamicLibrary library, const Callback& quiesce = [] {});
} // namespace eacp::Plugins
