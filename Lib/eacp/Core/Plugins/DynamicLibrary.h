#pragma once

#include <eacp/Core/Utils/Containers.h>
#include <string>

namespace eacp::Plugins
{
// RAII handle to a runtime-loaded library (dlopen/LoadLibrary). Symbols are
// bound locally and resolved eagerly (RTLD_LOCAL | RTLD_NOW), so a module's
// internals never enter the global symbol namespace — several modules that
// each statically link their own eacp copy can coexist in one process.
class DynamicLibrary
{
public:
    DynamicLibrary() = default;
    explicit DynamicLibrary(const std::string& path);
    ~DynamicLibrary();

    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;

    bool open(const std::string& path);
    void close();

    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] void* findSymbol(const std::string& name) const;

    template <typename FunctionType>
    FunctionType findFunction(const std::string& name) const
    {
        return reinterpret_cast<FunctionType>(findSymbol(name));
    }

    // Names of the functions this library exports, ready to pass back into
    // findSymbol/findFunction (the Mach-O leading underscore is stripped).
    // Implemented via the dyld symbol table on macOS and the PE export
    // directory on Windows; Linux returns an empty list for now.
    [[nodiscard]] Vector<std::string> getFunctionNames() const;

private:
    void* handle = nullptr;
};
} // namespace eacp::Plugins
