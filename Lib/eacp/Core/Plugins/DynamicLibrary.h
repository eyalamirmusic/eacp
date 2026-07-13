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

// Tears a module down and unmaps it, in the order the platform requires.
//
// Unmapping is only safe once nothing in the process still points into the
// module's code, and the OS holds pointers you didn't hand it: an autoreleased
// window whose class the module registered, a block already queued on the loop,
// a message posted to a window whose WndProc lives in the module. All of them
// come due *later* than the code that created them, and every one of them is a
// jump into unmapped memory if the image is gone by then.
//
// So this runs the teardown in three beats:
//
//   1. `quiesce` — the caller destroys what the module owns (its windows, its
//      threads, its objects). Usually a call to an exported shutdown function.
//   2. drain — the platform finishes the teardown it defers (on Apple, the
//      autorelease pool scoped around step 1).
//   3. unmap — on a later loop turn when a loop is running, so anything the
//      OS queued during step 1 has already run; immediately when there is no
//      loop left to defer to (app teardown), where step 2 is all we get.
//
// Takes the library by value: hand it over with std::move and the caller
// cannot be left holding a handle to an image that is about to go.
//
//     Plugins::unload(std::move(library), [shutdown] { shutdown(); });
//
// Must be called on the UI thread. The image is unmapped only if this was the
// last DynamicLibrary on that path (see the class above) — a module someone
// else still holds open stays mapped, and only their close drops it.
void unload(DynamicLibrary library, const Callback& quiesce = [] {});
} // namespace eacp::Plugins
