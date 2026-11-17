#pragma once

#include <atomic>
#include <mutex>
#include <optional>

namespace eacp
{
// A process-wide object built on first use from any thread, which can be
// released and built again. Kept in storage that is never destroyed
// (Singleton::getImmortal<ReleasableShared<T>>()), so only release() ever
// destroys the object.
//
// For what a plug-in module holds on the shared drawing devices. eacp links
// statically into plug-in modules, whose static destructors run on the host's
// FreeLibrary, under the loader lock. Releasing a device there - or the last
// object holding a reference to one - makes the driver wait for its worker
// threads to exit, and they need that lock to exit, so the host hangs.
// Graphics::releaseSharedDevices gives such objects back from the module exit
// instead, through a listener each holder registers; a module unloaded
// without that call leaks them.
template <typename T>
class ReleasableShared
{
public:
    // make(std::optional<T>&) emplaces the object, under the lock.
    template <typename Make>
    T& get(Make&& make)
    {
        if (auto* object = current.load(std::memory_order_acquire))
            return *object;

        auto lock = std::lock_guard(mutex);

        if (!storage)
        {
            make(storage);
            current.store(&*storage, std::memory_order_release);
        }

        return *storage;
    }

    T* find() const { return current.load(std::memory_order_acquire); }

    void release()
    {
        auto lock = std::lock_guard(mutex);
        current.store(nullptr, std::memory_order_release);
        storage.reset();
    }

private:
    std::mutex mutex;
    std::optional<T> storage;
    std::atomic<T*> current {nullptr};
};
} // namespace eacp
