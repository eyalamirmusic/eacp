#pragma once

/** @file Pimpl, the pointer-to-implementation holder. */

#include <memory>

namespace eacp
{
// Owns a T built from the constructor's arguments; move-only. Holds a
// shared_ptr, so T may stay incomplete in the header.
template <typename T>
class Pimpl
{
public:
    template <typename... Args>
    Pimpl(Args&&... args)
    {
        ptr = std::make_shared<T>(std::forward<Args>(args)...);
    }

    Pimpl(const Pimpl& other) = delete;
    Pimpl(Pimpl&& other) noexcept = default;

    Pimpl& operator=(Pimpl&&) noexcept = default;

    T* get() { return ptr.get(); }
    const T* get() const { return ptr.get(); }

    T* operator->() { return ptr.get(); }
    const T* operator->() const { return ptr.get(); }

    T& operator*() { return *ptr; }
    const T& operator*() const { return *ptr; }

private:
    std::shared_ptr<T> ptr;
};
} // namespace eacp