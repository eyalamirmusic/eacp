#pragma once

#include "ShaderLibrary.h"

#include <functional>
#include <memory>

namespace eacp::GPU
{
class Device;

// The libraries programs compiled on one Device, shared by what decides the
// compiled output: the backend, the source text, its entry points, its thread
// group, whether it declares pipeline constants, the precompiled-shader
// configuration and, on D3D12, the DXC one. ShaderProgram::prepare and
// ComputeProgram::prepare take their library from here, so a second prepare, a
// second instance or a second window compiles nothing, and a ShaderWarmUp can
// compile ahead on a worker thread for them to find.
//
// One per Device (Device::perDevice), destroyed with it. Libraries are only
// ever released on the thread that asks through acquire() - the Device's
// owning one - or with the cache itself, never by warm(): destroying a Vulkan
// library defers its release into a list the owning thread drains unlocked.
class ShaderLibraryCache
{
public:
    using LibraryPointer = std::shared_ptr<const ShaderLibrary>;

    struct Stats
    {
        // Libraries compiled (or loaded precompiled) through the cache, failed
        // ones included.
        int compiles = 0;

        // Asks answered by a library already there, without compiling.
        int hits = 0;

        // Asks that found the library in flight on another thread and waited
        // for it rather than compiling it twice. Counted in hits as well when
        // what they waited for was usable.
        int waits = 0;

        // Libraries held now.
        int entries = 0;
    };

    // What warm() left in the cache for one source.
    struct Warmed
    {
        bool valid = false;
        bool precompiled = false;

        // False when the library was already there or in flight elsewhere.
        bool compiledNow = false;

        CompilationInfo compilation;
    };

    ShaderLibraryCache();
    ~ShaderLibraryCache();

    ShaderLibraryCache(const ShaderLibraryCache&) = delete;
    ShaderLibraryCache& operator=(const ShaderLibraryCache&) = delete;

    static ShaderLibraryCache& of(Device& device);

    // The library for `source`: the cached one, the one another thread is
    // compiling (waited for), or one compiled now and kept. A library that
    // failed is compiled again when it is asked for, so its messages are as
    // fresh as a direct compile's; one compiled before the backend device was
    // rebuilt (Device::backendGeneration) is dropped and compiled again. On the
    // Device's owning thread.
    LibraryPointer acquire(Device& device, const ShaderSource& source);

    // Compiles `source` into the cache unless it is there or in flight, which
    // it waits for instead. From any thread; holds no library outside the
    // cache's lock, so it never destroys one. `whileInFlight` runs after the
    // entry is claimed and before the compile, with nothing locked.
    Warmed warm(
        Device& device,
        const ShaderSource& source,
        const std::function<void()>& whileInFlight = [] {});

    // Takes up the backend generation `device` is at now, dropping libraries
    // from an older one. warm() stamps what it compiles with the generation
    // the cache last followed - reading the device's own is not safe off its
    // owning thread on D3D12 - and a cache that has followed none stamps a
    // generation no device has, which acquire() drops. So call this on the
    // Device's owning thread before warming; ShaderWarmUp::start() does, and
    // every acquire() does it first.
    void followBackendGeneration(Device& device);

    // The usable library for `source` - finished, valid, and of the generation
    // followed last - or null. The caller shares it and may be the one to
    // release it, so off the owning thread only where that is free-threaded:
    // D3D12, whose libraries are bytecode blobs, where a ShaderWarmUp's worker
    // builds pipelines from it; never Vulkan.
    LibraryPointer find(const ShaderSource& source) const;

    // Whether a finished library for `source` is held, failed or not.
    bool contains(const ShaderSource& source) const;

    // Whether `source` is being compiled right now.
    bool isInFlight(const ShaderSource& source) const;

    Stats stats() const;

private:
    struct State;
    Pimpl<State> state;
};
} // namespace eacp::GPU
