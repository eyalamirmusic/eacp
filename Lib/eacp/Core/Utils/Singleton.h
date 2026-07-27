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

// Constructor-arg overload. Args are forwarded to T's constructor on
// first call; subsequent calls return the same object and ignore the
// args (standard static-local semantics).
template <typename T, typename Arg, typename... Args>
T& get(Arg&& arg, Args&&... args)
{
    static T object(std::forward<Arg>(arg), std::forward<Args>(args)...);
    return object;
}

// The same, deliberately never destroyed. For a process-wide registry whose
// entries deregister themselves from a destructor: anything constructed before
// the registry is destroyed after it, so a holder that outlives it — one owned
// by another singleton, or by a namespace-scope object — would deregister into
// a destroyed container. Since first use decides construction order, and that
// order varies with which caller happens to run first, get() cannot be relied
// on here at all.
//
// The bytes are a static array rather than a heap block because eacp links
// statically into runtime-loaded plugins (eacp_add_plugin), so every loaded
// image holds its own copy of each singleton. This storage unmaps with the
// image; a heap block would outlive it on the host's heap, once per load. Only
// the pointer needs a guard, and a pointer is trivially destructible, so the
// module registers nothing for atexit and nothing of ours can fault while it
// detaches.
template <typename T>
T& getImmortal()
{
    alignas(T) static std::byte storage[sizeof(T)];
    static auto* object = new (storage) T();
    return *object;
}
} // namespace eacp::Singleton