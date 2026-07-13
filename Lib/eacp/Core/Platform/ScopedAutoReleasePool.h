#pragma once

namespace eacp
{
// An autorelease pool where the platform has one, and an empty object where it
// doesn't — so portable code can bound the lifetime of the OS's deferred
// releases without an #ifdef at the call site.
//
// On Apple platforms the frameworks hand back autoreleased objects constantly:
// they are released not when you drop them, but when the enclosing pool drains
// — normally at the end of the current run-loop iteration. Where that is too
// late, scope a pool around the work and the release lands at the end of the
// scope instead. The case that forces this is unloading a module (see
// Plugins::unload): objects whose class lives in the module must be dead
// before its image is unmapped, and "the end of the current run-loop
// iteration" may be after the unmap.
//
// Everywhere else the constructor and destructor do nothing, so the same code
// compiles and reads the same on Windows and Linux.
class ScopedAutoReleasePool
{
public:
    ScopedAutoReleasePool();
    ~ScopedAutoReleasePool();

    ScopedAutoReleasePool(const ScopedAutoReleasePool&) = delete;
    ScopedAutoReleasePool& operator=(const ScopedAutoReleasePool&) = delete;

private:
    [[maybe_unused]] void* pool = nullptr;
};
} // namespace eacp
