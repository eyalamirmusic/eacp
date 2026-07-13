#pragma once

#include "../Utils/Common.h"
#include "../Utils/FilePath.h"

namespace eacp::Plugins
{
// RAII shared handle to a runtime-loaded library (dlopen/LoadLibrary).
// Symbols are bound locally and resolved eagerly (RTLD_LOCAL | RTLD_NOW), so
// a module's internals never enter the global symbol namespace — several
// modules that each statically link their own eacp copy can coexist in one
// process.
//
// Instances on the same path share one loaded image through a process-wide
// refcounted registry: the image stays mapped while any of them is alive,
// and the last close() (or destruction) unloads it, running the module's
// static teardown (ObjC::RuntimeClass registrations dispose on the way out).
// A host that must keep a module resident for the whole process — pending
// native callbacks can outlive a close, the reason JUCE hosts and VST3 never
// unload plugin images — expresses that by keeping an instance alive.
// Whoever closes the last instance mid-process owns proving the module
// quiescent first: no windows, no timers, no callbacks still queued into its
// code.
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

    // Names of the functions this library exports, ready to pass back into
    // findSymbol/findFunction (the Mach-O leading underscore is stripped).
    // Implemented via the dyld symbol table on macOS and the PE export
    // directory on Windows; Linux returns an empty list for now.
    [[nodiscard]] Vector<std::string> getFunctionNames() const;

private:
    void* handle = nullptr;
    std::string path;
};
} // namespace eacp::Plugins
