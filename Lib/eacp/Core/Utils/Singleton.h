#pragma once

#include <cstddef>
#include <new>
#include <utility>

namespace eacp::Singleton
{
template <typename T>
T& get()
{
    static T object;
    return object;
}

// Args are used on the first call only, per static-local semantics.
template <typename T, typename Arg, typename... Args>
T& get(Arg&& arg, Args&&... args)
{
    static T object(std::forward<Arg>(arg), std::forward<Args>(args)...);
    return object;
}

// Deliberately never destroyed, for a registry whose entries deregister from
// their own destructors and can outlive it. Static storage, not the heap: it
// unmaps with a plugin's image and registers nothing for atexit.
template <typename T>
T& getImmortal()
{
    alignas(T) static std::byte storage[sizeof(T)];
    static auto* object = new (storage) T();
    return *object;
}
} // namespace eacp::Singleton