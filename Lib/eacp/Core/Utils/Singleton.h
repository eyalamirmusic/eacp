#pragma once

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
// on here at all. Leaking one object at exit is the cheaper end of the trade.
template <typename T>
T& getImmortal()
{
    static auto* object = new T();
    return *object;
}
} // namespace eacp::Singleton