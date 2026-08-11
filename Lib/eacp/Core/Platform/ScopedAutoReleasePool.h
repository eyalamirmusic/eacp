#pragma once

namespace eacp
{
// Drains the OS's deferred releases at the end of this scope instead of at the
// end of the run-loop iteration. Does nothing off Apple platforms, so portable
// code can scope one without an #ifdef.
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
